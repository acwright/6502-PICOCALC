#include "serial.h"

#include <stdbool.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdio.h"

// R65C51 (6551 ACIA) register bits — see BIOS.inc for the authoritative
// layout this must match (SC_DATA/SC_RESET/SC_STATUS/SC_CMD/SC_CTRL).
#define SC_CMD_IRD      0x02 // receiver interrupt disable
#define SC_CMD_TIC0     0x04
#define SC_CMD_TIC1     0x08
#define SC_CMD_REM      0x10 // receiver echo mode
#define SC_CMD_PME      0x20 // parity mode enable
#define SC_CMD_PMC      0xC0 // parity mode control

#define SC_STATUS_OVR   0x04
#define SC_STATUS_RDRF  0x08 // receive data register full
#define SC_STATUS_TDRE  0x10 // transmit data register empty
#define SC_STATUS_IRQ   0x80

// Control register: baud rate in bits 0-3, receiver clock source in bit 4,
// word length in bits 5-6, stop bits in bit 7.
#define SC_CTRL_BAUD    0x0F
#define SC_CTRL_WORDLEN 0x60
#define SC_CTRL_STOP    0x80

// The PicoCalc's side-header UART pins (clockworkpi/PicoCalc: GP0 = UART0
// TX, GP1 = UART0 RX). They are also the Pico SDK's default UART pins.
#define SERIAL_UART   uart0
#define SERIAL_PIN_TX 0
#define SERIAL_PIN_RX 1

// What the line runs at until the machine says otherwise, which the BIOS
// does almost immediately (InitSC writes $1F = 19200 8-N-1).
#define SERIAL_DEFAULT_BAUD 19200

// Transmit bytes queue here on the way to the UART, which sends them at the
// programmed line rate. The queue is what keeps a burst of output from
// stalling the 6502 on a 19200-baud wire byte by byte; when it does fill up,
// TDRE goes low and the machine waits, exactly as it would on real hardware.
// USB CDC is not queued — it takes each byte as it is written — so the USB
// console keeps the immediacy it has had since Phase 3 for anything shorter
// than the queue.
#define SERIAL_TX_QUEUE 1024

// The 6551's sixteen baud rates. Index 0 selects an external 16x receiver
// clock instead, which nothing on this board provides, so it is treated as
// "leave the line where it is" and mapped to the default.
static const uint32_t baud_table[16] = {
    SERIAL_DEFAULT_BAUD, 50, 75, 110, 135, 150, 300, 600,
    1200, 1800, 2400, 3600, 4800, 7200, 9600, 19200,
};

static uint8_t tx, rx, cmd, ctrl, status;
static bool tx_pending;
static uint8_t rx_poll_counter;

static uint8_t tx_queue[SERIAL_TX_QUEUE];
static uint16_t tx_head, tx_tail; // tx_head == tx_tail means empty

static uint16_t queue_next(uint16_t index) {
    return (uint16_t) ((index + 1) % SERIAL_TX_QUEUE);
}

static bool queue_full(void) {
    return queue_next(tx_head) == tx_tail;
}

// Pushes each queued byte into the UART's own FIFO as it makes room, which
// is what paces the line.
static void drain_tx_queue(void) {
    while (tx_head != tx_tail && uart_is_writable(SERIAL_UART)) {
        uart_get_hw(SERIAL_UART)->dr = tx_queue[tx_tail];
        tx_tail = queue_next(tx_tail);
    }
}

// Applies the control and command registers to the real UART, so a terminal
// on the side header is talking at the rate and format the machine thinks it
// is using. Reconfiguring the UART rewrites its line-control register, which
// would corrupt a byte in flight, so nothing is touched unless the settings
// have actually changed — the BIOS rewrites the command register constantly
// while running XMODEM, only ever to move the RTS and interrupt bits.
static void apply_line_settings(void) {
    static uint8_t applied_ctrl = 0xFF;
    static uint8_t applied_parity = 0xFF;

    uint8_t parity_bits = cmd & (SC_CMD_PME | SC_CMD_PMC);
    if (ctrl == applied_ctrl && parity_bits == applied_parity) return;
    applied_ctrl = ctrl;
    applied_parity = parity_bits;

    uart_set_baudrate(SERIAL_UART, baud_table[ctrl & SC_CTRL_BAUD]);

    uint data_bits = 8 - ((ctrl & SC_CTRL_WORDLEN) >> 5); // 00 = 8 bits ... 11 = 5 bits
    uint stop_bits = (ctrl & SC_CTRL_STOP) ? 2 : 1;

    uart_parity_t parity = UART_PARITY_NONE;
    if (cmd & SC_CMD_PME) {
        // Mark and space parity have no UART equivalent here; they fall back
        // to none, the framing staying otherwise correct.
        switch (cmd & SC_CMD_PMC) {
            case 0x00: parity = UART_PARITY_ODD; break;
            case 0x40: parity = UART_PARITY_EVEN; break;
            default: break;
        }
    }

    uart_set_format(SERIAL_UART, data_bits, stop_bits, parity);
}

void serial_init(void) {
    uart_init(SERIAL_UART, SERIAL_DEFAULT_BAUD);
    gpio_set_function(SERIAL_PIN_TX, GPIO_FUNC_UART);
    gpio_set_function(SERIAL_PIN_RX, GPIO_FUNC_UART);
    uart_set_format(SERIAL_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(SERIAL_UART, true);
}

void serial_reset(void) {
    tx = 0;
    rx = 0;
    cmd = 0;
    ctrl = 0;
    status = SC_STATUS_TDRE;
    tx_pending = false;
    rx_poll_counter = 0;
    tx_head = tx_tail = 0;
    apply_line_settings();
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
            // A write while TDRE is clear overwrites the byte still waiting
            // to go out, on this card as on the real one — the queue below
            // is full and the byte is simply lost.
            if (!queue_full()) {
                tx_queue[tx_head] = value;
                tx_head = queue_next(tx_head);
            }
            putchar_raw((int) value); // USB CDC, unpaced
            status &= ~SC_STATUS_TDRE;
            tx_pending = true;
            break;
        case 1: // programmed reset (value ignored)
            cmd &= 0xE0;
            status &= ~SC_STATUS_OVR;
            apply_line_settings();
            break;
        case 2:
            cmd = value;
            apply_line_settings();
            break;
        case 3:
            ctrl = value;
            apply_line_settings();
            break;
    }
}

uint8_t serial_tick(void) {
    // Rate-limit RX polling so a byte-at-a-time USB CDC read isn't attempted
    // every single CPU tick (see 6502-DEV SerialCard::tick, PLAN.md Phase 3).
    if (++rx_poll_counter >= 64) {
        rx_poll_counter = 0;

        // The side-header UART is checked first; anything it hasn't sent,
        // the USB console might have (PLAN.md Phase 8).
        int c = uart_is_readable(SERIAL_UART) ? (int) uart_getc(SERIAL_UART)
                                              : getchar_timeout_us(0);
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

    drain_tx_queue();

    // TDRE comes back as soon as the queue has room for another byte.
    if (tx_pending && !queue_full()) {
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
