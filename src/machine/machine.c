#include "machine.h"

#include <stdbool.h>

#include "vrEmu6502/vrEmu6502.h"
#include "bios_rom.h"
#include "serial.h"
#include "video.h"
#include "gpio.h"

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
            case 4: return serial_read(slot_addr); // IO 5: Serial (6551 ACIA)
            case 5: return gpio_read(slot_addr); // IO 6: GPIO/VIA (keyboard)
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
            case 4: serial_write(slot_addr, val); break; // IO 5: Serial
            case 5: gpio_write(slot_addr, val); break; // IO 6: GPIO/VIA
            case 7: video_write(slot_addr, val); break; // IO 8: Video
            default: break; // slot not present -> write is a no-op
        }
        return;
    }
    // ROM is read-only from the bus.
}

void machine_init(void) {
    cpu = vrEmu6502New(CPU_W65C02, bus_read, bus_write);
}

void machine_reset(void) {
    serial_reset();
    video_reset();
    gpio_reset();
    vrEmu6502Reset(cpu);
}

void machine_run(uint32_t ticks) {
    vrEmu6502Interrupt *irq = vrEmu6502Int(cpu);

    for (uint32_t i = 0; i < ticks; i++) {
        vrEmu6502Tick(cpu);

        uint8_t serial_status = serial_tick();
        uint8_t video_irq = video_tick();
        uint8_t gpio_irq = gpio_tick();
        *irq = ((serial_status & 0x80) || video_irq || (gpio_irq & 0x80)) ? IntRequested : IntCleared;
    }
}
