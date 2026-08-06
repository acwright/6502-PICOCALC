#ifndef MACHINE_SETTINGS_H
#define MACHINE_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/vreg.h"

// Phase 11: the handful of firmware settings that outlive a power cycle —
// clock speed, how much of the expansion RAM card is visible, backlight
// brightness, and the idle timeout that blanks the backlight (PLAN.md
// Phase 11).
//
// Persisted the same way machine/nvram.c persists the RTC's NVRAM: a
// magic-numbered, checksummed record in a reserved flash sector, written back
// a couple of seconds after the last change rather than on every keystroke,
// so adjusting a value in the launcher doesn't erase a sector per step.
//
// This module only *stores* settings. It never sets the clock, talks to the
// keyboard controller, or resizes the RAM card — the callers that own those
// (main.c and the launcher) read the values and apply them. That keeps this a
// plain persistence module with no peripheral dependencies, exactly as
// nvram.c is.

// Reads the stored record and validates it, falling back to the defaults
// below if it is absent, corrupt, or no longer legal for this build. Must be
// called before any getter — and, because it decides the clock speed, before
// main() touches the voltage regulator or the system clock.
//
// Safe that early: it only reads flash through the XIP window, which is
// already how every instruction is being fetched by then, so no peripheral
// setup is needed. (Writing is a different matter — see settings_flush().)
void settings_init(void);

// Writes the record back if it has been dirty long enough to have settled.
// Call once per outer-loop iteration, alongside nvram_task().
void settings_task(void);

// Writes any pending change now instead of waiting for the settle period.
// Returns false if the write could not be done (Core 1 could not be parked;
// see nvram_flush(), which has the same constraint). The launcher calls this
// on the way out of the settings screen, where the machine is already stopped
// and the renderer already suspended.
bool settings_flush(void);

//
// Clock speed
//
// A short, fixed table of vetted steps rather than a free-running kHz value:
// this setting survives a power cycle, so a speed the chip cannot sustain
// would come back after every reboot, and the machine hangs before the
// launcher is reachable to change it back — with a BOOTSEL reflash the only
// way out. Both steps here are ones this project has already run on real
// hardware.
//
// A change takes effect on the next power-on, since the clock is only set
// once, at the top of main().

uint8_t settings_clock_count(void);              // number of steps in the table
uint8_t settings_clock_index(void);              // currently selected step
uint32_t settings_clock_khz(void);               // its system clock, for set_sys_clock_khz()
enum vreg_voltage settings_clock_vreg(void);     // and the core voltage that goes with it
const char *settings_clock_label(uint8_t index); // for the settings screen
void settings_set_clock_index(uint8_t index);    // clamped to the table

//
// Expansion RAM (IO 1)
//
// How many of the compiled-in banks the card actually presents. Only ever
// reduces what the build has room for — the backing array is a fixed-size
// static (see ram_bank.h's per-target budget), so this changes what the
// machine can *see*, not what the firmware allocates. IO 2 has no setting
// because no target compiles it in.

uint16_t settings_ram_banks(void);
void settings_set_ram_banks(uint16_t banks); // clamped, rounded down to a power of two

//
// Backlight
//
// 0-255, as the keyboard controller's register takes it (kbd_set_backlight()).
// Never stored as 0: a dark panel with no way to see the menu that turns it
// back up is a trap, and "off" is what the sleep timeout below is for.
#define SETTINGS_BACKLIGHT_MIN 16
#define SETTINGS_BACKLIGHT_STEP 16

uint8_t settings_backlight(void);
void settings_set_backlight(uint8_t value); // clamped to [MIN, 255]

//
// Sleep
//
// Seconds of no key activity before the backlight is switched off, or 0 to
// leave it on forever. The machine keeps running either way — this is a
// backlight blank, not a suspend; the 6502 does not stop and audio keeps
// playing. Any keypress brings the panel back at settings_backlight().

uint16_t settings_sleep_timeout_s(void);
void settings_set_sleep_timeout_s(uint16_t seconds);

// The offered timeouts, for the settings screen to cycle through.
uint8_t settings_sleep_count(void);
uint16_t settings_sleep_at(uint8_t index);
uint8_t settings_sleep_index(void);
void settings_set_sleep_index(uint8_t index);

#endif
