#ifndef MACHINE_SERIAL_H
#define MACHINE_SERIAL_H

#include <stdint.h>

// Emulated R65C51 ACIA (6551), IO slot 4 ($9000-$9003), bridged to the Pico's
// USB CDC stdio. Register layout/behaviour ported from 6502-DEV's SerialCard
// (see PLAN.md Phase 3), addr is slot-local (0-3):
//   0 = data (R/W), 1 = status(R)/programmed-reset(W), 2 = command (R/W),
//   3 = control (R/W).
void serial_reset(void);
uint8_t serial_read(uint16_t addr);
void serial_write(uint16_t addr, uint8_t value);

// Call once per emulated CPU clock tick. Polls USB CDC for an incoming byte
// (rate-limited) and flushes any pending transmit byte. Returns the status
// register; bit 7 (SC_STATUS_IRQ) set means the card wants an IRQ serviced.
uint8_t serial_tick(void);

#endif
