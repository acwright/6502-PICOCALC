#ifndef MACHINE_MEDIA_H
#define MACHINE_MEDIA_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/flash.h" // FLASH_SECTOR_SIZE, for the layout floor below

// Phase 10: what a ROM or cartridge picked in the launcher (src/launcher)
// actually becomes — the two images the bus reads out of $A000-$FFFF.
//
// Both live in reserved sectors at the top of flash rather than in SRAM, for
// two reasons. The first is that they do not fit: the RP2040 has ~24 KB of
// SRAM left over once the machine, the canvas and the framebuffer are placed
// (PLAN.md §4), and a ROM image alone is 32 KB. The second is that flash is
// the more honest home anyway — a cartridge stays in the slot when the power
// goes off, and a replacement BIOS is burnt into a chip, so both survive a
// power cycle here exactly as they would on the real machine. Reads cost the
// same as the built-in BIOS, which is already a const array in the same
// flash, reached through the same XIP cache.
//
// The launcher is always reachable (it runs from Core 0's outer loop, not
// from the emulated CPU), so a cartridge that hangs the machine or a ROM that
// will not boot can always be ejected again — see launcher.h.

// A ROM image is the full 32 KB the machine's ROM chip holds, $8000-$FFFF,
// of which only $A000 up is ever read (the I/O window is decoded in front of
// the rest). Same origin and size as the built-in bios_rom[].
#define MEDIA_ROM_SIZE  0x8000

// A cartridge overlays $C000-$FFFF only — 16 KB, the Kernal and character set
// below it staying visible (see the DOCS "Writing a cartridge" chapter).
#define MEDIA_CART_SIZE 0x4000

// Longest file name remembered for the status line. FatFs is built without
// long-name support, so names arrive as 8.3 and always fit.
#define MEDIA_NAME_MAX 16

// Bottom edge of this module's reserved flash region — the NVRAM sector, the
// header sector, the ROM image and the cartridge image, all counted down from
// the top of the chip (see media.c's layout comment). Anything else wanting a
// sector of its own takes it from below this, rather than re-deriving the
// chain and risking the two drifting apart (machine/settings.c does exactly
// that).
#define MEDIA_RESERVED_FLASH_BASE \
    (PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE - MEDIA_ROM_SIZE - MEDIA_CART_SIZE)

// Reads the header record left in flash by an earlier load, so a cartridge or
// ROM chosen before the last power-off is still in place. Call once at boot,
// before machine_init().
void media_init(void);

// The 32 KB ROM image the bus should read, indexed from a $8000 origin. Never
// NULL: the built-in BIOS unless a ROM has been loaded over it.
const uint8_t *media_rom(void);

// The 16 KB cartridge image, indexed from a $C000 origin, or NULL when the
// slot is empty.
const uint8_t *media_cart(void);

// File names for the launcher's status lines; "" when nothing is loaded.
const char *media_rom_name(void);
const char *media_cart_name(void);

// Called after each flash sector, so the launcher can show progress. `done`
// and `total` are bytes.
typedef void (*media_progress_fn)(uint32_t done, uint32_t total);

// Copy a file from the SD card into the ROM/cartridge region of flash. `name`
// is what the status line should show. Both take tens of milliseconds per
// sector with Core 1 parked (see nvram.c's note on flash_safe_execute), which
// is why they report progress. Return false if the file could not be read or
// flash could not be written; the previous contents are lost either way, and
// the slot is left empty rather than half-loaded.
//
// The caller must reset the machine afterwards — these change what is at the
// reset vector, which is only re-read on reset, as swapping a chip on real
// hardware is only ever done with the power off.
bool media_load_rom(const char *path, const char *name, media_progress_fn progress);
bool media_load_cart(const char *path, const char *name, media_progress_fn progress);

// Empty the cartridge slot / go back to the built-in BIOS. Same reset rule.
void media_eject_cart(void);
void media_restore_builtin_rom(void);

#endif
