#include "serial.h"

#include <stdbool.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico.h"
#include "pico/stdio.h"

// The card modelled is the standard Serial Card with its `CTS EN` jumper at
// ground, as every board is built (6502-COB `09988ca`). On that card DCDB and
// DSRB are tied to ground and DTRB is not connected, so all three of the
// R6551's modem inputs are permanently low — asserted:
//
// - CTSB low never gates the transmitter, so there is no CTS here at all. A
//   variable for a pin soldered to ground would only be dead code to drift.
// - DCDB low never gates the receiver, so DTR alone does (receiver_enabled()).
// - DSRB and DCDB read 0 in the status register, and never change, so the
//   chip's interrupt on a modem-line change can never fire either.
// - DTRB reaching nothing on the card does not stop command bit 0 gating the
//   receiver, the transmitter and the interrupts inside the chip; that is
//   modelled, and is not a mistake.
//
// Here that is not only the chosen card but the only honest one. The ACIA is
// bridged to USB CDC and the side-header UART, and neither carries an RTS or
// CTS wire, so there is no far end that could drive a line if a jumper were
// moved to the cable. No card picker, no jumpers, and none of the Serial Card
// Pro's lines.
//
// With every line at ground this agrees with the reference model,
// 6502-EMULATOR's src/core/IO/ACIA.ts and SerialCard.ts, in all but the places
// commented below: a byte that arrives while the receiver is off waits on the
// link instead of being lost (serial_tick()), an overrun can happen here, from
// the UART's own flag, where the reference has no line timing to overrun, and
// an echo can be lost on the side header if its queue is full.

// R65C51 (6551 ACIA) register bits — see BIOS.inc for the authoritative
// layout this must match (SC_DATA/SC_RESET/SC_STATUS/SC_CMD/SC_CTRL).
#define SC_CMD_DTR      0x01 // data terminal ready; clear turns the card off
#define SC_CMD_IRD      0x02 // receiver interrupt disable
#define SC_CMD_TIC0     0x04
#define SC_CMD_TIC1     0x08
#define SC_CMD_TIC      0x0C // transmitter interrupt control; 00 = transmitter off
#define SC_CMD_REM      0x10 // receiver echo mode
#define SC_CMD_PME      0x20 // parity mode enable
#define SC_CMD_PMC      0xC0 // parity mode control

#define SC_STATUS_PE    0x01 // parity error
#define SC_STATUS_FE    0x02 // framing error
#define SC_STATUS_OVR   0x04
#define SC_STATUS_RDRF  0x08 // receive data register full
#define SC_STATUS_TDRE  0x10 // transmit data register empty
#define SC_STATUS_DCD   0x20 // data carrier detect, active low: 0 = carrier
#define SC_STATUS_DSR   0x40 // data set ready, active low: 0 = ready
#define SC_STATUS_IRQ   0x80

// The three receive error flags. The datasheet is explicit that these are
// "automatically cleared after a read of the Receiver Data Register" — not
// after a status read, which clears the interrupt flag and nothing else.
#define SC_STATUS_ERRORS (SC_STATUS_PE | SC_STATUS_FE | SC_STATUS_OVR)

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
// TDRE stays low and the machine waits, exactly as it would on real hardware.
// USB CDC is not queued — it takes each byte as the transmitter sends it — so
// the USB console keeps the immediacy it has had since Phase 3 for anything
// shorter than the queue.
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

// Whether the transmitter is running.
//
// The R6551 needs two things for it: DTR (command bit 0) set — with it clear
// "the transmitter is disabled immediately" — and a TIC (command bits 3-2)
// other than 00. Rockwell's Rev. 4 sheet and Synertek's SY6551 sheet spell the
// four TIC values out as
//
//   00 = Transmit Interrupt Disabled, RTSB = High, Transmitter Off
//   01 = Transmit Interrupt Enabled,  RTSB = Low,  Transmitter On
//   10 = Transmit Interrupt Disabled, RTSB = Low,  Transmitter On
//   11 = Transmit Interrupt Disabled, RTSB = Low,  Transmit BRK
//
// so 00 is not merely "transmit interrupt disabled", as Rockwell's 1981 Rev. 1
// sheet has it. That was settled on the bench (2026-09-17) on a real KIM with
// a real R6551: POKE 36866,1 ($01 — DTR on, TIC 00) stopped the board
// transmitting mid-reply and hung it, while POKE 36866,9 ($09 — TIC 10) sent
// normally.
//
// A disabled transmitter never empties the transmit data register, so TDRE
// stays clear and firmware that writes a byte and then polls TDRE — which is
// every BIOS here — waits. That is the point: it is what the chip does.
//
// Modelling it is only safe because the embedded ROM is the reissued BIOS v1.6
// (see src/rom/bios_rom.c), whose SerialChrout calls ScRtsLow to put TIC back
// to 10 before every byte it sends, and whose XModem command value is $0B —
// DTR on, TIC 10 — so the transmitter is never off while it is transmitting.
// A ROM loaded from the SD card that raises RTS and then transmits will hang
// here, exactly as it hangs on a real board; that is the fidelity, not a bug.
//
// CTSB high would disable the transmitter too, but it is at ground on the card
// modelled here (see the top of this file), so it never does.
static inline bool transmitter_enabled(void) {
    return (cmd & SC_CMD_DTR) && (cmd & SC_CMD_TIC);
}

// Whether the receiver is running: DTR set, and DCDB low, which it always is
// on this card (see the top of this file).
static inline bool receiver_enabled(void) {
    return (cmd & SC_CMD_DTR) != 0;
}

// Pushes each queued byte into the UART's own FIFO as it makes room, which
// is what paces the line. RAM-resident along with serial_tick() below:
// called from machine_run() on every emulated cycle (PLAN.md Phase 11 perf
// pass — see machine.c's bus_read()/bus_write() for the measurement and full
// reasoning).
static void __not_in_flash_func(drain_tx_queue)(void) {
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
            // A data read clears RDRF and, per the datasheet, the three
            // receive error flags, and takes any pending interrupt with them.
            status &= (uint8_t) ~(SC_STATUS_IRQ | SC_STATUS_RDRF | SC_STATUS_ERRORS);
            return rx;
        case 1: // status register
            // Bits 6 and 5 are the levels on the DSRB and DCDB pins, and both
            // are active low: 0 means ready / carrier present. The standard
            // Serial Card ties both pins to ground, so both always read 0.
            s = (uint8_t) (status & ~(SC_STATUS_DCD | SC_STATUS_DSR));
            // Reading clears the interrupt flag and nothing else; the byte
            // returned above is the state from before the clear.
            status &= (uint8_t) ~SC_STATUS_IRQ;
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
            // The byte only waits here. serial_tick() sends it, and only once
            // the transmitter is on — so a write made with DTR clear or TIC
            // 00 leaves TDRE clear and the byte where it is until the command
            // register says the transmitter may run again. A write while TDRE
            // is clear overwrites the byte still waiting, on this card as on
            // the real one.
            tx = value;
            status &= ~SC_STATUS_TDRE;
            tx_pending = true;
            break;
        case 1: // programmed reset (value ignored)
            // Per the datasheet's "Program Reset Operation": clears command
            // bits 4-0 — so DTR goes high, the receiver, transmitter and
            // interrupts are disabled, RTS goes high and echo mode ends — and
            // the overrun bit of the status register. The control register,
            // the other status bits and any byte waiting in the transmit
            // register are left as they were, and a pending interrupt is not
            // withdrawn: "if IRQ is low when the reset occurs, it stays low
            // until serviced".
            cmd &= 0xE0;
            status &= (uint8_t) ~SC_STATUS_OVR;
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

uint8_t __not_in_flash_func(serial_tick)(void) {
    // Rate-limit RX polling so a byte-at-a-time USB CDC read isn't attempted
    // every single CPU tick (see 6502-DEV SerialCard::tick, PLAN.md Phase 3).
    //
    // Nothing is pulled off either link while the receive register is still
    // full: the byte stays where it is — in the UART's FIFO, or in the USB
    // stack's buffer, where the host will be flow-controlled if it backs up —
    // rather than being read here and dropped. That matters for XMODEM
    // (PLAN.md Phase 10), where a host sends a 132-byte packet as one burst
    // and every byte of it has to arrive; a real 6551 is fed by a wire that
    // paces itself, and this is the closest equivalent. An overrun is
    // reported from the UART's own OE flag, which is where it can genuinely
    // still happen.
    //
    // Nothing is taken off either link while the receiver is off (DTR clear,
    // the reset state): on a board the byte would be lost on the wire, while
    // here it waits in the UART's FIFO or the USB stack's buffer until the
    // machine programs the command register. That is the one place this card
    // is kinder than the chip, and the window is only the few hundred cycles
    // between reset and the BIOS's InitSC.
    if (++rx_poll_counter >= 64) {
        rx_poll_counter = 0;

        if (receiver_enabled()) {
            if (uart_get_hw(SERIAL_UART)->rsr & UART_UARTRSR_OE_BITS) {
                uart_get_hw(SERIAL_UART)->rsr = 0; // write clears it
                status |= SC_STATUS_OVR;
            }

            if (!(status & SC_STATUS_RDRF)) {
                // The side-header UART is checked first; anything it hasn't
                // sent, the USB console might have (PLAN.md Phase 8).
                int c = uart_is_readable(SERIAL_UART) ? (int) uart_getc(SERIAL_UART)
                                                      : getchar_timeout_us(0);
                if (c != PICO_ERROR_TIMEOUT) {
                    rx = (uint8_t) c;
                    status |= SC_STATUS_RDRF;
                    // Echo mode retransmits the byte on TxD without going
                    // through the transmit register, so TIC 00 does not stop
                    // it; CTSB high would, but it is at ground. TxD is both
                    // links here, as for any byte the machine sends, so the
                    // echo goes out of both. On the chip the echo runs a half
                    // bit behind the receiver and cannot back up; here the
                    // USB console can outrun the UART, and an echo that finds
                    // the queue full is lost on the side header only.
                    if (cmd & SC_CMD_REM) {
                        if (!queue_full()) {
                            tx_queue[tx_head] = rx;
                            tx_head = queue_next(tx_head);
                        }
                        putchar_raw((int) rx);
                    }
                    // Receiver interrupt: IRD (bit 1) clear enables it, and
                    // DTR is already on or we would not be here.
                    if (!(cmd & SC_CMD_IRD)) {
                        status |= SC_STATUS_IRQ;
                    }
                }
            }
        }
    }

    drain_tx_queue();

    // The waiting byte goes out once the transmitter is on and the queue has
    // room for it, and TDRE comes back with it. With the transmitter off, or
    // the queue full at 19200 baud, the byte stays in the transmit register
    // and TDRE stays clear — which is what makes the machine wait.
    if (tx_pending && transmitter_enabled() && !queue_full()) {
        tx_queue[tx_head] = tx;
        tx_head = queue_next(tx_head);
        putchar_raw((int) tx); // USB CDC, unpaced

        status |= SC_STATUS_TDRE;
        tx_pending = false;

        // Transmit interrupt: TIC (bits 3-2) = 01, which is the only one of
        // the four that enables it. IRD is the *receiver* interrupt's bit and
        // has no say here.
        if ((cmd & SC_CMD_TIC) == SC_CMD_TIC0) {
            status |= SC_STATUS_IRQ;
        }
    }

    // With DTR clear "all interrupts are disabled": the IRQB pin is not
    // driven, whatever the status register happens to be holding.
    return (cmd & SC_CMD_DTR) ? status : (uint8_t) (status & ~SC_STATUS_IRQ);
}
