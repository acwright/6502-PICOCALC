#ifndef MACHINE_GPIO_H
#define MACHINE_GPIO_H

#include <stdint.h>

// Emulated 65C22 VIA, IO slot 5 ($9400-$940F, see PLAN.md Phase 5 and
// BIOS.inc's GPIO_* equates). Register model ported from 6502-DEV's
// GPIOCard (DB Emulator/lib/6502/IO/GPIOCard.*). Only Port B (the "matrix
// keyboard" path, per BIOS's InitKB/Irq comments) is wired to a real input
// source — the PicoCalc's I2C keyboard, fed in via gpio_key_press(). Port A
// (PS/2) and the joystick bits read back as idle/no-data, since this
// hardware has neither (joystick support is Phase 8).

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

#endif
