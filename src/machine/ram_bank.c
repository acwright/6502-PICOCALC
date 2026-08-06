#include "ram_bank.h"

#include <stddef.h>
#include <string.h>

_Static_assert(RAM_BANK_LOW_BANKS <= 256 &&
                   (RAM_BANK_LOW_BANKS & (RAM_BANK_LOW_BANKS - 1)) == 0,
               "RAM_BANK_LOW_BANKS must be a power of two, 0-256");
_Static_assert(RAM_BANK_HIGH_BANKS <= 256 &&
                   (RAM_BANK_HIGH_BANKS & (RAM_BANK_HIGH_BANKS - 1)) == 0,
               "RAM_BANK_HIGH_BANKS must be a power of two, 0-256");

#if RAM_BANK_LOW_BANKS > 0
static uint8_t low_data[RAM_BANK_LOW_BANKS * RAM_BANK_WINDOW_SIZE];
#endif
#if RAM_BANK_HIGH_BANKS > 0
static uint8_t high_data[RAM_BANK_HIGH_BANKS * RAM_BANK_WINDOW_SIZE];
#endif

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint16_t banks;  // compiled in, 0 = slot empty
    uint16_t active; // presented to the machine, 1..banks (0 only if empty)
    uint8_t bank;    // latch, $00-$FF as written
} ram_bank_card_t;

static ram_bank_card_t cards[2] = {
#if RAM_BANK_LOW_BANKS > 0
    {low_data, sizeof(low_data), RAM_BANK_LOW_BANKS, RAM_BANK_LOW_BANKS, 0},
#else
    {NULL, 0, 0, 0, 0},
#endif
#if RAM_BANK_HIGH_BANKS > 0
    {high_data, sizeof(high_data), RAM_BANK_HIGH_BANKS, RAM_BANK_HIGH_BANKS, 0},
#else
    {NULL, 0, 0, 0, 0},
#endif
};

// A card presenting fewer than 256 banks decodes only as many latch bits as it
// has banks for, so the 256-bank address space folds over what is fitted --
// the same masking 6502-DEV's RAMCard does on its 16-bank build, and what a
// real card short of address lines would do. All 256 BANK numbers therefore
// stay usable; they just alias.
static uint32_t bank_offset(const ram_bank_card_t *card, uint16_t addr) {
    uint32_t bank = card->bank & (uint32_t) (card->active - 1);
    return (bank << 10) | (addr & (RAM_BANK_WINDOW_SIZE - 1));
}

void ram_bank_init(void) {
    for (size_t i = 0; i < 2; i++) {
        cards[i].active = cards[i].banks; // all of it until a setting says otherwise
    }
    ram_bank_clear();
}

void ram_bank_reset(void) {
    // Nothing: see the note on the declaration in ram_bank.h.
}

void ram_bank_clear(void) {
    for (size_t i = 0; i < 2; i++) {
        cards[i].bank = 0;
        if (cards[i].data) memset(cards[i].data, 0x00, cards[i].size);
    }
}

uint8_t ram_bank_read(ram_bank_id_t id, uint16_t addr) {
    const ram_bank_card_t *card = &cards[id];
    if (card->banks == 0) return 0x00; // slot empty -> open bus
    return card->data[bank_offset(card, addr)];
}

void ram_bank_write(ram_bank_id_t id, uint16_t addr, uint8_t value) {
    ram_bank_card_t *card = &cards[id];
    if (card->banks == 0) return; // slot empty -> write goes nowhere

    addr &= RAM_BANK_WINDOW_SIZE - 1;
    // The latch is selected before the store, so the byte lands in the bank
    // being switched *to*, not the one being left. ProbeRAM depends on this
    // ordering only indirectly, but the desktop emulator and the Teensy both
    // do it, and a program that pages by writing $x3FF sees the same result.
    if (addr == RAM_BANK_LATCH_ADDR) card->bank = value;

    card->data[bank_offset(card, addr)] = value;
}

uint16_t ram_bank_banks(ram_bank_id_t id) {
    return cards[id].banks;
}

uint16_t ram_bank_active(ram_bank_id_t id) {
    return cards[id].active;
}

void ram_bank_set_active(ram_bank_id_t id, uint16_t banks) {
    ram_bank_card_t *card = &cards[id];
    if (card->banks == 0) return; // nothing fitted to resize

    if (banks > card->banks) banks = card->banks;
    // Never 0: bank_offset() masks with active-1, which on an unsigned zero
    // would come out all-ones and address the whole card rather than none of
    // it. A card with no banks at all is a build-time choice (banks == 0
    // above), not something to be arrived at by narrowing.
    if (banks < 1) banks = 1;

    uint16_t rounded = 1;
    while ((uint16_t) (rounded << 1) && (uint16_t) (rounded << 1) <= banks) rounded <<= 1;

    card->active = rounded;
    // The latch keeps whatever it held; the mask above simply folds it into
    // the smaller space from here on, exactly as it would on a smaller card.
}
