// machine/cart.c against the rollout's shared fixtures.
//
// 6502-VCS PLAN.md §§1-3 is the normative contract and
// `Firmware/FH Programmer/host/test/fixtures/` is the shared oracle: this test,
// 6502-EMULATOR's, the DB Emulator's and `6502-flash`'s own all check against
// **those files** rather than against each other, which is the only way four
// implementations of one contract end up agreeing rather than sharing a bug.
//
// The fixture's checksum is quoted below, so a stale copy fails here rather
// than passing against the wrong bytes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cart.h"

// ---------------------------------------------------------------------
// The Pico's flash, as an array
// ---------------------------------------------------------------------

static uint8_t flash[CART_SIZE_1M];
static uint8_t stage[CART_SECTOR_SIZE];
static int sector_writes;

uint8_t *media_stage(void) { return stage; }

bool media_cart_write_sector(uint32_t sector) {
    if ((sector + 1) * CART_SECTOR_SIZE > sizeof(flash)) return false;
    memcpy(&flash[sector * CART_SECTOR_SIZE], stage, CART_SECTOR_SIZE);
    sector_writes++;
    return true;
}

// ---------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------

static int checks, failures;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) { failures++; printf("  FAIL  %s\n", what); }
}

static void check_byte(uint8_t got, uint8_t want, const char *what) {
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL  %s: got $%02X, wanted $%02X\n", what, got, want);
    }
}

static uint32_t crc32_of(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t) (-(int32_t) (crc & 1)));
    }
    return ~crc;
}

static uint32_t load_fixture(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(1); }
    size_t got = fread(flash, 1, sizeof(flash), f);
    fclose(f);
    return (uint32_t) got;
}

// The JEDEC unlock addresses are only reachable from the banks that put them
// where the part decodes them: CPU $D555 in bank 2 translates to flash $5555,
// and CPU $CAAA in bank 1 to $2AAA. This is the sequence a cartridge writes.
static void unlock(void) {
    cart_write(0xE000, 0x02);
    cart_write(0xD555, 0xAA);
    cart_write(0xE000, 0x01);
    cart_write(0xCAAA, 0x55);
    cart_write(0xE000, 0x02);
}

static void program_byte(uint8_t bank, uint16_t addr, uint8_t value) {
    unlock();
    cart_write(0xD555, 0xA0);
    cart_write(0xE000, bank);
    cart_write(addr, value);
}

static void erase_sector(uint8_t bank, uint16_t addr) {
    unlock();
    cart_write(0xD555, 0x80);
    unlock();
    cart_write(0xE000, bank);
    cart_write(addr, 0x30);
}

int main(void) {
    uint32_t size = load_fixture("fixtures/banked-128K.crt");
    check(size == CART_SIZE_128K, "banked-128K.crt is 131,072 bytes");
    check(crc32_of(flash, size) == 0xE0E4F83Cu, "banked-128K.crt is e0e4f83c");

    printf("\xc2\xa7""1 sizes\n");
    check(cart_size_valid(0x4000) && !cart_size_banked(0x4000), "16 KB is a flat cart");
    check(cart_size_valid(CART_SIZE_FLAT) && !cart_size_banked(CART_SIZE_FLAT), "32 KB is a flat cart");
    check(cart_size_valid(CART_SIZE_128K) && cart_size_banked(CART_SIZE_128K), "128K is banked");
    check(cart_size_valid(CART_SIZE_1M) && cart_size_banked(CART_SIZE_1M), "1M is banked");
    check(!cart_size_valid(0x9000), "a length between the two is not a cartridge");
    check(!cart_size_valid(0x30000), "and neither is one between two banked sizes");
    check(strcmp(cart_size_name(CART_SIZE_512K), "512K") == 0, "512K is named");

    // Rule 3: the name must agree with the size, and a disagreement is a
    // warning and never a refusal.
    check(cart_name_agrees("Cart-512K.crt", CART_SIZE_512K), "a name that agrees");
    check(cart_name_agrees("Cart-VDP-1M.crt", CART_SIZE_1M), "-VDP comes before the target");
    check(cart_name_agrees("Cart.crt", CART_SIZE_FLAT), "no suffix claims the flat cart");
    check(!cart_name_agrees("Cart-512K.crt", CART_SIZE_256K), "a name that does not");
    check(!cart_name_agrees("Cart.crt", CART_SIZE_128K), "a banked image under a flat name");
    check(cart_name_agrees("whatever.bin", 0x9000), "an illegal size is someone else's error");

    printf("\xc2\xa7""2 the mapper contract\n");
    cart_init(flash, size);
    check(cart_banked(), "the fixture is banked");
    check_byte(cart_bank(), 0x00, "the register starts cleared");

    // Every byte of a bank is that bank's own number, so reading the wrong bank
    // is a wrong byte rather than a hang.
    for (uint8_t reg = 0x00; reg <= 0x0E; reg++) {
        cart_write(0xE000, reg);
        check_byte(cart_read(0xD000), reg, "an ordinary bank select");
        check_byte(cart_bank(), reg, "the register reads back");
    }

    // A 010A has no A17/A18, so high bank bits alias down.
    cart_write(0xE000, 0x10);
    check_byte(cart_read(0xD000), 0x00, "$10 aliases to bank 0");
    cart_write(0xE000, 0x2E);
    check_byte(cart_read(0xD000), 0x0E, "$2E aliases to bank 14");

    // Bit 7 is latched and goes nowhere: 128-255 mirror 0-127.
    cart_write(0xE000, 0x85);
    check_byte(cart_read(0xD000), 0x05, "$85 mirrors bank 5");
    check_byte(cart_bank(), 0x85, "and the register still holds $85");

    // B6 selects U2, which a 128K cart has not got: open bus, modelled as $FF.
    cart_write(0xE000, 0x40);
    check_byte(cart_read(0xD000), 0xFF, "the absent U2 reads open bus");
    check_byte(cart_read(0xC000), 0xFF, "open bus all the way across the window");

    // The fixed region ignores the register entirely, which is what keeps the
    // vectors reachable from every bank.
    for (unsigned reg = 0x00; reg <= 0xFF; reg += 0x11) {
        cart_write(0xE000, (uint8_t) reg);
        check_byte(cart_read(0xF000), 0x0F, "the fixed region ignores the register");
    }
    cart_write(0xE000, 0x00);
    for (int i = 0; i < 16; i++) {
        check_byte(cart_read(0xE000 + i), (uint8_t) "AC6502 FIXED $0F"[i], "the fixed bank's tag");
    }

    // The 16-byte tags at each end of a bank catch an offset error inside one.
    cart_write(0xE000, 0x03);
    for (int i = 0; i < 16; i++) {
        check_byte(cart_read(0xC000 + i), (uint8_t) "AC6502 BANK $03\n"[i], "bank 3's opening tag");
    }
    for (int i = 0; i < 16; i++) {
        check_byte(cart_read(0xDFF0 + i), (uint8_t) "END OF BANK $03\n"[i], "bank 3's closing tag");
    }

    // The first bytes of the image, which the "nothing held" sentinel used to
    // claim were in the sector cache.
    cart_write(0xE000, 0x00);
    for (int i = 0; i < 16; i++) {
        check_byte(cart_read(0xC000 + i), (uint8_t) "AC6502 BANK $00\n"[i], "bank 0's opening tag");
    }

    // RESB clears the register.
    cart_write(0xE000, 0x07);
    cart_reset();
    check_byte(cart_bank(), 0x00, "reset clears the bank register");
    check_byte(cart_read(0xD000), 0x00, "and the window is bank 0 again");

    printf("\xc2\xa7""3 the flash command set\n");
    cart_init(flash, size);

    // A byte programmed into the live window reads straight back, out of the
    // sector cache, without a flash write having happened yet. $F0 as the value
    // is deliberate: it is also the reset-to-read-mode command, and must not be
    // taken as one while a program is pending.
    sector_writes = 0;
    program_byte(0x0E, 0xC010, 0xF0);
    cart_write(0xE000, 0x0E);
    check_byte(cart_read(0xC010), 0x00, "a programmed byte reads straight back");
    check_byte(cart_read(0xC011), 0x0E, "and its neighbour is untouched");
    check(sector_writes == 0, "and nothing has gone to flash yet");
    check(cart_dirty(), "the cartridge has written to itself");
    check(cart_sector_dirty(28), "sector 28, which is bank 14's first 4 KB");

    // Byte program clears bits only.
    program_byte(0x0E, 0xC010, 0xFF);
    cart_write(0xE000, 0x0E);
    check_byte(cart_read(0xC010), 0x00, "program clears bits only");
    check(sector_writes == 0, "and stayed in the same sector");

    // Moving to another sector writes the held one back, and both read right
    // afterwards — the one thing a write-back cache can get wrong.
    program_byte(0x0E, 0xD010, 0x0F);
    check(sector_writes == 1, "moving sector wrote the old one back");
    cart_write(0xE000, 0x0E);
    check_byte(cart_read(0xD010), 0x0E & 0x0F, "the new sector's byte");
    check_byte(cart_read(0xC010), 0x00, "and the flushed sector still reads back");
    check(cart_sector_dirty(29), "sector 29 is dirty too");

    // An explicit flush puts everything in flash and leaves the reads alone.
    check(cart_flush(), "flush");
    check(sector_writes == 2, "the second sector went out");
    check_byte(cart_read(0xD010), 0x0E & 0x0F, "still reads back from flash");
    check_byte(flash[29 * CART_SECTOR_SIZE + 0x10], 0x0E & 0x0F, "and it is really there");

    // A flush gives the staging buffer back. machine/media.c stages images
    // through the same 4 KB, so a sector still claimed after a flush would have
    // the read path serving whatever media.c put there next — which is the
    // invariant the shared buffer rests on.
    program_byte(0x03, 0xC800, 0x00);
    cart_flush();
    memset(stage, 0x5A, sizeof(stage)); // media.c, staging something else
    cart_write(0xE000, 0x03);
    check_byte(cart_read(0xC800), 0x00, "the programmed byte, now from flash");
    check_byte(cart_read(0xC810), 0x03, "and its sector is not the staging buffer");

    // Sector erase: 4 KB to $FF on a 4 KB boundary.
    erase_sector(0x0E, 0xC010);
    cart_write(0xE000, 0x0E);
    check_byte(cart_read(0xC010), 0xFF, "the erased sector is $FF");
    check_byte(cart_read(0xCFFF), 0xFF, "to its last byte");
    check_byte(cart_read(0xD000), 0x0E, "and the next sector is untouched");
    check_byte(cart_read(0xD010), 0x0E & 0x0F, "including the byte programmed into it");

    // Software ID: flash xx00h is the manufacturer, xx01h the device. An
    // SST39SF010A is $BF/$B5, derived from CHIP_SIZE so this and `id` over the
    // Flash Helper agree.
    unlock();
    cart_write(0xD555, 0x90);
    cart_write(0xE000, 0x03);
    check_byte(cart_read(0xC000), 0xBF, "ID mode: the manufacturer");
    check_byte(cart_read(0xC001), 0xB5, "ID mode: an SST39SF010A");
    check_byte(cart_read(0xE000), 0xBF, "ID mode answers in the fixed region too");
    cart_write(0xC000, 0xF0);
    check_byte(cart_read(0xC000), (uint8_t) 'A', "and $F0 leaves it");
    check_byte(cart_read(0xE000), (uint8_t) 'A', "in the fixed region as well");

    // Bank 15 is the fixed bank, so with the register on $0F the same 8 KB is
    // visible through the window and the fixed region at once.
    cart_write(0xE000, 0x0F);
    check_byte(cart_read(0xC010), cart_read(0xE010), "bank 15 is the fixed bank");
    program_byte(0x0F, 0xC010, 0x00);
    cart_write(0xE000, 0x0F);
    check_byte(cart_read(0xC010), 0x00, "programmed through the window");
    check_byte(cart_read(0xE010), 0x00, "and visible in the fixed region");
    cart_flush();
    check_byte(cart_read(0xE010), 0x00, "and after a flush, from flash");

    // A bare flash write outside a sequence is swallowed.
    cart_write(0xE000, 0x03);
    cart_write(0xC020, 0x00);
    check_byte(cart_read(0xC020), 0x03, "a bare flash write does nothing");

    // Chip erase.
    unlock();
    cart_write(0xD555, 0x80);
    unlock();
    cart_write(0xD555, 0x10);
    cart_write(0xE000, 0x03);
    check_byte(cart_read(0xC000), 0xFF, "chip erase: the window");
    check_byte(cart_read(0xE000), 0xFF, "chip erase: the fixed region");
    check(cart_sector_dirty(0) && cart_sector_dirty(31), "every sector is dirty after one");

    cart_clear_dirty();
    check(!cart_dirty(), "and the record clears once it has been written out");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
