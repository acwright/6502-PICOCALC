#include "machine.h"

#include <stdbool.h>
#include <string.h>

#include "pico.h"

#include "vrEmu6502/vrEmu6502.h"
#include "serial.h"
#include "video.h"
#include "gpio.h"
#include "sound.h"
#include "storage.h"
#include "rtc.h"
#include "ram_bank.h"
#include "media.h"

// Emulated memory map (PLAN.md §3): $0000-$7FFF RAM, $8000-$9FFF IO (eight
// $400 slots), $A000-$FFFF ROM. The IO window is decoded in front of ROM, so
// only the top 24KB of the 32KB BIOS image is ever read as ROM.
#define RAM_SIZE  0x8000
#define IO_START  0x8000
#define IO_END    0x9FFF
#define MACHINE_ROM_BASE  0x8000 // ROM image index origin; only $A000-$FFFF is read
#define CART_BASE 0xC000 // a cartridge overlays the top 16KB, Kernal and all

// BASIC's workspace pointers (BIOS.inc BAS_*), for the program-image load in
// machine_set_program_end() below.
#define BAS_TXTTAB 0x035D // start of program text
#define BAS_VARTAB 0x035F // end of program / start of variables
#define BAS_ARYTAB 0x0361 // start of arrays
#define BAS_STREND 0x0363 // end of arrays
#define BAS_MEMSIZ 0x0367 // end of usable memory
#define BAS_WARM   0x036F // warm-start magic, the last byte BasColdInit writes
#define BASIC_WARM_MAGIC 0xA5

static uint8_t ram[RAM_SIZE];

// vrEmu6502New() would malloc() this, but RP2040's heap here is only
// whatever's left after two fixed 4KB core stacks eat into 264KB of SRAM
// (PLAN.md §4 -- everything else on this target is already this tight), and
// this is the one allocation on the whole boot path: a single long-lived
// instance, never freed, so there is nothing to gain from the heap and a
// real "Out of memory" panic to lose (hit on real RP2040 hardware once this
// session's other Phase 11 changes trimmed the margin further). Static
// storage sidesteps the question entirely instead of relitigating exactly
// how much allocator overhead a 40-ish byte malloc() costs.
static uint8_t cpu_storage[64] __attribute__((aligned(8)));
static VrEmu6502 *cpu;

// What $A000-$FFFF reads, taken from machine/media.h at each reset: the
// built-in BIOS or a ROM loaded over it, and a cartridge image or NULL. Held
// as pointers rather than fetched per access so the hot bus path stays a
// straight index.
static const uint8_t *rom_image;
static const uint8_t *cart_image;

// PLAN.md Phase 11 perf pass: caching the ROM in RAM (only the top 24KB,
// $A000-$FFFF, is ever read -- $8000-$9FFF is always shadowed by the I/O
// window above) was tried and measured on real RP2040 hardware short by
// ~6KB even after giving up its expansion-RAM banks entirely (see
// ram_bank.h); closing that gap meant shrinking a core's stack sight-unseen,
// which was judged not worth risking a silent stack-overflow corruption for.
// RP2350's "plenty of spare SRAM" turned out to be stale too -- the same
// change overflowed it by ~9KB once actually tried, not the ~285KB PLAN.md
// assumed -- so this stays flash-resident on both targets rather than carry
// two different, both-tight RAM budgets. See main.c's boot_mark() trail for
// how slow that leaves boot.
static void load_rom_image(void) {
    rom_image = media_rom();
}

// RAM-resident (PLAN.md Phase 11 perf pass): every one of BASIC's own
// instruction fetches from $A000-$FFFF lands here and then in flash (rom_image
// is a const array), so this dispatch itself competing for the RP2040's 16KB
// XIP cache against that same flash traffic was pure loss -- measured on real
// hardware at ~225kHz effective 6502 clock against a ~1-2MHz target (see
// main.c's boot_mark() trail). Keeping the trampoline out of flash can't help
// the ROM reads themselves (those still miss), but it stops the two from
// evicting each other's cache lines. See sound.c's sound_registers() for the
// same reasoning applied earlier, for a different reason (ISR-callable).
static uint8_t __not_in_flash_func(bus_read)(uint16_t addr, bool isDbg) {
    (void) isDbg;

    if (addr < IO_START) {
        return ram[addr];
    }
    if (addr <= IO_END) {
        uint16_t offset = addr - IO_START;
        uint8_t slot = offset >> 10;
        uint16_t slot_addr = offset & 0x3FF;
        switch (slot) {
            case 0: return ram_bank_read(RAM_BANK_LOW, slot_addr); // IO 1: RAM bank low
            case 1: return ram_bank_read(RAM_BANK_HIGH, slot_addr); // IO 2: RAM bank high
            case 2: return rtc_card_read(slot_addr); // IO 3: RTC (DS1511Y)
            case 3: return storage_read(slot_addr); // IO 4: Storage (CF/IDE)
            case 4: return serial_read(slot_addr); // IO 5: Serial (6551 ACIA)
            case 5: return gpio_read(slot_addr); // IO 6: GPIO/VIA (keyboard)
            case 6: return sound_read(slot_addr); // IO 7: Sound (SID)
            case 7: return video_read(slot_addr); // IO 8: Video (TMS9918)
            default: return 0x00; // slot not present -> open bus
        }
    }
    // A cartridge in the slot answers for $C000-$FFFF, vectors included, so
    // the machine boots into it instead of BASIC; $A000-$BFFF stays ROM, which
    // is what leaves the Kernal and character set callable underneath it.
    if (cart_image && addr >= CART_BASE) {
        return cart_image[addr - CART_BASE];
    }
    return rom_image[addr - MACHINE_ROM_BASE];
}

static void __not_in_flash_func(bus_write)(uint16_t addr, uint8_t val) {
    if (addr < IO_START) {
        ram[addr] = val;
        return;
    }
    if (addr <= IO_END) {
        uint16_t offset = addr - IO_START;
        uint8_t slot = offset >> 10;
        uint16_t slot_addr = offset & 0x3FF;
        switch (slot) {
            case 0: ram_bank_write(RAM_BANK_LOW, slot_addr, val); break; // IO 1: RAM bank low
            case 1: ram_bank_write(RAM_BANK_HIGH, slot_addr, val); break; // IO 2: RAM bank high
            case 2: rtc_card_write(slot_addr, val); break; // IO 3: RTC (DS1511Y)
            case 3: storage_write(slot_addr, val); break; // IO 4: Storage (CF/IDE)
            case 4: serial_write(slot_addr, val); break; // IO 5: Serial
            case 5: gpio_write(slot_addr, val); break; // IO 6: GPIO/VIA
            case 6: sound_write(slot_addr, val); break; // IO 7: Sound (SID)
            case 7: video_write(slot_addr, val); break; // IO 8: Video
            default: break; // slot not present -> write is a no-op
        }
        return;
    }
    // ROM is read-only from the bus.
}

void machine_init(void) {
    // Which ROM and cartridge images flash is holding, so a cartridge chosen
    // in the launcher before the last power-off is still in the slot now.
    // machine_reset() below is what actually hands them to the bus.
    media_init();
    load_rom_image();
    cart_image = media_cart();

    // One-time hardware setup for the cards that own a peripheral of their
    // own: the ACIA's side-header UART and the RTC's always-on timer plus
    // its flash-backed NVRAM. (The storage card is the exception — it needs
    // the SD card mounted first, so main.c brings it up explicitly.)
    serial_init();
    rtc_card_init();
    // Cold start for the expansion RAM: machine_reset() below is the reset
    // button, which real SRAM survives, so the clear belongs here.
    ram_bank_init();

    // Guards cpu_storage above against the vendored struct ever growing past
    // it (e.g. a future vrEmu6502 update); a plain assert() would compile
    // away under this project's Release/NDEBUG build, silently corrupting
    // whatever follows cpu_storage in RAM, so this stays a real check.
    if (vrEmu6502Size() > sizeof(cpu_storage)) {
        panic("cpu_storage too small for VrEmu6502 (need %u, have %u)",
              (unsigned) vrEmu6502Size(), (unsigned) sizeof(cpu_storage));
    }
    cpu = vrEmu6502NewInPlace(cpu_storage, CPU_W65C02, bus_read, bus_write);
}

void machine_reset(bool cold_start) {
    // Whatever the launcher last put in the ROM socket and the cartridge slot.
    // Picked up here rather than at the moment of loading because the reset
    // vector lives in both, and the CPU only reads it on reset — swapping
    // either on real hardware means powering the machine down first.
    load_rom_image();
    cart_image = media_cart();

    serial_reset();
    video_reset();
    gpio_reset();
    sound_reset();
    storage_reset();
    rtc_card_reset();
    ram_bank_reset();

    // The one thing a reset button cannot do: SRAM only comes up empty when
    // the power has actually been off (see machine.h). At boot this is
    // redundant — .bss is already zeroed before main() — but it is what makes
    // the launcher's power cycle differ from its reset.
    if (cold_start) {
        memset(ram, 0x00, sizeof(ram));
        ram_bank_clear();
    }

    vrEmu6502Reset(cpu);
}

uint8_t machine_peek(uint16_t addr) {
    return bus_read(addr, true);
}

void machine_poke(uint16_t addr, uint8_t value) {
    bus_write(addr, value);
}

static uint16_t peek_word(uint16_t addr) {
    return (uint16_t) (machine_peek(addr) | (machine_peek((uint16_t) (addr + 1)) << 8));
}

static void poke_word(uint16_t addr, uint16_t value) {
    machine_poke(addr, (uint8_t) (value & 0xFF));
    machine_poke((uint16_t) (addr + 1), (uint8_t) (value >> 8));
}

bool machine_basic_ready(void) {
    return machine_peek(BAS_WARM) == BASIC_WARM_MAGIC &&
           peek_word(BAS_TXTTAB) == MACHINE_PROGRAM_ADDRESS &&
           peek_word(BAS_MEMSIZ) == 0x8000;
}

bool machine_set_program_end(uint16_t length) {
    if (!machine_basic_ready()) return false;

    uint16_t end = (uint16_t) (MACHINE_PROGRAM_ADDRESS + length);
    poke_word(BAS_VARTAB, end);
    poke_word(BAS_ARYTAB, end);
    poke_word(BAS_STREND, end);
    return true;
}

void __not_in_flash_func(machine_run)(uint32_t ticks) {
    vrEmu6502Interrupt *irq = vrEmu6502Int(cpu);

    for (uint32_t i = 0; i < ticks; i++) {
        vrEmu6502Tick(cpu);

        uint8_t serial_status = serial_tick();
        uint8_t video_irq = video_tick();
        uint8_t gpio_irq = gpio_tick();
        sound_tick(); // no interrupt line; drives voice 3's OSC3 readback only
        rtc_card_tick();   // no interrupt line either (see rtc.h)
        *irq = ((serial_status & 0x80) || video_irq || (gpio_irq & 0x80)) ? IntRequested : IntCleared;
    }
}
