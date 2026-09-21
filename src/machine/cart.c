#include "cart.h"

#include <stddef.h>

#include <string.h>

#include "pico.h" // __not_in_flash_func

#include "media.h"

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

// Where a chip's JEDEC unlock sequence has got to (§3). Each chip has its own:
// the sequence is addressed through /CE and B6 picks the chip, so programming
// U2 does not disturb U1.
enum {
    SEQ_IDLE,
    SEQ_UNLOCK1, // 5555<-AA seen
    SEQ_UNLOCK2, // 2AAA<-55 seen
    SEQ_PROGRAM, // 5555<-A0 seen; the next write is the byte
    SEQ_ERASE1,  // 5555<-80 seen
    SEQ_ERASE2,  // 5555<-AA seen again
    SEQ_ERASE3,  // 2AAA<-55 seen again; the next write picks chip or sector
};

static uint8_t seq[2];
static bool id_mode[2];
// One flag for both, so the hot read path costs one predictable branch rather
// than two array loads for a mode nothing is normally in.
static bool id_any;
// $B5 / $B6 / $B7, derived from CHIP_SIZE so that this and `id` over the Flash
// Helper return the same pair.
static uint8_t device_id;

// The write-back sector cache. `cache_at` is the image offset of the 4 KB the
// staging buffer (media_stage) is holding, or CACHE_NONE. Reads of that range
// come out of RAM and everything else comes out of flash, which is one subtract
// and one compare on the hot path — the honest price of not erasing a 4 KB
// sector per programmed byte.
//
// 0x80000000 and not 0xFFFFFFFF: the read path leans on unsigned wrap, and
// `at - 0xFFFFFFFF` is `at + 1`, which is *inside* the window for every offset
// in the first sector — so the sentinel meant to say "nothing held" would have
// claimed to hold the start of the image. Any value more than a sector above
// the largest legal offset works; this one is obviously that.
#define CACHE_NONE 0x80000000u
static uint32_t cache_at = CACHE_NONE;
static bool cache_dirty;

// Which 4 KB sectors the cartridge has written to itself since it was loaded:
// one bit each, 256 of them at 1 MB. This is the difference between the .crt on
// the SD card and what the machine is running, and it is what a .sav is made
// of.
static uint8_t dirty_bits[CART_SIZE_1M / CART_SECTOR_SIZE / 8];

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

    if (banked) {
        device_id = (chip_size == CART_SIZE_128K) ? 0xB5
                  : (chip_size == CART_SIZE_256K) ? 0xB6
                                                  : 0xB7;
    } else {
        device_id = 0;
    }

    // A new image in the slot: nothing of the old one is still held, and
    // nothing of the new one has been written yet. media.c has already flushed
    // the old cart's sector before overwriting the region.
    cache_at = CACHE_NONE;
    cache_dirty = false;
    memset(dirty_bits, 0, sizeof(dirty_bits));

    cart_reset();
}

void cart_reset(void) {
    // RESB does not discard what the cartridge has written, so the held sector
    // goes back to flash rather than being dropped.
    cart_flush();

    bank_reg = 0;
    seq[0] = seq[1] = SEQ_IDLE;
    id_mode[0] = id_mode[1] = false;
    id_any = false;
}

bool cart_flush(void) {
    if (!cache_dirty || cache_at == CACHE_NONE) {
        cache_dirty = false;
        return true;
    }

    bool ok = media_cart_write_sector(cache_at / CART_SECTOR_SIZE);
    cache_dirty = false;
    // Whether it landed or not, the buffer is no longer a faithful copy of the
    // sector under it — saying so is better than serving bytes that may not be
    // in flash.
    cache_at = CACHE_NONE;
    return ok;
}

static void mark_dirty(uint32_t sector) {
    if (sector >= sizeof(dirty_bits) * 8) return;
    dirty_bits[sector >> 3] |= (uint8_t) (1u << (sector & 7));
}

bool cart_sector_dirty(uint32_t sector) {
    if (sector >= sizeof(dirty_bits) * 8) return false;
    return (dirty_bits[sector >> 3] & (uint8_t) (1u << (sector & 7))) != 0;
}

uint32_t cart_sector_count(void) {
    return cart_image_size / CART_SECTOR_SIZE;
}

bool cart_dirty(void) {
    for (uint32_t s = 0; s < cart_sector_count(); s++) {
        if (cart_sector_dirty(s)) return true;
    }
    return false;
}

void cart_clear_dirty(void) {
    memset(dirty_bits, 0, sizeof(dirty_bits));
}

// Bring `at`'s sector into the staging buffer, writing back whichever sector is
// there now. This is where the tens of milliseconds go, and it is why the cache
// exists: a save routine stays inside one sector and pays this once.
static void cache_take(uint32_t at) {
    uint32_t base = at & ~(CART_SECTOR_SIZE - 1);
    if (cache_at == base) return;

    cart_flush();
    memcpy(media_stage(), cart_image + base, CART_SECTOR_SIZE);
    cache_at = base;
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
// The image offset a CPU address reads or writes, and the chip it is on.
// Doing the translation in one place is what puts the JEDEC unlock addresses
// where §3 says they are without any special-casing: CPU $D555 in bank 2
// translates to flash $5555.
static inline uint32_t translate(uint16_t addr, uint8_t *chip_out) {
    if (addr >= CART_FIXED_BASE) {
        *chip_out = 0;
        return chip_size - CART_BANK_SIZE + (addr - CART_FIXED_BASE);
    }

    // The NC high address pins on a 010A or a 020A are exactly this mask, so a
    // register value above the part's bank count aliases down, and values
    // 128-255 mirror 0-127.
    *chip_out = (bank_reg >> 6) & 1;
    return (uint32_t) (bank_reg & bank_mask) * CART_BANK_SIZE +
           (uint32_t) (addr - CART_WINDOW_BASE);
}

// A chip in software ID mode answers every read from its identification pair
// rather than from its array: flash xx00h is the manufacturer, xx01h the
// device.
//
// Deliberately never inlined. cart_read() is RAM-resident, and letting this be
// folded into it would put a mode nothing is normally in into the SRAM the
// Pico 1 has under a kilobyte of.
static __attribute__((noinline)) uint8_t read_id(uint16_t addr) {
    uint8_t chip;
    uint32_t at = translate(addr, &chip);
    if (chip >= chips || !id_mode[chip]) {
        // The other chip is not in ID mode, so it answers normally.
        if (chip >= chips) return 0xFF;
        uint32_t abs = (uint32_t) chip * chip_size + at;
        if (abs - cache_at < CART_SECTOR_SIZE) return media_stage()[abs - cache_at];
        return cart_image[abs];
    }
    return (at & 1) ? device_id : 0xBF;
}

uint8_t __not_in_flash_func(cart_read)(uint16_t addr) {
    if (id_any) return read_id(addr);

    // The fixed region is always the **primary** chip's last 8 KB, whatever
    // the register holds. `chip` never applies to it, which is what keeps the
    // reset and interrupt vectors reachable from every bank.
    uint32_t at;
    if (addr >= CART_FIXED_BASE) {
        at = chip_size - CART_BANK_SIZE + (addr - CART_FIXED_BASE);
    } else {
        uint8_t chip = (bank_reg >> 6) & 1;
        // Selecting the absent U2 — a 512 KB image, or a 1 MB cart with only
        // one chip fitted — reads open bus, which every emulator in the rollout
        // models as $FF.
        if (chip >= chips) return 0xFF;
        at = (uint32_t) chip * chip_size +
             (uint32_t) (bank_reg & bank_mask) * CART_BANK_SIZE +
             (uint32_t) (addr - CART_WINDOW_BASE);
    }

    // The sector the cartridge is writing is in RAM and not yet in flash, so it
    // has to be read from there. Unsigned wrap is doing the work: with
    // cache_at at CACHE_NONE the difference is enormous and the branch is not
    // taken, so nothing has to test for "no sector held".
    if (at - cache_at < CART_SECTOR_SIZE) return media_stage()[at - cache_at];
    return cart_image[at];
}

// Byte program: **clears bits only**, new = old & data. A program that expects
// a 0 bit to come back as a 1 without an erase is a bug on hardware, and
// reproducing it here is how that bug shows up before the chip is real.
//
// Program and erase complete instantly. The emulated CPU on this port is not
// paced to a fixed clock (README, *Hardware Emulation*), so there is no cycle
// count to hang a 20 microsecond busy window on; both standard polling loops
// still terminate, because a read after the write already returns data. The
// consequence is in the README's list of knowing differences: a
// self-programming routine that runs from ROM instead of RAM works here and
// hangs on real hardware, where a busy chip answers every read with status
// bits — the fixed region and its vectors included. 6502-EMULATOR models the
// window and is where that bug is meant to be caught.
static void program(uint8_t chip, uint32_t at, uint8_t value) {
    uint32_t abs = (uint32_t) chip * chip_size + at;
    cache_take(abs);
    media_stage()[abs - cache_at] &= value;
    cache_dirty = true;
    mark_dirty(abs / CART_SECTOR_SIZE);
}

// Sector erase: 4 KB to $FF, aligned on a 4 KB boundary.
static void erase_sector(uint8_t chip, uint32_t at) {
    uint32_t abs = (uint32_t) chip * chip_size + at;
    cache_take(abs);
    memset(media_stage(), 0xFF, CART_SECTOR_SIZE);
    cache_dirty = true;
    mark_dirty(abs / CART_SECTOR_SIZE);
}

// Chip erase: the whole part to $FF, one real 4 KB sector at a time. That is up
// to sixty-four flash erases with Core 1 parked, from inside a single bus
// cycle — seconds, with the screen stopped. It is implemented because the
// command exists and a cartridge may send it, not because it is a reasonable
// thing for one to do.
static void erase_chip(uint8_t chip) {
    cart_flush();

    uint32_t first = (uint32_t) chip * chip_size / CART_SECTOR_SIZE;
    uint32_t count = chip_size / CART_SECTOR_SIZE;

    memset(media_stage(), 0xFF, CART_SECTOR_SIZE);
    for (uint32_t s = 0; s < count; s++) {
        media_cart_write_sector(first + s);
        mark_dirty(first + s);
    }
    cache_at = CACHE_NONE;
    cache_dirty = false;
}

// The command set of §3, over one chip's own address space.
//
// Unlock addresses are compared against the translated flash address masked to
// 15 bits, because the SST39SF0x0 only decodes A14-A0 for commands.
static void command(uint8_t chip, uint32_t at, uint8_t value) {
    uint32_t ua = at & 0x7FFFu;
    uint8_t *st = &seq[chip];

    // F0 at any address returns the chip to read mode. It is only a command
    // when a program is not pending: the byte a program writes may itself be
    // $F0.
    if (*st != SEQ_PROGRAM && value == 0xF0) {
        id_mode[chip] = false;
        id_any = id_mode[0] || id_mode[1];
        *st = SEQ_IDLE;
        return;
    }

    switch (*st) {
        case SEQ_IDLE:
            *st = (ua == 0x5555u && value == 0xAA) ? SEQ_UNLOCK1 : SEQ_IDLE;
            return;
        case SEQ_UNLOCK1:
            *st = (ua == 0x2AAAu && value == 0x55) ? SEQ_UNLOCK2 : SEQ_IDLE;
            return;
        case SEQ_UNLOCK2:
            if (ua != 0x5555u) { *st = SEQ_IDLE; return; }
            switch (value) {
                case 0xA0: *st = SEQ_PROGRAM; return;
                case 0x80: *st = SEQ_ERASE1; return;
                case 0x90:
                    id_mode[chip] = true;
                    id_any = true;
                    *st = SEQ_IDLE;
                    return;
                default: *st = SEQ_IDLE; return;
            }
        case SEQ_PROGRAM:
            *st = SEQ_IDLE;
            program(chip, at, value);
            return;
        case SEQ_ERASE1:
            *st = (ua == 0x5555u && value == 0xAA) ? SEQ_ERASE2 : SEQ_IDLE;
            return;
        case SEQ_ERASE2:
            *st = (ua == 0x2AAAu && value == 0x55) ? SEQ_ERASE3 : SEQ_IDLE;
            return;
        case SEQ_ERASE3:
            *st = SEQ_IDLE;
            if (ua == 0x5555u && value == 0x10) erase_chip(chip);
            else if (value == 0x30) erase_sector(chip, at);
            return;
        default:
            *st = SEQ_IDLE;
            return;
    }
}

// Flash-resident, unlike cart_read().
//
// A cartridge fetches every instruction it executes through the mapper and
// writes to it only to switch banks or to save, so this is thousands of times
// colder than the read path — and putting it in RAM meant putting the whole
// JEDEC state machine there with it, which a Pico 1 cannot pay for.
void cart_write(uint16_t addr, uint8_t data) {
    // A write anywhere in $E000-$FFFF latches the register and **reaches no
    // flash** — WEB is not asserted there, so it is not also a flash write.
    if (addr >= CART_FIXED_BASE) {
        bank_reg = data;
        return;
    }

    // $C000-$DFFF is a flash write at the translated address, on the chip the
    // register selects. It only does anything as part of a JEDEC command
    // sequence; a bare write outside one is swallowed.
    uint8_t chip;
    uint32_t at = translate(addr, &chip);
    if (chip >= chips) return; // U2 is not fitted: the write goes nowhere

    command(chip, at, data);
}
