// Phase 2: I2C1 keyboard driver — STM32 keyboard/backlight/battery
// controller on the PicoCalc mainboard. See PLAN.md §2/Phase 2 and
// clockworkpi/PicoCalc Code/picocalc_helloworld/i2ckbd/ (protocol
// reference; this is our own port/implementation, not a copy).
#ifndef KBD_H
#define KBD_H

#include <stdbool.h>
#include <stdint.h>

// The controller's own key codes, from clockworkpi/PicoCalc's
// Code/picocalc_keyboard/keyboard.h. Anything above $7F is the controller's
// private numbering, not ASCII, and kbd_decode() either translates it to the
// byte the emulated keyboard encoder would put on the port or drops it.
#define KBD_KEY_ENTER_RAW 0x0A // controller sends LF; the machine wants CR
#define KBD_KEY_ESC_RAW   0xB1
#define KBD_KEY_LEFT_RAW  0xB4
#define KBD_KEY_UP_RAW    0xB5
#define KBD_KEY_DOWN_RAW  0xB6
#define KBD_KEY_RIGHT_RAW 0xB7
#define KBD_KEY_INSERT    0xD1
#define KBD_KEY_DEL       0xD4

// Keys the emulated machine's keyboard does not have, so kbd_decode() drops
// them and they are free for the firmware's own use: F1 opens the launcher,
// which pages its lists with Page Up/Down (src/launcher/launcher.h, PLAN.md
// Phase 10).
#define KBD_KEY_F1        0x81
#define KBD_KEY_F10       0x8A
#define KBD_KEY_PAGE_UP   0xD6
#define KBD_KEY_PAGE_DOWN 0xD7

// The controller's event states, as reported in the first byte of a FIFO
// read. Callers that want the raw stream (kbd_poll_raw()) need these to tell
// a press from the hold repeats and the release that follow it.
#define KBD_STATE_PRESS   1
#define KBD_STATE_HOLD    2
#define KBD_STATE_RELEASE 3

// The modifier keys. The controller applies Shift and Sym itself (they arrive
// as the shifted character), so only Ctrl has to be tracked here; the rest are
// swallowed, which is also what the real machine does — see the DOCS keyboard
// chapter, where Caps Lock, Menu, Alt and Fn "send nothing at all".
#define KBD_KEY_MOD_ALT   0xA1
#define KBD_KEY_MOD_SHL   0xA2
#define KBD_KEY_MOD_SHR   0xA3
#define KBD_KEY_MOD_SYM   0xA4
#define KBD_KEY_MOD_CTRL  0xA5
// An older reading of the controller's key set had Ctrl here. Kept alongside
// $A5 so the modifier is tracked whichever code this board's firmware sends.
#define KBD_KEY_MOD_CTRL_ALT 0x7E

// Brings up I2C1 at the controller's fixed 10kHz bus speed. Must be called
// once before any other kbd_* call.
void kbd_init(void);

// Polls the controller's key FIFO (register 0x09) once. Returns the byte a
// newly *pressed* key hands to the machine, or -1 if nothing new / on error.
// See kbd_decode() for what that byte is.
int kbd_poll(void);

// Raw FIFO read for diagnostics: 0 on a successful I2C round trip (state
// and code filled, both 0 if no event is pending), -1 on I2C error (no
// ACK/timeout talking to the controller at all). kbd_poll() is built on
// top of this + kbd_decode(); use this directly to tell "controller isn't
// responding" apart from "no key pressed".
int kbd_poll_raw(uint8_t *state, uint8_t *code);

// Polls the controller until it ACKs on the bus or `timeout_ms` elapses.
// Returns true if it answered in time.
//
// The controller -- and everything else on the mainboard's own switched
// power rail: LCD, SD slot -- doesn't come up until the PicoCalc's physical
// power button is pressed. When serial is wired through the Pico module's
// own USB port rather than the mainboard's, that port powers the RP2040
// immediately on plug-in, well before the button is (or can be) pressed, so
// anything that touches the SD card or prints a boot banner right away is
// racing hardware that isn't there yet (see repo memory / PLAN.md Phase 11).
// Call this before any of that so it waits out the gap instead of racing it.
bool kbd_wait_ready(uint32_t timeout_ms);

// Turns an already-read (state, code) pair into the byte the machine's
// keyboard encoder would put on the VIA port, or -1 for "nothing to hand
// over". This is the emulated encoder's mapping, not the PicoCalc's: letters
// are always capitals (the ACE has no lower case from the keyboard at all —
// see the DOCS keyboard chapter), Ctrl+letter gives control codes 1-26,
// Ctrl+[ \ ] 2 6 - give $1B $1C $1D $00 $1E $1F, Enter is CR, and the arrows,
// Ins and Del carry the codes the reference encoder assigns them. Keys the
// machine has no equivalent for — the function row, Caps Lock, Home/End/PgUp/
// PgDn, and the modifiers themselves — send nothing.
int kbd_decode(uint8_t state, uint8_t code);

// The emulated joystick's currently-held buttons, as a mask of the VIA
// card's GPIO_JOY_* bits (machine/gpio.h), ready to hand to
// gpio_joystick1_set(). Maintained by kbd_decode() from the press and
// release events of the keys listed in kbd.c's joystick_map -- the arrows
// and four button keys. Those keys keep typing their normal characters as
// well; the PicoCalc has no separate stick, so its keyboard is the stick
// (PLAN.md Phase 8).
uint8_t kbd_joystick_state(void);

// Reads the battery register (0x0b). Returns a controller-defined raw
// value (see hardware notes), or -1 on I2C error.
int kbd_read_battery(void);

// Sets keyboard/LCD backlight brightness (register 0x0a, 0-255). Returns 0
// on success, -1 on I2C error.
int kbd_set_backlight(uint8_t value);

#endif
