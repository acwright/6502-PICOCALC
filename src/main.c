// Phase 1: LCD driver — draws the test pattern full-screen once, then draws a
// distinct-colored box in each corner via separate dirty-rect presents to
// verify partial-window mapping, and reports the full-frame push time over
// USB CDC (see PLAN.md Phase 1).
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

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"

#include "lcd/lcd.h"
#include "kbd/kbd.h"
#include "sd/sd.h"
#include "ff.h"
#include "machine/machine.h"
#include "machine/video_render.h"
#include "machine/gpio.h"
#include "machine/sound_synth.h"
#include "machine/storage.h"
#include "machine/nvram.h"

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

#define MARK 16

// Draws one corner marker and presents just that region, so its on-screen
// position reveals how a partial-window (dirty-rect) present maps to the
// panel. Expected: RED=top-left, GREEN=top-right, BLUE=bottom-left,
// YELLOW=bottom-right.
static void present_corner(int x, int y, uint8_t color) {
    lcd_fill_rect(x, y, MARK, MARK, color);
    lcd_present();
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
    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    lcd_init();
    lcd_draw_test_pattern();

    // One full-frame push; time it to report the achievable frame rate
    // without repeatedly re-pushing (which would tear the static frame).
    absolute_time_t t0 = get_absolute_time();
    lcd_present_full();
    int64_t us = absolute_time_diff_us(t0, get_absolute_time());
    uint32_t fps = (us > 0) ? (uint32_t) (1000000 / us) : 0;

    present_corner(0, 0, LCD_COLOR_RED);                          // top-left
    present_corner(LCD_WIDTH - MARK, 0, LCD_COLOR_GREEN);         // top-right
    present_corner(0, LCD_HEIGHT - MARK, LCD_COLOR_BLUE);         // bottom-left
    present_corner(LCD_WIDTH - MARK, LCD_HEIGHT - MARK, LCD_COLOR_YELLOW); // bottom-right

    char stats[32];
    snprintf(stats, sizeof(stats), "FPS:%lu", (unsigned long) fps);
    lcd_fill_rect(24, 8, 15 * LCD_CHAR_ADVANCE_X, LCD_CHAR_ADVANCE_Y, LCD_COLOR_BLACK);
    lcd_draw_string(24, 8, stats, LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    lcd_present(); // dirty-rect: only the FPS box

    printf("6502-PICOCALC lcd test (board=%s) full-frame %lld us -> %lu fps\n",
           PICOCALC_BOARD, (long long) us, (unsigned long) fps);

    kbd_init();
    sd_mount();
    // Must come after the mount above: the emulated CF card is a disk image
    // on the SD volume (PLAN.md Phase 7).
    storage_init();

    // Core 1 renders the VDP (video.c's slot-7 register/VRAM model, fed by
    // the CPU running on Core 0 below) to the LCD continuously, and its
    // sample interrupt synthesises the SID (slot 6) to the speaker — see
    // src/machine/video_render.c, src/machine/sound_synth.c, and PLAN.md
    // Phases 4 and 6.
    multicore_launch_core1(core1_entry);

    machine_init();
    machine_reset();

    uint32_t heartbeat = 0;
    absolute_time_t last_blink = get_absolute_time();
    absolute_time_t last_kbd_poll = get_absolute_time();

    while (true) {
        machine_run(MACHINE_TICKS_PER_BATCH);

        // kbd_poll() blocks for its own ~16ms I2C round trip (kbd.c), so
        // gate it to roughly that cadence rather than every outer-loop
        // iteration -- otherwise it would serialize with (and dominate)
        // the CPU tick loop above. A pressed key is handed to the GPIO/VIA
        // card's matrix keyboard encoder (slot 5); the BIOS picks it up via
        // its normal CB1 IRQ handler.
        if (absolute_time_diff_us(last_kbd_poll, get_absolute_time()) >= 16000) {
            int key = kbd_poll();
            if (key >= 0) gpio_key_press((uint8_t) key);
            // The same poll tracks the keys standing in for a joystick, so
            // JOY(1) reads whichever of them are held down right now
            // (PLAN.md Phase 8).
            gpio_joystick1_set(kbd_joystick_state());
            last_kbd_poll = get_absolute_time();
        }

        // Writes the RTC's NVRAM back to flash once the machine has stopped
        // poking at it. Parks Core 1 for tens of milliseconds when it fires,
        // so it is kept out here in the outer loop rather than done from the
        // card's store handler.
        nvram_task();

        if (absolute_time_diff_us(last_blink, get_absolute_time()) >= 500000) {
            heartbeat++;
            gpio_put(PICO_DEFAULT_LED_PIN, heartbeat & 1);
            last_blink = get_absolute_time();
        }
    }
}
