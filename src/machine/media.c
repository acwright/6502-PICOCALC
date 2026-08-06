#include "media.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"

#include "ff.h"
#include "bios_rom.h"

// Flash layout, working down from the top of the chip:
//
//   last sector      NVRAM (machine/nvram.c owns it)
//   -1 sector        this module's header record
//   -32 KB           ROM image
//   -16 KB           cartridge image
//
// 56 KB in total, against 2 MB on a Pico 1 and 4 MB on a Pico 2, with the
// firmware image (well under 200 KB) growing up from the bottom.
//
// MEDIA_RESERVED_FLASH_BASE (media.h) is this region's bottom edge, exposed
// so another flash-backed store (machine/settings.c) can reserve its own
// sector below it without re-deriving this chain.
#define MEDIA_NVRAM_SECTOR  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define MEDIA_HEADER_OFFSET (MEDIA_NVRAM_SECTOR - FLASH_SECTOR_SIZE)
#define MEDIA_ROM_OFFSET    (MEDIA_HEADER_OFFSET - MEDIA_ROM_SIZE)
#define MEDIA_CART_OFFSET   MEDIA_RESERVED_FLASH_BASE

// 'M','E','D','1' — bumped if the record below ever changes shape, so an old
// one is treated as "nothing loaded" rather than misread.
#define MEDIA_MAGIC 0x3144454Du

typedef struct {
    uint32_t magic;
    uint32_t rom_present;
    uint32_t cart_present;
    char rom_name[MEDIA_NAME_MAX];
    char cart_name[MEDIA_NAME_MAX];
} media_header_t;

#define MEDIA_HEADER_PROGRAM_SIZE \
    ((sizeof(media_header_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)

static media_header_t header;

// One sector on its way to flash. flash_range_program() cannot read from
// flash itself (the SD/FatFs path it came from certainly can't run during the
// write), so the file is staged here a sector at a time.
static uint8_t stage[FLASH_SECTOR_SIZE];

typedef struct {
    uint32_t offset;
    uint32_t length; // of stage[] to program; the whole sector is erased
} flash_op_t;

// Runs with interrupts off and Core 1 parked out of flash by
// flash_safe_execute(), so — like nvram.c's equivalent — it must not call
// anything that lives in flash itself.
static void __not_in_flash_func(media_program)(void *param) {
    const flash_op_t *op = (const flash_op_t *) param;
    flash_range_erase(op->offset, FLASH_SECTOR_SIZE);
    flash_range_program(op->offset, stage, op->length);
}

// Core 1 is parked for the erase, which takes tens of milliseconds; the whole
// point of doing this from the launcher (with the machine stopped and the
// renderer suspended) rather than while the machine runs.
static bool program_stage(uint32_t offset, uint32_t length) {
    flash_op_t op = { offset, length };
    return flash_safe_execute(media_program, &op, 5000) == PICO_OK;
}

static bool write_header(void) {
    memset(stage, 0xFF, sizeof(stage));
    memcpy(stage, &header, sizeof(header));
    return program_stage(MEDIA_HEADER_OFFSET, MEDIA_HEADER_PROGRAM_SIZE);
}

// Copies `size` bytes of an already-open file into flash at `offset`, having
// first skipped `skip` bytes of it. A file shorter than the region leaves the
// rest reading as erased flash ($FF), the same as an EEPROM nobody programmed.
static bool write_image(uint32_t offset, uint32_t size, FIL *file, uint32_t skip,
                        media_progress_fn progress) {
    if (f_lseek(file, skip) != FR_OK) return false;

    for (uint32_t done = 0; done < size; done += FLASH_SECTOR_SIZE) {
        memset(stage, 0xFF, sizeof(stage));
        UINT got = 0;
        if (f_read(file, stage, FLASH_SECTOR_SIZE, &got) != FR_OK) return false;
        if (!program_stage(offset + done, FLASH_SECTOR_SIZE)) return false;
        if (progress) progress(done + FLASH_SECTOR_SIZE, size);
    }
    return true;
}

static void set_name(char *dest, const char *name) {
    strncpy(dest, name ? name : "", MEDIA_NAME_MAX - 1);
    dest[MEDIA_NAME_MAX - 1] = '\0';
}

// The shared shape of both loads: mark the slot empty *before* the image is
// touched, so power lost part way through leaves a slot that is honestly
// empty rather than a header pointing at half an image.
static bool load_image(const char *path, const char *name, uint32_t offset, uint32_t size,
                       uint32_t *present, char *stored_name, media_progress_fn progress) {
    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) return false;

    // A cartridge is mapped at the very top of the address space, so it is the
    // *end* of the image that matters: a .crt is a 32 KB AT28C256 image whose
    // first 16 KB is padding (see the 6502-CRT linker config, and 6502-DEV's
    // loadCartPath, which skips the same 16 KB), while a bare 16 KB build has
    // no padding to skip. Taking the last MEDIA_*_SIZE bytes handles both.
    FSIZE_t file_size = f_size(&file);
    uint32_t skip = (file_size > size) ? (uint32_t) (file_size - size) : 0;

    *present = 0;
    set_name(stored_name, "");
    bool ok = write_header();

    if (ok) ok = write_image(offset, size, &file, skip, progress);
    f_close(&file);
    if (!ok) return false;

    *present = 1;
    set_name(stored_name, name);
    return write_header();
}

void media_init(void) {
    const media_header_t *stored = (const media_header_t *) (XIP_BASE + MEDIA_HEADER_OFFSET);

    if (stored->magic == MEDIA_MAGIC) {
        memcpy(&header, stored, sizeof(header));
        header.rom_name[MEDIA_NAME_MAX - 1] = '\0';
        header.cart_name[MEDIA_NAME_MAX - 1] = '\0';
    } else {
        memset(&header, 0, sizeof(header));
        header.magic = MEDIA_MAGIC;
    }
}

const uint8_t *media_rom(void) {
    return header.rom_present ? (const uint8_t *) (XIP_BASE + MEDIA_ROM_OFFSET) : bios_rom;
}

const uint8_t *media_cart(void) {
    return header.cart_present ? (const uint8_t *) (XIP_BASE + MEDIA_CART_OFFSET) : NULL;
}

const char *media_rom_name(void) {
    return header.rom_present ? header.rom_name : "";
}

const char *media_cart_name(void) {
    return header.cart_present ? header.cart_name : "";
}

bool media_load_rom(const char *path, const char *name, media_progress_fn progress) {
    return load_image(path, name, MEDIA_ROM_OFFSET, MEDIA_ROM_SIZE,
                      &header.rom_present, header.rom_name, progress);
}

bool media_load_cart(const char *path, const char *name, media_progress_fn progress) {
    return load_image(path, name, MEDIA_CART_OFFSET, MEDIA_CART_SIZE,
                      &header.cart_present, header.cart_name, progress);
}

// Ejecting only clears the flag — the image is left in flash to be erased by
// whatever is loaded next, since erasing it now would cost the same time for
// no benefit.
void media_eject_cart(void) {
    if (!header.cart_present) return;
    header.cart_present = 0;
    set_name(header.cart_name, "");
    write_header();
}

void media_restore_builtin_rom(void) {
    if (!header.rom_present) return;
    header.rom_present = 0;
    set_name(header.rom_name, "");
    write_header();
}
