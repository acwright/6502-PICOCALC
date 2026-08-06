#ifndef MACHINE_GPIO_H
#define MACHINE_GPIO_H

#include <stdint.h>

// Emulated 65C22 VIA, IO slot 5 ($9400-$940F, see PLAN.md Phase 5 and
// BIOS.inc's GPIO_* equates). Register model ported from 6502-DEV's
// GPIOCard (DB Emulator/lib/6502/IO/GPIOCard.*). Port B carries the "matrix
// keyboard" path (per BIOS's InitKB/Irq comments), fed from the PicoCalc's
// I2C keyboard via gpio_key_press(); Port A is the PS/2 path, which this
// hardware has no source for and which reads back idle.
//
// Both ports also carry a joystick, exactly as the reference
// GPIOJoystickAttachment hangs one off each: the BIOS's ReadJoystick1/2 turn
// the encoders off (PCR CB2/CA2 high) and read the raw port, so a stick
// shows through only while no keystroke is being handed over. Ports are
// active-low, so an untouched stick reads $FF (Phase 8, and see BASIC's
// FnJoy, which returns $FF for a machine with no VIA at all).

// Joystick button bits, as GPIOJoystickAttachment.h defines them:
// | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
// | R | L | D | U | Y | X | B | A |
#define GPIO_JOY_A     0x01
#define GPIO_JOY_B     0x02
#define GPIO_JOY_X     0x04
#define GPIO_JOY_Y     0x08
#define GPIO_JOY_UP    0x10
#define GPIO_JOY_DOWN  0x20
#define GPIO_JOY_LEFT  0x40
#define GPIO_JOY_RIGHT 0x80

void gpio_reset(void);

// addr is slot-local ($9400 + addr), only the low nibble is decoded.
uint8_t gpio_read(uint16_t addr);
void gpio_write(uint16_t addr, uint8_t value);

// Call once per emulated CPU clock tick (mirrors serial_tick()/video_tick()).
// Advances VIA Timer 1 and returns 0x80 when an enabled interrupt (T1 or the
// keyboard's CB1 data-ready) is pending, 0x00 otherwise.
uint8_t gpio_tick(void);

// Feeds one decoded keystroke (already Ctrl-remapped ASCII, from
// kbd_poll()) into the emulated matrix keyboard encoder on Port B. Only the
// most recent keystroke is latched — no FIFO — matching the reference
// GPIOKeyboardEncoderAttachment: a key pressed while the previous one is
// still unread by the BIOS's IRQ handler overwrites it.
void gpio_key_press(uint8_t ascii);

// Sets the state of the stick on Port B (JOY(1), gpio_joystick1_set) or
// Port A (JOY(2)). `buttons` is a GPIO_JOY_* mask of what is held down; the
// ports invert it themselves. Port B's stick is driven from the PicoCalc's
// own keys (see kbd_joystick_state()); Port A's is left idle, there being no
// second input device on this hardware.
void gpio_joystick1_set(uint8_t buttons);
void gpio_joystick2_set(uint8_t buttons);

#endif
