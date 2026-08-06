#include "sound_synth.h"

#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"

#include "sound.h"

// PicoCalc PWM speaker pins. PLAN.md §2 lists AUDIO_L as GP28, but the
// mainboard actually wires the left channel to GP26 and the right to GP27
// (clockworkpi/PicoCalc: AUDIO_PIN_L / AUDIO_PIN_R), which conveniently puts
// both channels on one PWM slice. PLAN.md has been corrected to match.
#define AUDIO_PIN_L 26
#define AUDIO_PIN_R 27

#define SID_SAMPLE_RATE 44100
#define SID_CLOCK       985248 // PAL 6581

#define SID_VOICES 3

// The SID's 24-bit phase accumulator advanced at SID_CLOCK becomes a 32-bit
// accumulator advanced once per output sample:
//   phaseInc = freqReg * SID_CLOCK * 256 / SID_SAMPLE_RATE
// The reference firmware does that scale factor in float; here it is a Q16
// constant so the interrupt handler stays integer-only (no soft-float, and
// no shared hardware divider state, in an ISR).
#define PHASE_INC_Q16 ((uint32_t) (((uint64_t) SID_CLOCK * 256 * 65536) / SID_SAMPLE_RATE))

// Envelope accumulators are Q8.24: (envAccum >> 24) is the 0-255 level. A
// rate is the per-sample step that walks the full 0-255 range in `ms`
// milliseconds. Also precomputed (as compile-time constants) to keep the
// division out of the ISR.
#define ENV_RATE(ms) ((uint32_t) (((255ull << 24) * 1000ull) / ((uint64_t) (ms) * SID_SAMPLE_RATE)))

// Attack times in ms, indexed by the 4-bit attack nibble.
static const uint32_t SID_ATTACK_RATE[16] = {
    ENV_RATE(2),    ENV_RATE(8),    ENV_RATE(16),   ENV_RATE(24),
    ENV_RATE(38),   ENV_RATE(56),   ENV_RATE(68),   ENV_RATE(80),
    ENV_RATE(100),  ENV_RATE(250),  ENV_RATE(500),  ENV_RATE(800),
    ENV_RATE(1000), ENV_RATE(3000), ENV_RATE(5000), ENV_RATE(8000),
};

// Decay/release times in ms, indexed by the 4-bit decay or release nibble.
static const uint32_t SID_DECAY_RATE[16] = {
    ENV_RATE(6),    ENV_RATE(24),   ENV_RATE(48),    ENV_RATE(72),
    ENV_RATE(114),  ENV_RATE(168),  ENV_RATE(204),   ENV_RATE(240),
    ENV_RATE(300),  ENV_RATE(750),  ENV_RATE(1500),  ENV_RATE(2400),
    ENV_RATE(3000), ENV_RATE(9000), ENV_RATE(15000), ENV_RATE(24000),
};

enum { ENV_IDLE = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    uint32_t phase;          // 32-bit DDS phase accumulator
    uint32_t phase_inc;      // precomputed increment per sample
    uint32_t env_accum;      // Q8.24 envelope accumulator
    uint32_t env_attack;     // Q8.24 increment per sample during attack
    uint32_t env_decay;      // Q8.24 decrement per sample during decay
    uint32_t env_release;    // Q8.24 decrement per sample during release
    uint8_t env_sustain;     // sustain level 0-255
    uint8_t env_state;       // ENV_* above
    uint8_t gate_prev;       // previous gate bit, for edge detection
    uint16_t lfsr;           // noise LFSR
    uint8_t noise_trigger;   // previous noise clock bit
} sid_voice_t;

// Core 1's own mirror of the card's register file (sound.h). Diffing against
// it is how register writes are noticed: the Teensy build had them pushed in
// as AV packets, but here the writing core must not be made to do any work
// beyond the store itself.
static uint8_t sid[SOUND_NUM_REGISTERS];
static uint32_t sid_seq;
static sid_voice_t voices[SID_VOICES];

static uint slice_l;
static uint slice_r;
static uint16_t pwm_top;
// Maps sid_sample()'s volume-scaled mix onto the PWM duty range in a single
// multiply: level = (sample * out_scale_q16) >> 16.
static uint32_t out_scale_q16;

// Precompute a voice's phase increment from its frequency registers.
static void __not_in_flash_func(sid_update_freq)(uint8_t v) {
    uint16_t freq = (uint16_t) (sid[v * 7] | ((uint16_t) sid[v * 7 + 1] << 8));
    voices[v].phase_inc = (uint32_t) (((uint64_t) freq * PHASE_INC_Q16) >> 16);
}

// Precompute a voice's envelope rates from its attack/decay + sustain/release
// registers.
static void __not_in_flash_func(sid_update_adsr)(uint8_t v) {
    uint8_t attack = (sid[v * 7 + 5] >> 4) & 0x0F;
    uint8_t decay = sid[v * 7 + 5] & 0x0F;
    uint8_t sustain = (sid[v * 7 + 6] >> 4) & 0x0F;
    uint8_t release = sid[v * 7 + 6] & 0x0F;

    voices[v].env_attack = SID_ATTACK_RATE[attack];
    voices[v].env_decay = SID_DECAY_RATE[decay];
    voices[v].env_release = SID_DECAY_RATE[release];
    voices[v].env_sustain = (uint8_t) (sustain * 17); // 0-15 nibble -> 0-255
}

// React to one changed register (the reference firmware's sidWrite tail).
static void __not_in_flash_func(sid_apply)(uint8_t reg, uint8_t value) {
    uint8_t v = reg / 7;
    if (v >= SID_VOICES) return; // $15-$18 (filter/volume) are read straight from sid[]

    uint8_t r = reg % 7;

    if (r == 0 || r == 1) sid_update_freq(v);
    if (r == 5 || r == 6) sid_update_adsr(v);

    if (r == 4) { // control register: gate edges drive the envelope
        uint8_t gate = value & 1;
        if (gate && !voices[v].gate_prev) {
            // Rising edge: restart the oscillator and begin the attack.
            voices[v].phase = 0;
            voices[v].env_accum = 0;
            voices[v].env_state = ENV_ATTACK;
        } else if (!gate && voices[v].gate_prev) {
            // Falling edge: release from wherever the envelope had got to.
            voices[v].env_state = ENV_RELEASE;
        }
        voices[v].gate_prev = gate;
    }
}

// Pull across whatever the 6502 has written since the last sample. Registers
// are walked in index order, so a note's control register (index 4) is seen
// before its attack/decay (5/6) even though the BIOS writes them the other
// way round — harmless, since the gate edge only arms the envelope and the
// fresh rates land before the next sample is synthesised.
//
// A gate that is pulsed low and back high entirely between two samples
// (under ~23us) is missed; the BIOS and BASIC hold notes for milliseconds.
static void __not_in_flash_func(sid_sync)(void) {
    const volatile uint8_t *shared = sound_registers();

    for (uint8_t reg = 0; reg < SOUND_NUM_REGISTERS; reg++) {
        uint8_t value = shared[reg];
        if (value == sid[reg]) continue;
        sid[reg] = value;
        sid_apply(reg, value);
    }
}

// Synthesise one sample. Returns the three-voice mix already scaled by the
// master volume, i.e. 0 .. 15 * SID_VOICES * 255; the reference firmware
// divides that back down to 0-255 for its 8-bit analogWrite(), but here the
// caller folds the same gain into its PWM scaling multiply, which both keeps
// the extra PWM resolution and keeps a division out of the interrupt.
static uint32_t __not_in_flash_func(sid_sample)(void) {
    uint8_t master_vol = sid[0x18] & 0x0F;
    int32_t mix = 0;

    for (uint8_t v = 0; v < SID_VOICES; v++) {
        sid_voice_t *sv = &voices[v];
        uint8_t ctrl = sid[v * 7 + 4];

        sv->phase += sv->phase_inc;

        uint8_t wave = 0;

        if (ctrl & 0x80) {
            // Noise: clock the LFSR on the rising edge of phase bit 27 (the
            // 32-bit accumulator's stand-in for the SID's bit 19).
            uint8_t trig = (uint8_t) ((sv->phase >> 27) & 1);
            if (trig && !sv->noise_trigger) {
                uint16_t bit = (uint16_t) (((sv->lfsr >> 0) ^ (sv->lfsr >> 2) ^
                                            (sv->lfsr >> 3) ^ (sv->lfsr >> 5)) & 1);
                sv->lfsr = (uint16_t) ((sv->lfsr >> 1) | (bit << 15));
            }
            sv->noise_trigger = trig;
            wave = (uint8_t) (sv->lfsr >> 8);

        } else if (ctrl & 0x40) {
            // Pulse: compare the top 12 bits of phase against pulse width.
            uint16_t pw = (uint16_t) (sid[v * 7 + 2] | ((uint16_t) (sid[v * 7 + 3] & 0x0F) << 8));
            wave = ((sv->phase >> 20) < pw) ? 255 : 0;

        } else if (ctrl & 0x20) {
            // Sawtooth: the phase's top 8 bits directly.
            wave = (uint8_t) (sv->phase >> 24);

        } else if (ctrl & 0x10) {
            // Triangle: mirror the sawtooth over the second half of the cycle.
            wave = (sv->phase < 0x80000000u)
                       ? (uint8_t) (sv->phase >> 23)
                       : (uint8_t) ((0xFFFFFFFFu - sv->phase) >> 23);
        }

        switch (sv->env_state) {
            case ENV_ATTACK: { // ramp up to 255
                // The reference firmware adds and then tests for >= 0xFF000000,
                // which the fastest attack rates (A=0 steps by ~48.5M) can
                // jump clean over: the sum wraps past 32 bits, lands back near
                // zero, and the envelope restarts instead of settling. Catch
                // the wrap explicitly.
                uint32_t next = sv->env_accum + sv->env_attack;
                if (next < sv->env_accum || next >= 0xFF000000u) {
                    sv->env_accum = 0xFF000000u;
                    sv->env_state = (sv->env_sustain < 255) ? ENV_DECAY : ENV_SUSTAIN;
                } else {
                    sv->env_accum = next;
                }
                break;
            }
            case ENV_DECAY: // ramp down to the sustain level
                if (sv->env_accum <= sv->env_decay || (sv->env_accum >> 24) <= sv->env_sustain) {
                    sv->env_accum = (uint32_t) sv->env_sustain << 24;
                    sv->env_state = ENV_SUSTAIN;
                } else {
                    sv->env_accum -= sv->env_decay;
                }
                break;
            case ENV_SUSTAIN: // hold
                sv->env_accum = (uint32_t) sv->env_sustain << 24;
                break;
            case ENV_RELEASE: // ramp down to silence
                if (sv->env_accum <= sv->env_release) {
                    sv->env_accum = 0;
                    sv->env_state = ENV_IDLE;
                } else {
                    sv->env_accum -= sv->env_release;
                }
                break;
            default:
                break;
        }

        mix += ((int32_t) wave * (int32_t) (sv->env_accum >> 24)) >> 8;
    }

    return (uint32_t) mix * master_vol;
}

// Sample clock: fires once per PWM period, i.e. SID_SAMPLE_RATE times a
// second. Kept (with everything it calls) in RAM so Core 0's constant ROM
// reads over XIP can't stall it.
static void __not_in_flash_func(sound_pwm_irq)(void) {
    pwm_clear_irq(slice_l);

    uint32_t seq = sound_write_seq();
    if (seq != sid_seq) {
        sid_seq = seq;
        sid_sync();
    }

    uint32_t level = (sid_sample() * out_scale_q16) >> 16;
    if (level > pwm_top) level = pwm_top;

    pwm_set_gpio_level(AUDIO_PIN_L, (uint16_t) level);
    pwm_set_gpio_level(AUDIO_PIN_R, (uint16_t) level);
}

void sound_synth_init(void) {
    memset(sid, 0, sizeof(sid));
    memset(voices, 0, sizeof(voices));
    for (uint8_t v = 0; v < SID_VOICES; v++) {
        voices[v].lfsr = 0xACE1; // non-zero seed
        // Derive the rates that go with an all-zero register file, rather
        // than leaving them at zero: a zeroed rate is a step of nothing,
        // which would silently wedge a voice gated on before its ADSR
        // registers were ever written.
        sid_update_freq(v);
        sid_update_adsr(v);
    }
    sid_seq = sound_write_seq();

    gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);
    slice_l = pwm_gpio_to_slice_num(AUDIO_PIN_L);
    slice_r = pwm_gpio_to_slice_num(AUDIO_PIN_R);

    // One PWM period per output sample: the carrier sits at 44.1kHz, well
    // above hearing, and the wrap interrupt becomes a jitter-free sample
    // clock with no timer or DMA of its own. Resolution is whatever the
    // system clock affords (~2834 levels at 125MHz, better than the Teensy's
    // 8-bit analogWrite()), and is recomputed here rather than hardcoded so
    // a later overclock (PLAN.md Phase 11) stays in tune.
    uint32_t period = clock_get_hz(clk_sys) / SID_SAMPLE_RATE;
    pwm_top = (uint16_t) (period - 1);
    // sid_sample() spans 0 .. 15 (master volume) * SID_VOICES * 255 (mix).
    out_scale_q16 = (uint32_t) ((period << 16) / (15 * SID_VOICES * 255));

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, pwm_top);
    pwm_init(slice_l, &cfg, false);
    if (slice_r != slice_l) pwm_init(slice_r, &cfg, false);
    pwm_set_gpio_level(AUDIO_PIN_L, 0);
    pwm_set_gpio_level(AUDIO_PIN_R, 0);

    pwm_clear_irq(slice_l);
    pwm_set_irq_enabled(slice_l, true);
    irq_set_exclusive_handler(PWM_DEFAULT_IRQ_NUM(), sound_pwm_irq);
    irq_set_enabled(PWM_DEFAULT_IRQ_NUM(), true);

    pwm_set_enabled(slice_l, true);
    if (slice_r != slice_l) pwm_set_enabled(slice_r, true);
}
