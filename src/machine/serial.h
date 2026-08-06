#ifndef MACHINE_SERIAL_H
#define MACHINE_SERIAL_H

#include <stdint.h>

// Emulated R65C51 ACIA (6551), IO slot 4 ($9000-$9003), bridged to the Pico's
// USB CDC stdio. Register layout/behaviour ported from 6502-DEV's SerialCard
// (see PLAN.md Phase 3), addr is slot-local (0-3):
//   0 = data (R/W), 1 = status(R)/programmed-reset(W), 2 = command (R/W),
//   3 = control (R/W).
//
// Phase 8 puts the same ACIA on the PicoCalc's side-header UART pins as well
// (UART0, GP0 = TX, GP1 = RX), so a real terminal or an XMODEM transfer can
// talk to the machine with no host computer in the middle. Both links are
// live at once: bytes the machine sends go out of both, and a byte arriving
// on either is what it receives. The UART runs at whatever the control
// register asks for -- the BIOS's InitSC sets 19200 8-N-1 -- while USB CDC
// ignores line settings, as USB serial does.
//
// Debug output (printf) deliberately stays on USB CDC only, so the UART
// carries nothing but the emulated machine's own bytes and binary transfers
// come through clean.

// Brings up the UART and its pins. Call once at boot, before serial_reset().
void serial_init(void);

void serial_reset(void);
uint8_t serial_read(uint16_t addr);
void serial_write(uint16_t addr, uint8_t value);

// Call once per emulated CPU clock tick. Polls the UART and USB CDC for an
// incoming byte (rate-limited) and pushes transmitted bytes out. Returns the
// status register; bit 7 (SC_STATUS_IRQ) set means the card wants an IRQ
// serviced.
uint8_t serial_tick(void);

#endif
