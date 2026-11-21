// machine/storage.c, the CompactFlash model, against what a real card does.
//
// The reference is the SD controller firmware that answers on the bus as a
// card does (6502-COB `Firmware/SCP Controller/core/ata.c` and `identify.c`),
// and 6502-EMULATOR's Storage.ts follows the same card. Each check below is one
// of the five places 6502-PICOCALC#1 found the model differing from a card, or
// one of the two faults found with them.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "storage.h"

// ---------------------------------------------------------------------
// FatFs over a sparse image: a sector holds memory only once written
// ---------------------------------------------------------------------

#define MAX_SECTORS (STORAGE_SECTOR_COUNT + 2048)
static uint8_t *sectors[MAX_SECTORS];
static bool exists;
static int fail_writes; // >0: the next f_write fails (an SD card I/O error)

static void image_clear(void) {
    for (uint32_t i = 0; i < MAX_SECTORS; i++) { free(sectors[i]); sectors[i] = NULL; }
    exists = false;
}

FRESULT f_open(FIL *fp, const char *path, uint8_t mode) {
    (void) path;
    if (!(mode & FA_CREATE_NEW) && !exists) return FR_NO_FILE;
    if (mode & FA_CREATE_NEW) { exists = true; fp->size = 0; }
    fp->pos = 0;
    return FR_OK;
}
FRESULT f_close(FIL *fp) { (void) fp; return FR_OK; }
FRESULT f_sync(FIL *fp) { (void) fp; return FR_OK; }
FRESULT f_lseek(FIL *fp, FSIZE_t ofs) { fp->pos = ofs; return FR_OK; }

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
    uint8_t *dst = buff;
    UINT n = 0;
    while (n < btr && fp->pos < fp->size) {
        uint32_t s = (uint32_t) (fp->pos / STORAGE_SECTOR_SIZE), o = (uint32_t) (fp->pos % STORAGE_SECTOR_SIZE);
        dst[n++] = sectors[s] ? sectors[s][o] : 0;
        fp->pos++;
    }
    *br = n;
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
    if (fail_writes > 0) { fail_writes--; *bw = 0; return FR_DISK_ERR; }
    const uint8_t *src = buff;
    for (UINT n = 0; n < btw; n++, fp->pos++) {
        uint32_t s = (uint32_t) (fp->pos / STORAGE_SECTOR_SIZE), o = (uint32_t) (fp->pos % STORAGE_SECTOR_SIZE);
        if (s >= MAX_SECTORS) { *bw = n; return FR_DISK_ERR; }
        if (!sectors[s]) {
            if (!src[n]) continue; // a zero into an unwritten sector: still zero
            sectors[s] = calloc(1, STORAGE_SECTOR_SIZE);
        }
        sectors[s][o] = src[n];
    }
    if (fp->pos > fp->size) fp->size = fp->pos;
    *bw = btw;
    return FR_OK;
}

static uint8_t image_byte(uint32_t sector, uint32_t offset) {
    return sectors[sector] ? sectors[sector][offset] : 0;
}

static void image_poke(uint32_t sector, uint32_t offset, uint8_t value) {
    if (!sectors[sector]) sectors[sector] = calloc(1, STORAGE_SECTOR_SIZE);
    sectors[sector][offset] = value;
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
    if (got != want) { failures++; printf("  FAIL  %s: got $%02X, wanted $%02X\n", what, got, want); }
}

// A fresh card: a blank one-bank image, created by storage_init(), and reset.
static void fresh(void) {
    image_clear();
    storage_init();
    storage_reset();
}

static void command(uint32_t lba, uint8_t count, uint8_t cmd) {
    storage_write(2, count);
    storage_write(3, (uint8_t) lba);
    storage_write(4, (uint8_t) (lba >> 8));
    storage_write(5, (uint8_t) (lba >> 16));
    storage_write(6, (uint8_t) (0xE0 | ((lba >> 24) & 0x0F)));
    storage_write(7, cmd);
}

static uint8_t status(void) { return storage_read(7); }

// ---------------------------------------------------------------------

static void identify_is_laid_out_as_a_card_sends_it(void) {
    fresh();
    storage_write(7, 0xEC);
    uint8_t id[512];
    for (int i = 0; i < 512; i++) id[i] = storage_read(0);

    check_byte(id[0], 0x8A, "IDENTIFY word 0, low byte ($848A)");
    check_byte(id[1], 0x84, "IDENTIFY word 0, high byte ($848A)");

    // A driver swaps each word of a string back
    char text[41];
    struct { int from, words; const char *want, *what; } strings[] = {
        {10, 10, "ACWD6502EMUCF1010101", "serial number, words 10-19"},
        {23, 4, "1.0     ", "firmware revision, words 23-26"},
        {27, 20, "ACWD6502EMUCF                           ", "model, words 27-46"},
    };
    for (int k = 0; k < 3; k++) {
        int n = strings[k].words * 2;
        for (int i = 0; i < n; i++) text[i] = (char) id[strings[k].from * 2 + (i ^ 1)];
        text[n] = 0;
        if (strcmp(text, strings[k].want) != 0) printf("        got \"%s\"\n", text);
        check(strcmp(text, strings[k].want) == 0, strings[k].what);
    }
    check_byte(id[20], 'C', "the pair \"AC\" is sent C first");
    check_byte(id[21], 'A', "the pair \"AC\" is sent A second");

    uint32_t w7 = id[14] | id[15] << 8, w8 = id[16] | id[17] << 8;
    check((w7 << 16 | w8) == STORAGE_SECTOR_COUNT, "words 7-8, sectors per card, high word first");
    uint32_t w60 = id[120] | id[121] << 8, w61 = id[122] | id[123] << 8;
    check((w61 << 16 | w60) == STORAGE_SECTOR_COUNT, "words 60-61, LBA sectors, low word first");
}

static void ready_reads_rdy_and_dsc(void) {
    fresh();
    check_byte(status(), 0x50, "status when ready");
    storage_write(7, 0xEF);
    check_byte(status(), 0x50, "status after SET FEATURES");
    storage_write(7, 0xFF);
    check_byte(status(), 0x51, "status after an unknown command: DSC kept");
}

static void a_count_of_0_moves_256_sectors(void) {
    fresh();
    for (uint32_t s = 0; s < 257; s++) { image_poke(s, 0, (uint8_t) s); image_poke(s, 1, (uint8_t) (s >> 8)); }
    command(0, 0, 0x20);
    int moved = 0, wrong = 0;
    while (status() & 0x08) {
        uint8_t lo = storage_read(0), hi = storage_read(0);
        if ((lo | hi << 8) != moved) wrong++;
        for (int i = 2; i < 512; i++) storage_read(0);
        moved++;
    }
    check(moved == 256, "a read with a count of 0 moves 256 sectors");
    check(wrong == 0, "each sector of the 256 is the right one");

    command(0, 0, 0x30);
    moved = 0;
    while ((status() & 0x08) && moved < 300) {
        for (int i = 0; i < 512; i++) storage_write(0, 0xA5);
        moved++;
    }
    check(moved == 256, "a write with a count of 0 takes 256 sectors");
    check_byte(image_byte(255, 511), 0xA5, "the 256th sector written");
    check_byte(image_byte(256, 1), 0x01, "and not the 257th");
}

static void a_command_mid_transfer_ends_it(void) {
    fresh();
    image_poke(0, 0, 0x11);
    image_poke(0, 1, 0x22);
    command(0, 1, 0x20);
    check_byte(storage_read(0), 0x11, "the read's first byte");
    storage_write(7, 0x20);
    check_byte(status(), 0x51, "a command mid-read: ERR, DRQ clear");
    check_byte(storage_read(1), 0x04, "a command mid-read: ABRT");
    check_byte(storage_read(0), 0x00, "the old read moves no more bytes");
    storage_write(7, 0x20);
    check_byte(status(), 0x58, "the next command starts afresh");
    check_byte(storage_read(0), 0x11, "from the first byte");

    fresh();
    command(3, 1, 0x30);
    for (int i = 0; i < 100; i++) storage_write(0, 0x77);
    storage_write(7, 0xEC);
    check_byte(status(), 0x51, "a command mid-write: ERR, DRQ clear");
    for (int i = 0; i < 512; i++) storage_write(0, 0x77);
    check_byte(image_byte(3, 0), 0x00, "the old write puts nothing on the card");

    fresh();
    storage_write(7, 0xEC);
    storage_read(0);
    storage_write(7, 0xEF);
    check_byte(status(), 0x51, "a command mid-IDENTIFY: ERR, DRQ clear");
    check_byte(storage_read(0), 0x00, "the old IDENTIFY moves no more bytes");
}

static void the_data_register_is_ignored_outside_a_transfer(void) {
    fresh();
    for (int i = 0; i < 512; i++) storage_write(0, 0x99);
    check_byte(image_byte(0, 0), 0x00, "512 data writes with no command put nothing on the card");
}

static void erase_erases_count_sectors(void) {
    fresh();
    for (uint32_t s = 0; s < 300; s++) image_poke(s, 0, 0xEE);
    command(10, 3, 0xC0);
    check_byte(status(), 0x50, "erase of 3: no error");
    check_byte(image_byte(9, 0), 0xEE, "erase leaves the sector before");
    check_byte(image_byte(10, 0), 0x00, "erase clears the first");
    check_byte(image_byte(12, 0), 0x00, "erase clears the third");
    check_byte(image_byte(13, 0), 0xEE, "erase leaves the sector after");
    command(20, 0, 0xC0);
    check_byte(image_byte(275, 0), 0x00, "erase of 0 clears 256 sectors");
    check_byte(image_byte(276, 0), 0xEE, "and not the 257th");
}

static void a_transfer_stops_at_the_end_of_the_card(void) {
    const uint32_t last = STORAGE_SECTOR_COUNT - 1;
    fresh();
    command(last, 2, 0x20);
    for (int i = 0; i < 512; i++) storage_read(0);
    check_byte(status() & 0x09, 0x01, "a read past the end: ERR, DRQ clear");
    check_byte(storage_read(1), 0x14, "a read past the end: ABRT and IDNF");

    command(last, 2, 0x30);
    for (int i = 0; i < 1024; i++) storage_write(0, 0x55);
    check_byte(status() & 0x09, 0x01, "a write past the end: ERR, DRQ clear");
    check_byte(image_byte(last, 511), 0x55, "the last sector is written");
    check(sectors[last + 1] == NULL, "and nothing past it");

    command(last, 2, 0xC0);
    check_byte(status() & 0x01, 0x01, "an erase past the end: ERR");
    check_byte(image_byte(last, 0), 0x00, "the last sector is erased");
}

int main(void) {
    identify_is_laid_out_as_a_card_sends_it();
    ready_reads_rdy_and_dsc();
    a_count_of_0_moves_256_sectors();
    a_command_mid_transfer_ends_it();
    the_data_register_is_ignored_outside_a_transfer();
    erase_erases_count_sectors();
    a_transfer_stops_at_the_end_of_the_card();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
