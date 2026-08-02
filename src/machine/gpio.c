#include "gpio.h"

#include <stdbool.h>

// 65C22 VIA register offsets within the slot (see BIOS.inc GPIO_* and
// 6502-DEV's GPIOCard.h VIA_* constants — this must match).
#define VIA_ORB     0x00
#define VIA_ORA     0x01
#define VIA_DDRB    0x02
#define VIA_DDRA    0x03
#define VIA_T1CL    0x04
#define VIA_T1CH    0x05
#define VIA_T1LL    0x06
#define VIA_T1LH    0x07
#define VIA_T2CL    0x08
#define VIA_T2CH    0x09
#define VIA_SR      0x0A
#define VIA_ACR     0x0B
#define VIA_PCR     0x0C
#define VIA_IFR     0x0D
#define VIA_IER     0x0E
#define VIA_ORA_NH  0x0F

// IFR/IER interrupt bits.
#define IRQ_CA2     0x01
#define IRQ_CA1     0x02
#define IRQ_SR      0x04
#define IRQ_CB2     0x08
#define IRQ_CB1     0x10
#define IRQ_T2      0x20
#define IRQ_T1      0x40
#define IRQ_IRQ     0x80

static uint8_t orb, ora, ddrb, ddra;
static uint16_t t1c, t1l;
static uint8_t t2cl, t2ch, sr, acr, pcr, ifr, ier;
static bool t1_running;

// Derived from PCR bits 7-5 (CB2 mode): manual-output-low ($C0) enables the
// matrix keyboard encoder, manual-output-high ($E0) disables it — see
// BIOS.inc GPIO_PCR_CB2_LO/HI and Kernal.asm's KBEnable/KBDisable.
static bool cb2_enabled;

static uint8_t kb_ascii;
static bool kb_data_ready;

static void update_irq(void) {
    if (ifr & ier & 0x7F) {
        ifr |= IRQ_IRQ;
    } else {
        ifr &= ~IRQ_IRQ;
    }
}

// Combines a port's data-direction/output register with an external input
// source per-bit, as real 6522 hardware does (output bits read back the
// register, input bits read the external line) — see GPIOCard::readPortA/B.
static uint8_t combine_port(uint8_t ddr, uint8_t reg_out, uint8_t external) {
    uint8_t value = 0xFF;
    for (int bit = 0; bit < 8; bit++) {
        uint8_t mask = (uint8_t) (1 << bit);
        bool bit_set = (ddr & mask) ? (reg_out & mask) : (external & mask);
        if (bit_set) value |= mask; else value &= (uint8_t) ~mask;
    }
    return value;
}

static uint8_t read_port_a(void) {
    return combine_port(ddra, ora, 0xFF); // no PS/2 keyboard or joystick 2 wired
}

static uint8_t read_port_b(void) {
    uint8_t external = (cb2_enabled && kb_data_ready) ? kb_ascii : 0xFF;
    return combine_port(ddrb, orb, external);
}

void gpio_reset(void) {
    orb = ora = ddrb = ddra = 0x00;
    t1c = t1l = 0xFFFF;
    t2cl = t2ch = sr = acr = pcr = ifr = ier = 0x00;
    t1_running = false;
    cb2_enabled = false;
    kb_ascii = 0;
    kb_data_ready = false;
}

uint8_t gpio_read(uint16_t addr) {
    uint8_t reg = addr & 0x0F;
    uint8_t value = 0x00;

    switch (reg) {
        case VIA_ORB: {
            bool served = cb2_enabled && kb_data_ready;
            ifr &= ~(IRQ_CB1 | IRQ_CB2);
            update_irq();
            value = read_port_b();
            if (served) kb_data_ready = false; // consumed
            break;
        }
        case VIA_ORA:
            ifr &= ~(IRQ_CA1 | IRQ_CA2);
            update_irq();
            value = read_port_a();
            break;
        case VIA_DDRB:
            value = ddrb;
            break;
        case VIA_DDRA:
            value = ddra;
            break;
        case VIA_T1CL:
            ifr &= ~IRQ_T1;
            update_irq();
            value = (uint8_t) (t1c & 0xFF);
            break;
        case VIA_T1CH:
            value = (uint8_t) (t1c >> 8);
            break;
        case VIA_T1LL:
            value = (uint8_t) (t1l & 0xFF);
            break;
        case VIA_T1LH:
            value = (uint8_t) (t1l >> 8);
            break;
        case VIA_T2CL:
            value = t2cl;
            break;
        case VIA_T2CH:
            value = t2ch;
            break;
        case VIA_SR:
            value = sr;
            break;
        case VIA_ACR:
            value = acr;
            break;
        case VIA_PCR:
            value = pcr;
            break;
        case VIA_IFR:
            value = ifr;
            break;
        case VIA_IER:
            value = ier | 0x80; // bit 7 always reads as 1
            break;
        case VIA_ORA_NH:
            value = read_port_a(); // no interrupt-flag side effects
            break;
    }

    return value;
}

void gpio_write(uint16_t addr, uint8_t value) {
    uint8_t reg = addr & 0x0F;

    switch (reg) {
        case VIA_ORB:
            ifr &= ~(IRQ_CB1 | IRQ_CB2);
            update_irq();
            orb = value;
            break;
        case VIA_ORA:
            ifr &= ~(IRQ_CA1 | IRQ_CA2);
            update_irq();
            ora = value;
            break;
        case VIA_DDRB:
            ddrb = value;
            break;
        case VIA_DDRA:
            ddra = value;
            break;
        case VIA_T1CL:
        case VIA_T1LL:
            t1l = (uint16_t) ((t1l & 0xFF00) | value);
            break;
        case VIA_T1CH:
            t1l = (uint16_t) ((t1l & 0x00FF) | (value << 8));
            t1c = t1l;
            ifr &= ~IRQ_T1;
            update_irq();
            t1_running = true;
            break;
        case VIA_T1LH:
            t1l = (uint16_t) ((t1l & 0x00FF) | (value << 8));
            ifr &= ~IRQ_T1;
            update_irq();
            break;
        case VIA_T2CL:
            t2cl = value;
            break;
        case VIA_T2CH:
            t2ch = value;
            break;
        case VIA_SR:
            sr = value;
            ifr &= ~IRQ_SR;
            update_irq();
            break;
        case VIA_ACR:
            acr = value;
            break;
        case VIA_PCR: {
            pcr = value;
            uint8_t cb2_mode = (uint8_t) ((value >> 5) & 0x07);
            if (cb2_mode == 0x06) cb2_enabled = true;       // manual output low
            else if (cb2_mode == 0x07) cb2_enabled = false; // manual output high
            break;
        }
        case VIA_IFR:
            ifr &= (uint8_t) ~(value & 0x7F);
            update_irq();
            break;
        case VIA_IER:
            if (value & 0x80) ier |= (value & 0x7F);
            else ier &= (uint8_t) ~(value & 0x7F);
            update_irq();
            break;
        case VIA_ORA_NH:
            ora = value; // no interrupt-flag side effects
            break;
    }
}

uint8_t gpio_tick(void) {
    if (t1_running && t1c > 0) {
        t1c--;
        if (t1c == 0) {
            ifr |= IRQ_T1;
            if (acr & 0x40) t1c = t1l; // free-run: reload from latch
            else t1_running = false;  // one-shot: stop until rewritten
        }
    }

    if (cb2_enabled && kb_data_ready) {
        ifr |= IRQ_CB1;
    }

    update_irq();
    return (ifr & ier & 0x7F) ? IRQ_IRQ : 0x00;
}

void gpio_key_press(uint8_t ascii) {
    kb_ascii = ascii;
    kb_data_ready = true;
}
