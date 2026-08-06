// Phase 1: LCD driver — brought up here and then handed straight to the VDP
// renderer on Core 1. The test pattern, corner markers and frame-rate readout
// that verified the panel and the dirty-rect present are gone; the screen
// belongs to the emulated machine from power-on (see PLAN.md Phase 1, and
// lcd_draw_test_pattern() if it is wanted again for a bring-up).
// Phase 2: I2C1 keyboard + SD/FatFs — see kbd_and_sd_init() and PLAN.md
// Phase 2. Phase 0's USB CDC + LED heartbeat carry on.
// Phase 3: vrEmu6502 CPU + memory map, serial console bridged onto this same
// USB CDC stdio stream (see src/machine). The outer loop below is now the
// CPU tick loop rather than the physical-keyboard debug print used to verify
// Phase 2 (see PLAN.md Phase 3).
// Phase 4: TMS9918 VDP (slot 7) is now wired up, so BIOS's ProbeVideo
// succeeds and the console auto-selects video over serial. Core 1 runs the
// VDP renderer (src/machine/video_render.c) continuously; Core 0 keeps
// running the CPU tick loop below (see PLAN.md Phase 4).
// Phase 5: the physical I2C keyboard is now polled from the outer loop and
// fed into the GPIO/VIA card's emulated matrix keyboard encoder (slot 5,
// src/machine/gpio.c) so the BIOS's own IRQ-driven keyboard path is
// exercised, fully interactive with no host needed (see PLAN.md Phase 5).
// Phase 6: the SID (slot 6, src/machine/sound.c) is wired up and synthesised
// on Core 1 alongside the renderer, out to the PWM speaker pins (see
// src/machine/sound_synth.c and PLAN.md Phase 6).
// Phase 7: the CompactFlash card (slot 3, src/machine/storage.c) is backed by
// a disk image on the SD card, so BASIC's LOAD/SAVE/DIR and the Monitor's
// storage commands work and persist across power cycles (PLAN.md Phase 7).
// Phase 8: the last three probed cards. The DS1511Y RTC (slot 2,
// src/machine/rtc.c) runs off the Pico's always-on timer with its NVRAM in
// flash, the VIA's joysticks (slot 5) are driven from the PicoCalc's own
// keys, and the 6551 (slot 4) now also reaches the side-header UART pins
// (PLAN.md Phase 8).
// Phase 9: the banked expansion RAM cards (slots 0 and 1,
// src/machine/ram_bank.c) fill the last two probed slots, sized to whatever
// SRAM the target has left over (PLAN.md §4 and Phase 9).
// Phase 10: F1 opens the loader (src/launcher/launcher.c) from the keyboard
// poll below — an on-device menu that browses the SD card for programs,
// cartridges and ROMs and puts the chosen one into the machine (PLAN.md
// Phase 10).

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "lcd/lcd.h"
#include "kbd/kbd.h"
#include "launcher/launcher.h"
#include "sd/sd.h"
#include "ff.h"
#include "machine/machine.h"
#include "machine/video_render.h"
#include "machine/gpio.h"
#include "machine/sound_synth.h"
#include "machine/storage.h"
#include "machine/nvram.h"
#include "machine/ram_bank.h"
#include "machine/settings.h"

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

// Phase 11 performance pass (PLAN.md): stock clock is 125MHz on RP2040 /
// 150MHz on RP2350, and the CPU tick loop (machine_run, below) is
// unthrottled -- it already runs the emulated 6502 as fast as the interpreter
// and its per-tick card polling allow (see machine/video.h's note that
// nothing paces it to a real clock rate yet), so raising clk_sys is a direct,
// linear lever on emulated MHz. Which speed is used is a stored setting now
// (machine/settings.c), defaulting to the 200MHz step every build up to this
// one ran at unconditionally; the voltage that goes with it comes from the
// same table. If a build proves unstable on real hardware (hangs, garbled
// LCD/SD), the stock step is the one to fall back to -- there is no watchdog
// recovery from a bad overclock, which is why the table only offers speeds
// this project has actually run.

// Milestone timestamps printed over USB serial (see boot_mark() below) exist
// solely to answer "where did the 35 seconds go" empirically rather than by
// guesswork -- pull them back out once boot time is where PLAN.md Phase 11
// wants it.
static absolute_time_t boot_t0;

static void boot_mark(const char *label) {
    printf("boot: %-24s t+%lums\n", label,
           (unsigned long) (absolute_time_diff_us(boot_t0, get_absolute_time()) / 1000));
}

// Brings up the SD card, mounts it, and lists the root directory over USB
// serial — the Phase 2 "done when" SD check. The mount stays live for the
// rest of the run: the emulated CF card (Phase 7, src/machine/storage.c) is
// backed by a file on this volume. Phase 2's dump of the first file found is
// gone, since that file is now usually the multi-megabyte CF image.
static void sd_mount(void) {
    sd_bus_init();
    printf("SD: card-detect pin reads %s (polarity unverified on hardware)\n",
           sd_card_detect() ? "present" : "absent");

    static FATFS fs;
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        printf("SD: mount failed (FRESULT=%d)\n", fr);
        return;
    }
    printf("SD: mounted (%s)\n", sd_is_block_addressed() ? "SDHC/SDXC" : "SDSC/MMC");

    DIR dir;
    FILINFO fno;
    fr = f_opendir(&dir, "/");
    if (fr != FR_OK) {
        printf("SD: opendir failed (FRESULT=%d)\n", fr);
        return;
    }

    for (;;) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;
        printf("SD:  %c %8lu  %s\n", (fno.fattrib & AM_DIR) ? 'd' : '-',
               (unsigned long) fno.fsize, fno.fname);
    }
    f_closedir(&dir);
}

// Everything Core 1 owns: the SID's 44.1kHz sample interrupt (which has to
// be enabled on this core, not Core 0's real-time 6502) and then the VDP
// renderer's own never-returning frame loop.
static void core1_entry(void) {
    // Writing the RTC's NVRAM back to flash (src/machine/nvram.c) means
    // erasing a sector, which cannot happen while this core is fetching
    // instructions from flash. This opts Core 1 in to being parked for the
    // duration; without it, flash_safe_execute() refuses to run at all and
    // NVRAM would stop persisting.
    flash_safe_execute_core_init();

    sound_synth_init();
    video_render_core1_entry();
}

int main(void) {
    boot_t0 = get_absolute_time();

    // Before the clock is touched, because it decides what the clock will be.
    // Reads flash through the XIP window only, which needs nothing set up
    // first -- it is already how these very instructions are being fetched.
    settings_init();

    // Done before stdio_init_all() so the chosen clock covers the whole boot,
    // not just steady-state.
    vreg_set_voltage(settings_clock_vreg());
    sleep_ms(10); // let the regulator settle before raising the clock
    set_sys_clock_khz(settings_clock_khz(), true);

    stdio_init_all();
    boot_mark("stdio_init_all");

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Black panel until Core 1's VDP renderer paints the machine's first
    // frame, the same as a real set warming up — the machine owns the screen
    // from power-on, so nothing of the firmware's own is drawn on it.
    lcd_init();
    lcd_present_full();
    boot_mark("lcd_init");

    printf("6502-PICOCALC (board=%s, clk_sys=%lu kHz)\n", PICOCALC_BOARD,
           (unsigned long) (clock_get_hz(clk_sys) / 1000));

    kbd_init();
    boot_mark("kbd_init");

    // The mainboard's own switched power rail (LCD, keyboard controller, SD
    // slot) doesn't come up until the PicoCalc's physical power button is
    // pressed -- a human-reaction-time gap after USB power reaches the Pico
    // module itself when serial is wired through the Pico's own port rather
    // than the mainboard's. Racing that gap is what made sd_mount() below
    // fail its first attempt on real hardware every time (FRESULT=3, then a
    // lucky recovery inside FatFs's own retry). Wait for the keyboard
    // controller to ack instead -- it's on the same rail as the SD slot --
    // so mount only runs against hardware that's actually powered. A 5s cap
    // means a bare Pico with no PicoCalc board attached still boots instead
    // of hanging here forever.
    if (!kbd_wait_ready(5000)) {
        printf("kbd: controller not responding after 5s -- continuing anyway\n");
    }
    boot_mark("kbd_wait_ready");

    // Now that the controller is answering, put the panel at the stored
    // brightness rather than leaving it at whatever its own power-on default
    // happens to be.
    kbd_set_backlight(settings_backlight());

    sd_mount();
    boot_mark("sd_mount");
    // Must come after the mount above: the emulated CF card is a disk image
    // on the SD volume (PLAN.md Phase 7).
    storage_init();
    boot_mark("storage_init");

    // Core 1 renders the VDP (video.c's slot-7 register/VRAM model, fed by
    // the CPU running on Core 0 below) to the LCD continuously, and its
    // sample interrupt synthesises the SID (slot 6) to the speaker — see
    // src/machine/video_render.c, src/machine/sound_synth.c, and PLAN.md
    // Phases 4 and 6.
    multicore_launch_core1(core1_entry);

    machine_init();
    boot_mark("machine_init");

    // Narrow the expansion card to whatever the settings screen last asked
    // for. Must follow machine_init(), which is what sizes the cards to the
    // build's maximum in the first place.
    ram_bank_set_active(RAM_BANK_LOW, settings_ram_banks());

    // How much paged RAM this machine presents (PLAN.md §4). The BIOS works
    // it out for itself with ProbeRAM and shows the result in `MEM`'s HW
    // byte; this line is just so the size is visible from the host without
    // recompiling to find out.
    printf("RAM: expansion IO 1 = %u of %u banks (%u KB), IO 2 = %u banks (%u KB)\n",
           (unsigned) ram_bank_active(RAM_BANK_LOW),
           (unsigned) ram_bank_banks(RAM_BANK_LOW),
           (unsigned) (ram_bank_active(RAM_BANK_LOW) * RAM_BANK_WINDOW_SIZE / 1024),
           (unsigned) ram_bank_banks(RAM_BANK_HIGH),
           (unsigned) (ram_bank_banks(RAM_BANK_HIGH) * RAM_BANK_WINDOW_SIZE / 1024));

    // A cold start: this is the power coming on, not the reset button.
    machine_reset(true);
    boot_mark("machine_reset");

    uint32_t heartbeat = 0;
    absolute_time_t last_blink = get_absolute_time();
    absolute_time_t last_kbd_poll = get_absolute_time();

    // Backlight sleep (PLAN.md Phase 11): the panel goes dark after the
    // configured idle time and comes back on the next key. Only the backlight
    // sleeps — the 6502 keeps running, audio keeps playing, and the serial
    // console stays live, so nothing is lost by letting it happen mid-session.
    absolute_time_t last_activity = get_absolute_time();
    bool backlight_asleep = false;

    // One-shot report of when BASIC actually became ready and how fast the
    // tick loop had to run to get there, so a slow boot can be traced to
    // either a fixed startup cost above (see the boot_mark() trail) or a low
    // effective 6502 clock (PLAN.md Phase 11 wants ~1-2 MHz sustained).
    // Pull this back out once boot time is where the plan wants it.
    bool basic_reported = false;
    uint64_t run_ticks = 0;
    absolute_time_t run_t0 = get_absolute_time();

    while (true) {
        machine_run(MACHINE_TICKS_PER_BATCH);
        run_ticks += MACHINE_TICKS_PER_BATCH;

        if (!basic_reported && machine_basic_ready()) {
            basic_reported = true;
            int64_t run_us = absolute_time_diff_us(run_t0, get_absolute_time());
            uint32_t khz = run_us > 0 ? (uint32_t) ((run_ticks * 1000ull) / (uint64_t) run_us) : 0;
            boot_mark("BASIC ready");
            printf("boot: %lu ticks in %lldus -> effective %lu kHz\n",
                   (unsigned long) run_ticks, (long long) run_us, (unsigned long) khz);
        }

        // kbd_poll() blocks for its own ~16ms I2C round trip (kbd.c), so
        // gate it to roughly that cadence rather than every outer-loop
        // iteration -- otherwise it would serialize with (and dominate)
        // the CPU tick loop above. A pressed key is handed to the GPIO/VIA
        // card's matrix keyboard encoder (slot 5); the BIOS picks it up via
        // its normal CB1 IRQ handler.
        if (absolute_time_diff_us(last_kbd_poll, get_absolute_time()) >= 16000) {
            uint8_t state, code;
            if (kbd_poll_raw(&state, &code) == 0) {
                // Any event at all counts as the machine being in use —
                // presses, the repeats of a held key, and releases alike.
                if (state != 0) {
                    last_activity = get_absolute_time();
                    if (backlight_asleep) {
                        kbd_set_backlight(settings_backlight());
                        backlight_asleep = false;
                    }
                }

                int key = kbd_decode(state, code);
                // F1 belongs to the firmware rather than the machine: it opens
                // the loader, which stops the 6502 and takes the screen until
                // it is closed again (PLAN.md Phase 10). Checked after
                // kbd_decode() so that call still does its bookkeeping.
                if (launcher_hotkey(state, code)) {
                    launcher_run();
                    // The launcher runs its own key loop, so nothing has been
                    // through the check above for as long as it was open —
                    // without this the screen could go dark the instant it
                    // closes. Its settings screen may also have changed the
                    // brightness under us.
                    last_activity = get_absolute_time();
                    if (backlight_asleep) backlight_asleep = false;
                    kbd_set_backlight(settings_backlight());
                } else if (key >= 0) {
                    gpio_key_press((uint8_t) key);
                }
            }
            // The same poll tracks the keys standing in for a joystick, so
            // JOY(1) reads whichever of them are held down right now
            // (PLAN.md Phase 8).
            gpio_joystick1_set(kbd_joystick_state());
            last_kbd_poll = get_absolute_time();
        }

        // Blank the panel once it has been left alone long enough. Cheap
        // enough to check every pass, like the heartbeat below.
        uint16_t sleep_after = settings_sleep_timeout_s();
        if (!backlight_asleep && sleep_after > 0 &&
            absolute_time_diff_us(last_activity, get_absolute_time()) >=
                (int64_t) sleep_after * 1000000) {
            kbd_set_backlight(0);
            backlight_asleep = true;
        }

        // Writes the RTC's NVRAM back to flash once the machine has stopped
        // poking at it. Parks Core 1 for tens of milliseconds when it fires,
        // so it is kept out here in the outer loop rather than done from the
        // card's store handler.
        nvram_task();
        // Same idea for the firmware's own settings, which the launcher's
        // settings screen leaves dirty.
        settings_task();

        if (absolute_time_diff_us(last_blink, get_absolute_time()) >= 500000) {
            heartbeat++;
            gpio_put(PICO_DEFAULT_LED_PIN, heartbeat & 1);
            last_blink = get_absolute_time();
        }
    }
}
