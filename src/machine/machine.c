#include "machine.h"

#include <stdbool.h>

#include "vrEmu6502/vrEmu6502.h"
#include "bios_rom.h"
#include "serial.h"
#include "video.h"
#include "gpio.h"
#include "sound.h"
#include "storage.h"
#include "rtc.h"

// Emulated memory map (PLAN.md §3): $0000-$7FFF RAM, $8000-$9FFF IO (eight
// $400 slots), $A000-$FFFF ROM. The IO window is decoded in front of ROM, so
// only the top 24KB of the 32KB BIOS image is ever read as ROM.
#define RAM_SIZE  0x8000
#define IO_START  0x8000
#define IO_END    0x9FFF
#define ROM_BASE  0x8000 // bios_rom[] index origin; only $A000-$FFFF is read

static uint8_t ram[RAM_SIZE];
static VrEmu6502 *cpu;

static uint8_t bus_read(uint16_t addr, bool isDbg) {
    (void) isDbg;

    if (addr < IO_START) {
        return ram[addr];
    }
    if (addr <= IO_END) {
        uint16_t offset = addr - IO_START;
        uint8_t slot = offset >> 10;
        uint16_t slot_addr = offset & 0x3FF;
        switch (slot) {
            case 2: return rtc_card_read(slot_addr); // IO 3: RTC (DS1511Y)
            case 3: return storage_read(slot_addr); // IO 4: Storage (CF/IDE)
            case 4: return serial_read(slot_addr); // IO 5: Serial (6551 ACIA)
            case 5: return gpio_read(slot_addr); // IO 6: GPIO/VIA (keyboard)
            case 6: return sound_read(slot_addr); // IO 7: Sound (SID)
            case 7: return video_read(slot_addr); // IO 8: Video (TMS9918)
            default: return 0x00; // slot not present -> open bus
        }
    }
    return bios_rom[addr - ROM_BASE];
}

static void bus_write(uint16_t addr, uint8_t val) {
    if (addr < IO_START) {
        ram[addr] = val;
        return;
    }
    if (addr <= IO_END) {
        uint16_t offset = addr - IO_START;
        uint8_t slot = offset >> 10;
        uint16_t slot_addr = offset & 0x3FF;
        switch (slot) {
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
    // One-time hardware setup for the cards that own a peripheral of their
    // own: the ACIA's side-header UART and the RTC's always-on timer plus
    // its flash-backed NVRAM. (The storage card is the exception — it needs
    // the SD card mounted first, so main.c brings it up explicitly.)
    serial_init();
    rtc_card_init();

    cpu = vrEmu6502New(CPU_W65C02, bus_read, bus_write);
}

void machine_reset(void) {
    serial_reset();
    video_reset();
    gpio_reset();
    sound_reset();
    storage_reset();
    rtc_card_reset();
    vrEmu6502Reset(cpu);
}

void machine_run(uint32_t ticks) {
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
