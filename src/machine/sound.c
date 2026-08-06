#include "sound.h"

#include <string.h>

#include "hardware/sync.h"
#include "pico/platform.h"

// SID register indices used below (see BIOS.inc's SID_* equates).
#define SID_V3_FREQ_LO  0x0E
#define SID_V3_FREQ_HI  0x0F
#define SID_V3_PW_LO    0x10
#define SID_V3_PW_HI    0x11
#define SID_V3_CTRL     0x12
#define SID_PADDLE_X    0x19
#define SID_PADDLE_Y    0x1A
#define SID_OSC3        0x1B
#define SID_ENV3        0x1C

// Control-register waveform bits.
#define SID_CTRL_TRIANGLE 0x10
#define SID_CTRL_SAWTOOTH 0x20
#define SID_CTRL_PULSE    0x40
#define SID_CTRL_NOISE    0x80

// Non-zero LFSR seed, as on a real SID after reset.
#define NOISE_SEED 0x7FFFF8

static volatile uint8_t registers[SOUND_NUM_REGISTERS];
static volatile uint32_t write_seq;

static uint32_t v3_accumulator;
static uint32_t v3_noise_shift;

static void publish(void) {
    // Ensure the register byte written just above is visible before the
    // sequence number that advertises it (see sound.h).
    __dmb();
    write_seq++;
}

void sound_reset(void) {
    for (uint8_t i = 0; i < SOUND_NUM_REGISTERS; i++) registers[i] = 0;
    v3_accumulator = 0;
    v3_noise_shift = NOISE_SEED;
    publish();
}

uint8_t sound_read(uint16_t addr) {
    uint8_t reg = addr & 0x1F;

    if (reg == SID_OSC3) {
        // Live voice 3 waveform output. The BIOS's ProbeSID gates voice 3
        // into noise at full frequency, spins ~1280 cycles, and treats a
        // reading of $00 or $FF as "no SID fitted" — so this has to actually
        // move as sound_tick() runs.
        uint8_t ctrl = registers[SID_V3_CTRL];
        if (ctrl & SID_CTRL_NOISE) {
            return (uint8_t) ((v3_noise_shift >> 8) & 0xFF);
        }
        if (ctrl & SID_CTRL_SAWTOOTH) {
            return (uint8_t) ((v3_accumulator >> 16) & 0xFF);
        }
        if (ctrl & SID_CTRL_TRIANGLE) {
            uint8_t msb = (uint8_t) ((v3_accumulator >> 23) & 1);
            uint8_t upper = (uint8_t) ((v3_accumulator >> 16) & 0xFF);
            return msb ? (uint8_t) ~upper : upper;
        }
        if (ctrl & SID_CTRL_PULSE) {
            uint16_t pw = (uint16_t) (((registers[SID_V3_PW_HI] & 0x0F) << 8) | registers[SID_V3_PW_LO]);
            uint16_t phase = (uint16_t) ((v3_accumulator >> 12) & 0xFFF);
            return phase < pw ? 0xFF : 0x00;
        }
        return 0x00;
    }

    if (reg == SID_ENV3) {
        // ENV3: the envelope generators live on Core 1 (sound_synth.c) and
        // aren't read back here, same as the reference firmware.
        return 0x00;
    }

    if (reg == SID_PADDLE_X || reg == SID_PADDLE_Y) {
        return registers[reg];
    }

    return 0x00; // every other SID register is write-only
}

void sound_write(uint16_t addr, uint8_t value) {
    registers[addr & 0x1F] = value;
    publish();
}

uint8_t sound_tick(void) {
    // Advance voice 3's phase accumulator for the OSC3 readback above.
    uint16_t freq = (uint16_t) (((uint16_t) registers[SID_V3_FREQ_HI] << 8) | registers[SID_V3_FREQ_LO]);
    uint32_t prev = v3_accumulator;
    v3_accumulator = (v3_accumulator + freq) & 0xFFFFFF;

    // Clock the noise LFSR on each 0->1 transition of accumulator bit 19.
    if (!(prev & 0x080000) && (v3_accumulator & 0x080000)) {
        uint32_t bit0 = v3_noise_shift & 1;
        uint32_t bit1 = (v3_noise_shift >> 2) & 1;
        v3_noise_shift = (v3_noise_shift >> 1) | ((bit0 ^ bit1) << 22);
    }

    return 0x00;
}

// Both accessors are called from Core 1's sample interrupt (sound_synth.c),
// which is deliberately RAM-resident so Core 0's constant ROM reads over XIP
// can't stall it — so these have to be too.
const volatile uint8_t *__not_in_flash_func(sound_registers)(void) {
    return registers;
}

uint32_t __not_in_flash_func(sound_write_seq)(void) {
    return write_seq;
}
