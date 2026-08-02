#include "video.h"

#include <string.h>

// Frame-interrupt (vblank) timing assumes a nominal 1MHz 6502 clock, matching
// 6502-DEV's cachedCpuFrequency default. The tick loop (machine_run) isn't
// actually paced to this rate yet (see PLAN.md Phase 11), so on real hardware
// vblank IRQs and the Core-1 snapshot below refresh faster than true 60Hz —
// harmless for now (no code depends on real-time frame pacing before Phase
// 6's audio), revisit once the CPU is throttled.
#define VIDEO_CPU_HZ 1000000
#define VIDEO_FRAME_HZ 60

// R1 bit 5 = frame-interrupt enable.
#define VIDEO_R1_IE 0x20
// Status register bit 7 = frame-interrupt flag.
#define VIDEO_STATUS_IRQ 0x80

static uint8_t vram[VIDEO_VRAM_SIZE];
static uint8_t registers[8];
static uint8_t status;
static uint16_t addr;
static uint8_t latch;
static uint8_t first_byte; // 1 = expecting the first byte of a control-port latch pair
static uint8_t read_buf;

static uint32_t cycle_count;
static uint32_t cycles_per_frame;

// Snapshot handed to Core 1 (see video.h) — refreshed once per emulated frame.
static uint8_t shared_vram[VIDEO_VRAM_SIZE];
static uint8_t shared_registers[8];

void video_reset(void) {
    memset(vram, 0, sizeof(vram));
    memset(registers, 0, sizeof(registers));
    status = 0;
    addr = 0;
    latch = 0;
    first_byte = 1;
    read_buf = 0;
    cycle_count = 0;
    cycles_per_frame = VIDEO_CPU_HZ / VIDEO_FRAME_HZ;
    memset(shared_vram, 0, sizeof(shared_vram));
    memset(shared_registers, 0, sizeof(shared_registers));
}

uint8_t video_read(uint16_t slot_addr) {
    switch (slot_addr & 0x01) {
        case 0: { // data port: return buffered value, then prefetch next
            uint8_t val = read_buf;
            read_buf = vram[addr & 0x3FFF];
            addr = (addr + 1) & 0x3FFF;
            first_byte = 1;
            return val;
        }
        case 1: { // status register: read clears the frame-interrupt flag
            uint8_t s = status;
            status &= (uint8_t) ~VIDEO_STATUS_IRQ;
            first_byte = 1;
            return s;
        }
        default:
            return 0x00;
    }
}

void video_write(uint16_t slot_addr, uint8_t value) {
    switch (slot_addr & 0x01) {
        case 0: // data port: write VRAM, auto-increment
            vram[addr & 0x3FFF] = value;
            addr = (addr + 1) & 0x3FFF;
            first_byte = 1;
            break;
        case 1: // control port: two-byte latch sequence
            if (first_byte) {
                latch = value;
                first_byte = 0;
            } else {
                if (value & 0x80) {
                    // Register write: latch is the data, value bits 0-2 are the register number.
                    registers[value & 0x07] = latch;
                } else {
                    // Address set.
                    addr = (uint16_t) (latch | ((uint16_t) (value & 0x3F) << 8));
                    if (!(value & 0x40)) { // bit 6 clear = read mode: prefetch
                        read_buf = vram[addr & 0x3FFF];
                        addr = (addr + 1) & 0x3FFF;
                    }
                }
                first_byte = 1;
            }
            break;
    }
}

static void video_snapshot(void) {
    memcpy(shared_vram, vram, sizeof(shared_vram));
    memcpy(shared_registers, registers, sizeof(shared_registers));
}

uint8_t video_tick(void) {
    cycle_count++;
    if (cycle_count < cycles_per_frame) return 0x00;

    cycle_count = 0;
    status |= VIDEO_STATUS_IRQ;
    video_snapshot();

    return (registers[1] & VIDEO_R1_IE) ? 0x80 : 0x00;
}

const uint8_t *video_snapshot_vram(void) {
    return shared_vram;
}

const uint8_t *video_snapshot_registers(void) {
    return shared_registers;
}
