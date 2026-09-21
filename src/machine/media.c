#include "media.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"

#include "ff.h"
#include "bios_rom.h"
#include "cart.h"

// Flash layout, working down from the top of the chip:
//
//   last sector      NVRAM (machine/nvram.c owns it)
//   -1 sector        this module's header record
//   -32 KB           ROM image
//   -16 KB           unused — where the old flat cartridge slot was
//   -1 sector        settings (machine/settings.c owns it), frozen where it is
//   below            cartridge region, 1 MB
//
// Everything above the cartridge region is at the offset it has always been
// at, and the cartridge region is the only thing that grows. The sixteen
// stranded kilobytes are the price of that, and a fair one: the alternative was
// reordering the block and migrating the settings record, which is real work on
// persisted state, on a board where it cannot be tested from a fresh flash.
//
// MEDIA_RESERVED_FLASH_BASE (media.h) is the whole region's bottom edge. The
// hazard the note there warns about is still real for anything else that wants
// a sector: take it from below the base rather than re-deriving this chain.
#define MEDIA_NVRAM_SECTOR  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define MEDIA_HEADER_OFFSET (MEDIA_NVRAM_SECTOR - FLASH_SECTOR_SIZE)
#define MEDIA_ROM_OFFSET    (MEDIA_HEADER_OFFSET - MEDIA_ROM_SIZE)
#define MEDIA_CART_OFFSET   MEDIA_RESERVED_FLASH_BASE

// A firmware that grew into the cartridge region would corrupt a cart rather
// than fail to link, so it fails to compile instead. Half a megabyte is many
// times what this firmware has ever needed.
_Static_assert(MEDIA_RESERVED_FLASH_BASE >= 512 * 1024,
               "the reserved region leaves too little flash for the firmware");

// The settings sector's offset is frozen at the value it used to be derived as,
// and this is the derivation, kept solely to prove the literals in media.h are
// that value and not a value near it. Getting it wrong by one sector orphans
// every existing user's settings on their next boot, silently — which is the
// one failure in this whole change that a board cannot be tested out of.
_Static_assert(MEDIA_SETTINGS_FLASH_OFFSET ==
                   PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE - MEDIA_ROM_SIZE -
                       MEDIA_CART_WINDOW - FLASH_SECTOR_SIZE,
               "the frozen settings offset is not the offset it froze at");

// 'M','E','D','3' — bumped if the record below ever changes shape, so an old
// one is treated as "nothing loaded" rather than misread.
//
// The bump from 'M','E','D','2' is the cartridge region moving. The image used
// to sit at the old slot's offset and now sits a megabyte lower, so a cart
// loaded before this firmware cannot be where the header says it is, and
// reading it as "nothing loaded" is the only honest answer. The user reloads it
// from the SD card in a few seconds. The record also grows — longer names, and
// the cartridge's size — and folding that into the same bump is why there is
// one migration and not two.
//
// **The ROM image has not moved**: it is counted down from the top of the chip
// and everything above the cartridge region is where it was. So a ROM override
// is carried across (see media_init()), which is what a chip in a socket should
// do across a firmware update. The bump from 'M','E','D','1' deliberately did
// not carry one, because that update's whole point was a reissued BIOS; this
// one's is not.
//
// A 'M','E','D','1' record is from firmware up to v1.0.3, whose ROM override
// was already dropped by the last bump and whose cartridge has now moved. There
// is nothing left in it to carry, so it is not read at all.
#define MEDIA_MAGIC    0x3344454Du
#define MEDIA_MAGIC_V2 0x3244454Du

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
    // The length of the file the cartridge was loaded from, which is what
    // decides how it is mapped. Meaningless unless cart_present.
    uint32_t cart_size;
} media_header_t;

// The v2 record, kept only so a ROM override can be carried across rather than
// dropped. Its names are shorter and it has no cart_size.
#define MEDIA_NAME_MAX_V2 16

typedef struct {
    uint32_t magic;
    uint32_t rom_present;
    uint32_t cart_present;
    char rom_name[MEDIA_NAME_MAX_V2];
    char cart_name[MEDIA_NAME_MAX_V2];
    uint32_t builtin_checksum;
} media_header_v2_t;

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
//
// It is also machine/cart.c's write-back sector cache — see media_stage() in
// media.h for why there is one of these and not two, and for the rule that
// keeps the two uses out of each other's way.
static uint8_t stage[FLASH_SECTOR_SIZE];

// The chip's erase unit and the Pico's are the same 4 KB, which is what lets
// one emulated sector be one real one.
_Static_assert(CART_SECTOR_SIZE == FLASH_SECTOR_SIZE,
               "the cartridge sector cache assumes the two erase units match");

uint8_t *media_stage(void) {
    return stage;
}

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

bool media_cart_write_sector(uint32_t sector) {
    if (sector >= MEDIA_CART_MAX / FLASH_SECTOR_SIZE) return false;
    return program_stage(MEDIA_CART_OFFSET + sector * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
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
static bool load_image(FIL *file, const char *name, uint32_t offset, uint32_t size,
                       uint32_t skip, uint32_t *present, char *stored_name,
                       media_progress_fn progress) {
    *present = 0;
    set_name(stored_name, "");
    bool ok = write_header();

    if (ok) ok = write_image(offset, size, file, skip, progress);
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
    } else if (stored->magic == MEDIA_MAGIC_V2) {
        // The cartridge has moved out from under this record (see MEDIA_MAGIC),
        // so it goes. The ROM image has not moved, so the override stays.
        const media_header_v2_t *old = (const media_header_v2_t *) stored;
        header.rom_present = old->rom_present;
        header.builtin_checksum = old->builtin_checksum;
        memcpy(header.rom_name, old->rom_name, MEDIA_NAME_MAX_V2);
        header.rom_name[MEDIA_NAME_MAX_V2 - 1] = '\0';
        // Nothing is written back here on purpose. Rewriting the header would
        // mean erasing a sector at boot, with Core 1 already running the
        // renderer out of flash and nothing yet drawn on the panel — a flash
        // write is what the launcher does with the machine stopped, not
        // something to do unasked while the machine is coming up. Carrying the
        // migration in RAM costs nothing: it produces the same answer on every
        // boot, and the next header the launcher writes is a v3 one.
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

uint32_t media_cart_size(void) {
    return header.cart_present ? header.cart_size : 0;
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
    // The staging buffer is the cartridge's sector cache as well (media_stage),
    // and a ROM load is about to fill it with something else.
    cart_flush();

    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) return false;

    // A ROM image is the whole 32 KB the chip holds, so a longer file is read
    // from its end — the same rule a cartridge follows, and for the same reason.
    FSIZE_t file_size = f_size(&file);
    uint32_t skip = (file_size > MEDIA_ROM_SIZE) ? (uint32_t) (file_size - MEDIA_ROM_SIZE) : 0;

    // Stamped before the load, not after, so it is in both headers load_image()
    // writes. This is what a later firmware compares against to know that the
    // BIOS it ships is no longer the one this ROM was chosen over.
    header.builtin_checksum = rom_checksum(bios_rom);

    bool ok = load_image(&file, name, MEDIA_ROM_OFFSET, MEDIA_ROM_SIZE, skip,
                         &header.rom_present, header.rom_name, progress);
    f_close(&file);

    // Whatever the built-in BIOS is now, it is the one this choice was made
    // against — there is nothing left to warn about either way.
    rom_override_stale = false;
    return ok;
}

media_cart_result_t media_load_cart(const char *path, const char *name,
                                    media_progress_fn progress) {
    // Whatever is in the slot keeps what it wrote to itself, and the staging
    // buffer is where that is being kept.
    cart_flush();

    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) return MEDIA_CART_ERR_OPEN;

    uint32_t file_size = (uint32_t) f_size(&file);

    if (file_size > MEDIA_CART_MAX) {
        f_close(&file);
        return MEDIA_CART_ERR_TOO_BIG;
    }

    // The mapper is decided by the file's size and never by its name (cart.h,
    // 6502-VCS PLAN.md §1 rule 2), so a length that is not one a cartridge
    // comes in is refused rather than half-loaded.
    if (!cart_size_valid(file_size)) {
        f_close(&file);
        return MEDIA_CART_ERR_SIZE;
    }

    // A banked image goes into flash whole, because the mapper indexes all of
    // it. A flat one is mapped at the very top of the address space, so it is
    // the *end* of the image that matters: a .crt is a 32 KB AT28C256 image
    // whose first 16 KB is padding (see the 6502-CRT linker config), while a
    // bare 16 KB build has no padding to skip. Taking the last 16 KB handles
    // both, and only the 16 KB the machine can see is written to flash.
    uint32_t written = cart_size_banked(file_size)
                           ? file_size
                           : ((file_size > MEDIA_CART_WINDOW) ? MEDIA_CART_WINDOW : file_size);
    uint32_t skip = file_size - written;

    header.cart_size = file_size;

    bool ok = load_image(&file, name, MEDIA_CART_OFFSET, written, skip,
                         &header.cart_present, header.cart_name, progress);
    f_close(&file);

    return ok ? MEDIA_CART_OK : MEDIA_CART_ERR_WRITE;
}

// Ejecting only clears the flag — the image is left in flash to be erased by
// whatever is loaded next, since erasing it now would cost the same time for
// no benefit.
void media_eject_cart(void) {
    if (!header.cart_present) return;
    cart_flush();
    header.cart_present = 0;
    header.cart_size = 0;
    set_name(header.cart_name, "");
    write_header();
}

void media_restore_builtin_rom(void) {
    if (!header.rom_present) return;
    cart_flush(); // write_header() below stages through the cartridge's cache
    header.rom_present = 0;
    header.builtin_checksum = 0;
    set_name(header.rom_name, "");
    rom_override_stale = false;
    write_header();
}
