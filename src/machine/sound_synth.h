#ifndef MACHINE_SOUND_SYNTH_H
#define MACHINE_SOUND_SYNTH_H

// Phase 6: SID synthesiser, ported from 6502-DEV's DOB Display firmware
// (Firmware/DOB Display/src/main.cpp sidWrite/sidReset/sidISR) to run off a
// PWM wrap interrupt instead of the Teensy's IntervalTimer, writing the
// PicoCalc's PWM speaker pins instead of analogWrite(). It reads the slot-6
// card's register file directly (see sound.h) rather than receiving AV
// packets over a serial link.

// Brings up the audio PWM slice and starts the sample clock. Must be called
// from Core 1: the sample interrupt is enabled on whichever core calls this,
// and Core 0 is the hard-real-time 6502 (PLAN.md §1 core split).
void sound_synth_init(void);

#endif
