// Phase 2: I2C1 keyboard driver — STM32 keyboard/backlight/battery
// controller on the PicoCalc mainboard. See PLAN.md §2/Phase 2 and
// clockworkpi/PicoCalc Code/picocalc_helloworld/i2ckbd/ (protocol
// reference; this is our own port/implementation, not a copy).
#ifndef KBD_H
#define KBD_H

#include <stdint.h>

// Special (non-ASCII) key code the controller reports for the physical
// Ctrl key itself, so it can be tracked as a held modifier below.
#define KBD_KEY_CTRL 0x7E

// The controller's own Enter key code (see clockworkpi/PicoCalc's
// keyboard.h KEY_ENTER) -- it is 0x0A, NOT ASCII CR. kbd_decode() remaps it
// to CR (0x0D) since that's what the emulated console (BIOS/BASIC's line
// editor) checks for end-of-line, same as a serial terminal's Enter key.
#define KBD_KEY_ENTER_RAW 0x0A

// The controller's own Esc key code (clockworkpi/PicoCalc's keyboard.h
// KEY_ESC) -- it is 0xB1, NOT ASCII ESC. kbd_decode() remaps it to 0x1B
// since that's the byte BASIC's BasCheckBreak polls for to break a running
// program (along with Ctrl-C); left unmapped, ESC presses were silently
// swallowed and could never interrupt a RUNning loop.
#define KBD_KEY_ESC_RAW 0xB1

// Arrow-key codes from the controller's own key set (clockworkpi/PicoCalc
// Code/picocalc_keyboard/keyboard.h KEY_LEFT..KEY_RIGHT). They pass through
// kbd_decode() unchanged, and also drive the emulated joystick below.
#define KBD_KEY_LEFT_RAW  0xB4
#define KBD_KEY_UP_RAW    0xB5
#define KBD_KEY_DOWN_RAW  0xB6
#define KBD_KEY_RIGHT_RAW 0xB7

// Brings up I2C1 at the controller's fixed 10kHz bus speed. Must be called
// once before any other kbd_* call.
void kbd_init(void);

// Polls the controller's key FIFO (register 0x09) once. Returns the ASCII
// code of a newly *pressed* key, or -1 if nothing new/on error. Held Ctrl
// remaps a-z to the corresponding control code (Ctrl-A=1 .. Ctrl-Z=26), as
// the emulated 6551 console expects.
int kbd_poll(void);

// Raw FIFO read for diagnostics: 0 on a successful I2C round trip (state
// and code filled, both 0 if no event is pending), -1 on I2C error (no
// ACK/timeout talking to the controller at all). kbd_poll() is built on
// top of this + kbd_decode(); use this directly to tell "controller isn't
// responding" apart from "no key pressed".
int kbd_poll_raw(uint8_t *state, uint8_t *code);

// Applies the same press/Ctrl-modifier decode as kbd_poll() to an
// already-read (state, code) pair.
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
