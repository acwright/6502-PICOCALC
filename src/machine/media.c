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

// 'M','E','D','2' — bumped if the record below ever changes shape, so an old
// one is treated as "nothing loaded" rather than misread.
//
// The bump from 'M','E','D','1' (firmware up to v1.0.3) is doing a second job
// as well as its first. A UF2 rewrites the firmware at the bottom of flash and
// leaves this header and the ROM image at the top untouched, so a machine that
// had ever loaded a ROM from the SD card carried on booting that ROM after an
// update — which is how a v1.0.3 flash, whose whole point was the reissued
// BIOS v1.6, came up on a BIOS v1.5 loaded months earlier. Treating a v1
// record as having no ROM in the socket puts every such machine back on the
// built-in BIOS, which is what flashing a firmware release means. The image
// itself is left in flash exactly as media_restore_builtin_rom() leaves it,
// and re-loading it is two keystrokes in the launcher.
//
// A cartridge is a different matter and is carried across (see media_init()):
// it is not a BIOS, it is not what a firmware update replaces, and losing one
// silently would be the same kind of surprise in the other direction.
#define MEDIA_MAGIC    0x3244454Du
#define MEDIA_MAGIC_V1 0x3144454Du

typedef struct {
    uint32_t magic;
    uint32_t rom_present;
    uint32_t cart_present;
    char rom_name[MEDIA_NAME_MAX];
    char cart_name[MEDIA_NAME_MAX];
    // What the built-in BIOS hashed to when this ROM was put in the socket, so
    // a later firmware can tell that it now ships a different one (see
    // media_rom_override_stale()). Meaningless unless rom_present.
    uint32_t builtin_checksum;
} media_header_t;

// The v1 record, kept only so its cartridge can be migrated rather than
// dropped. Identical to the above bar the trailing checksum.
typedef struct {
    uint32_t magic;
    uint32_t rom_present;
    uint32_t cart_present;
    char rom_name[MEDIA_NAME_MAX];
    char cart_name[MEDIA_NAME_MAX];
} media_header_v1_t;

#define MEDIA_HEADER_PROGRAM_SIZE \
    ((sizeof(media_header_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)

static media_header_t header;

// Worked out once in media_init() and then just reported; see
// media_rom_override_stale().
static bool rom_override_stale;

// A cheap 32-bit fingerprint of a 32 KB ROM image. Deliberately not the plain
// byte sum the other flash-backed stores use (nvram.c, settings.c): theirs
// guards a short record against a half-finished write, where this has to tell
// two whole BIOS builds apart, and over 32 KB a bare sum is far too easy to
// land on twice — any change that moves one byte up and another down cancels.
// Mixing the position in costs one multiply per byte, 32768 of them, once at
// boot and once per ROM load. A hash proper would be a lot of code for a
// question this small.
static uint32_t rom_checksum(const uint8_t *rom) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < MEDIA_ROM_SIZE; i++) sum = sum * 31u + rom[i];
    return sum;
}

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

    memset(&header, 0, sizeof(header));
    header.magic = MEDIA_MAGIC;

    if (stored->magic == MEDIA_MAGIC) {
        memcpy(&header, stored, sizeof(header));
        header.rom_name[MEDIA_NAME_MAX - 1] = '\0';
        header.cart_name[MEDIA_NAME_MAX - 1] = '\0';
    } else if (stored->magic == MEDIA_MAGIC_V1) {
        // Firmware up to v1.0.3. The ROM override goes (see MEDIA_MAGIC), the
        // cartridge stays.
        const media_header_v1_t *old = (const media_header_v1_t *) stored;
        header.cart_present = old->cart_present;
        memcpy(header.cart_name, old->cart_name, MEDIA_NAME_MAX);
        header.cart_name[MEDIA_NAME_MAX - 1] = '\0';
        // Nothing is written back here on purpose. Rewriting the header would
        // mean erasing a sector at boot, with Core 1 already running the
        // renderer out of flash and nothing yet drawn on the panel — a flash
        // write is what the launcher does with the machine stopped, not
        // something to do unasked while the machine is coming up. Carrying the
        // migration in RAM costs nothing: it produces the same answer on every
        // boot, and the next header the launcher writes is a v2 one.
    }

    // Whether the firmware has started shipping a different built-in BIOS than
    // the one this override was chosen over. A record written before
    // the checksum existed cannot get here — the magic bump above already
    // cleared its ROM — so a zero checksum only ever means a ROM loaded by a
    // build that did not stamp one, which no released build is.
    rom_override_stale = header.rom_present && header.builtin_checksum != rom_checksum(bios_rom);
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

bool media_rom_override_stale(void) {
    return rom_override_stale;
}

// Scanned out of the image rather than carried as a #define beside it, which
// would be one more thing to forget when the bytes are re-embedded. The BIOS
// banner reads "-- 6502 BIOS v1.6 --"; this returns the "v1.6", or "" for an
// image that words it differently.
const char *media_builtin_rom_version(void) {
    static const char needle[] = "BIOS v";
    static char version[8];
    static bool searched;

    if (searched) return version;
    searched = true;

    const uint32_t needle_len = sizeof(needle) - 1;
    for (uint32_t i = 0; i + needle_len <= MEDIA_ROM_SIZE; i++) {
        if (memcmp(&bios_rom[i], needle, needle_len) != 0) continue;

        // From the 'v' of "v1.6" up to the first character that is not part of
        // a version number.
        const uint8_t *from = &bios_rom[i + needle_len - 1];
        uint32_t room = MEDIA_ROM_SIZE - (i + needle_len - 1);
        uint32_t n = 0;
        while (n < room && n < sizeof(version) - 1 &&
               (from[n] == 'v' || from[n] == '.' || (from[n] >= '0' && from[n] <= '9'))) {
            version[n] = (char) from[n];
            n++;
        }
        version[n] = '\0';
        break;
    }
    return version;
}

bool media_load_rom(const char *path, const char *name, media_progress_fn progress) {
    // Stamped before the load, not after, so it is in both headers load_image()
    // writes. This is what a later firmware compares against to know that the
    // BIOS it ships is no longer the one this ROM was chosen over.
    header.builtin_checksum = rom_checksum(bios_rom);

    bool ok = load_image(path, name, MEDIA_ROM_OFFSET, MEDIA_ROM_SIZE,
                         &header.rom_present, header.rom_name, progress);
    // Whatever the built-in BIOS is now, it is the one this choice was made
    // against — there is nothing left to warn about either way.
    rom_override_stale = false;
    return ok;
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
    header.builtin_checksum = 0;
    set_name(header.rom_name, "");
    rom_override_stale = false;
    write_header();
}
