#include "serial.h"

#include <stdbool.h>

#include "pico/stdio.h"

// R65C51 (6551 ACIA) register bits — see BIOS.inc for the authoritative
// layout this must match (SC_DATA/SC_RESET/SC_STATUS/SC_CMD/SC_CTRL).
#define SC_CMD_IRD      0x02 // receiver interrupt disable
#define SC_CMD_TIC0     0x04
#define SC_CMD_TIC1     0x08
#define SC_CMD_REM      0x10 // receiver echo mode

#define SC_STATUS_OVR   0x04
#define SC_STATUS_RDRF  0x08 // receive data register full
#define SC_STATUS_TDRE  0x10 // transmit data register empty
#define SC_STATUS_IRQ   0x80

static uint8_t tx, rx, cmd, ctrl, status;
static bool tx_pending;
static uint8_t rx_poll_counter;

void serial_reset(void) {
    tx = 0;
    rx = 0;
    cmd = 0;
    ctrl = 0;
    status = SC_STATUS_TDRE;
    tx_pending = false;
    rx_poll_counter = 0;
}

uint8_t serial_read(uint16_t addr) {
    uint8_t s;
    switch (addr & 0x03) {
        case 0: // receive data register
            status &= ~(SC_STATUS_IRQ | SC_STATUS_RDRF);
            return rx;
        case 1: // status register
            s = status;
            status &= ~SC_STATUS_IRQ;
            return s;
        case 2:
            return cmd;
        case 3:
            return ctrl;
        default:
            return 0;
    }
}

void serial_write(uint16_t addr, uint8_t value) {
    switch (addr & 0x03) {
        case 0: // transmit data register
            tx = value;
            status &= ~SC_STATUS_TDRE;
            tx_pending = true;
            break;
        case 1: // programmed reset (value ignored)
            cmd &= 0xE0;
            status &= ~SC_STATUS_OVR;
            break;
        case 2:
            cmd = value;
            break;
        case 3:
            ctrl = value;
            break;
    }
}

uint8_t serial_tick(void) {
    // Rate-limit RX polling so a byte-at-a-time USB CDC read isn't attempted
    // every single CPU tick (see 6502-DEV SerialCard::tick, PLAN.md Phase 3).
    if (++rx_poll_counter >= 64) {
        rx_poll_counter = 0;
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            if (status & SC_STATUS_RDRF) {
                status |= SC_STATUS_OVR; // previous byte not yet read
            } else {
                rx = (uint8_t) c;
                status |= SC_STATUS_RDRF;
                if (cmd & SC_CMD_REM) {
                    putchar_raw((int) rx);
                }
                if (!(cmd & SC_CMD_IRD)) {
                    status |= SC_STATUS_IRQ;
                }
            }
        }
    }

    if (tx_pending) {
        putchar_raw((int) tx);
        status |= SC_STATUS_TDRE;
        tx_pending = false;

        if (!(cmd & SC_CMD_IRD)) {
            uint8_t tic = (cmd & (SC_CMD_TIC0 | SC_CMD_TIC1)) >> 2;
            if (tic == 0x01) { // interrupt on TX register empty
                status |= SC_STATUS_IRQ;
            }
        }
    }

    return status;
}
