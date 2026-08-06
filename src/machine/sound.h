#ifndef MACHINE_SOUND_H
#define MACHINE_SOUND_H

#include <stdint.h>

// Emulated MOS 6581 SID, IO slot 6 ($9800-$981C, see PLAN.md Phase 6 and
// BIOS.inc's SID_* equates). Register model ported from 6502-DEV's SoundCard
// (DB Emulator/lib/6502/IO/SoundCard.*); the AV-packet link to the Teensy's
// separate DOB Display board is dropped, so instead of streaming each
// register write over a serial link this card just publishes its register
// file for the synthesiser running on Core 1 (see src/machine/sound_synth.c).
//
// Only what the BIOS actually needs is modelled on this side: the write-only
// register file, and voice 3's phase accumulator + noise LFSR, which
// ProbeSID reads back through OSC3 ($981B) to decide whether a SID is fitted.
// Waveform synthesis proper happens on Core 1.
#define SOUND_NUM_REGISTERS 32

void sound_reset(void);

// addr is slot-local; only the low 5 bits are decoded (the SID's 29 registers
// mirror every $20 bytes across the slot, as on real hardware).
uint8_t sound_read(uint16_t addr);
void sound_write(uint16_t addr, uint8_t value);

// Call once per emulated CPU clock tick (mirrors serial_tick()/video_tick()).
// Advances voice 3's phase accumulator and clocks the noise LFSR so OSC3
// reads back live. Always returns 0x00: the SID has no interrupt line.
uint8_t sound_tick(void);

// The live register file, for Core 1's synthesiser. Single writer (Core 0,
// via sound_write()), single reader; individual bytes never tear, and
// sound_write_seq() below tells the reader when anything at all changed.
const volatile uint8_t *sound_registers(void);

// Bumped after every register write and on reset, with a barrier ahead of it
// so a reader that observes a new sequence number is guaranteed to see the
// register bytes that go with it.
uint32_t sound_write_seq(void);

#endif
