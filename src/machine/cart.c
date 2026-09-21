#include "cart.h"

#include <stddef.h>

#include "pico.h" // __not_in_flash_func

// Where the image is, and what §2's derived constants work out to for it.
// All read-only once cart_init() has run, bar the register.
static const uint8_t *cart_image;
static uint32_t cart_image_size;
static uint32_t chip_size;
static uint8_t chips;
static uint8_t bank_mask;
static bool banked;

// The bank register: write-only, 8 bits, at any address in $E000-$FFFF.
// Bit 7 is latched and goes nowhere.
static uint8_t bank_reg;

bool cart_size_banked(uint32_t size) {
    return size == CART_SIZE_128K || size == CART_SIZE_256K ||
           size == CART_SIZE_512K || size == CART_SIZE_1M;
}

bool cart_size_valid(uint32_t size) {
    // Up to 32 KB is the flat cart this port has always taken: a bare 16 KB
    // build, or a 32 KB EEPROM image whose first half is padding. Above that,
    // only the four banked lengths — a file of some other size is not a chip
    // image, and loading half of it would be worse than saying so.
    return (size > 0 && size <= CART_SIZE_FLAT) || cart_size_banked(size);
}

const char *cart_size_name(uint32_t size) {
    if (size == CART_SIZE_128K) return "128K";
    if (size == CART_SIZE_256K) return "256K";
    if (size == CART_SIZE_512K) return "512K";
    if (size == CART_SIZE_1M)   return "1M";
    if (size > 0x4000u && size <= CART_SIZE_FLAT) return "32K";
    if (size > 0) return "16K";
    return "?";
}

void cart_init(const uint8_t *image, uint32_t size) {
    cart_image = image;
    cart_image_size = size;
    banked = image != NULL && cart_size_banked(size);

    if (banked) {
        // §2's derived constants. On a -1M image U1 is the first 512 KB and U2
        // the second (§1 rule 4), so the file is linear and so is the bank
        // numbering.
        chips = (size > CART_SIZE_512K) ? 2 : 1;
        chip_size = size / chips;
        bank_mask = (uint8_t) ((chip_size / CART_BANK_SIZE) - 1);
    } else {
        chips = 0;
        chip_size = 0;
        bank_mask = 0;
    }

    cart_reset();
}

void cart_reset(void) {
    bank_reg = 0;
}

bool cart_banked(void) {
    return banked;
}

uint8_t cart_bank(void) {
    return bank_reg;
}

// RAM-resident for the same reason machine.c's bus dispatch is: every opcode
// fetch a banked cartridge makes lands here and then in flash, and letting
// this compete for the RP2040's 16 KB XIP cache against that same flash
// traffic is pure loss.
uint8_t __not_in_flash_func(cart_read)(uint16_t addr) {
    // The fixed region is always the **primary** chip's last 8 KB, whatever
    // the register holds. `chip` never applies to it, which is what keeps the
    // reset and interrupt vectors reachable from every bank.
    if (addr >= CART_FIXED_BASE) {
        return cart_image[chip_size - CART_BANK_SIZE + (addr - CART_FIXED_BASE)];
    }

    uint8_t chip = (bank_reg >> 6) & 1;
    // Selecting the absent U2 — a 512 KB image, or a 1 MB cart with only one
    // chip fitted — reads open bus, which every emulator in the rollout models
    // as $FF.
    if (chip >= chips) return 0xFF;

    // The NC high address pins on a 010A or a 020A are exactly this mask, so a
    // register value above the part's bank count aliases down, and values
    // 128-255 mirror 0-127.
    uint32_t at = (uint32_t) chip * chip_size +
                  (uint32_t) (bank_reg & bank_mask) * CART_BANK_SIZE +
                  (uint32_t) (addr - CART_WINDOW_BASE);
    return cart_image[at];
}

void __not_in_flash_func(cart_write)(uint16_t addr, uint8_t data) {
    // A write anywhere in $E000-$FFFF latches the register and **reaches no
    // flash** — WEB is not asserted there, so it is not also a flash write.
    if (addr >= CART_FIXED_BASE) {
        bank_reg = data;
        return;
    }

    // $C000-$DFFF is a flash write, which only does anything as part of a JEDEC
    // command sequence (§3). Until that state machine exists, a write here is
    // swallowed exactly as one outside a sequence is.
    (void) data;
}
