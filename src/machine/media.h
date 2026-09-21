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

// What a flat cartridge overlays: $C000-$FFFF, 16 KB, the Kernal and character
// set below it staying visible (see the DOCS "Writing a cartridge" chapter).
// A .crt for that slot is 16 KB of cartridge, either on its own or as the top
// half of a 32 KB EEPROM image — media_load_cart() takes the last 16 KB of
// whatever it is given, which handles both.
#define MEDIA_CART_WINDOW 0x4000

// How much flash the cartridge region gets: a full megabyte, which is the
// largest image 6502-VCS PLAN.md §1 defines, on both boards. The firmware is
// well under 200 KB on either, so a uniform maximum is affordable and means
// neither build refuses a cart the other takes.
//
// Reserving costs nothing unused: flash that is not written is not erased, and
// media_load_cart() only ever erases what it writes.
#define MEDIA_CART_MAX 0x100000

// Longest file name remembered for the status line, including the terminator.
// Long enough for the whole of a cartridge name under 6502-VCS PLAN.md §1 —
// `HelloWorldCart-VDP-512K.crt` is 27 characters — which is the point of
// building FatFs with long names (ffconf.h FF_USE_LFN).
#define MEDIA_NAME_MAX 32

// Where machine/settings.c keeps its sector — **frozen**, and written out
// rather than derived.
//
// It used to be one sector below everything this module reserves, which was
// right for a fixed-size cartridge and is wrong the moment the cartridge region
// grows: growing it drags the floor down and drags this with it, and every
// existing user's settings are silently orphaned on the next boot. Reordering
// the block and migrating the record is real work on persisted state, on a
// board where it cannot be tested from a fresh flash — exactly the kind of risk
// this port should not be generating.
//
// So it stops tracking anything. The arithmetic it used to be, kept here
// because it is how these two numbers were arrived at and not because anything
// still computes them:
//
//   PICO_FLASH_SIZE_BYTES
//     - 2 * FLASH_SECTOR_SIZE   NVRAM sector, header sector
//     - 0x8000                  ROM image
//     - 0x4000                  the old flat cartridge slot
//     - FLASH_SECTOR_SIZE       this sector
//   = PICO_FLASH_SIZE_BYTES - 0xF000
//
//   Pico 1 (2 MB): 0x200000 - 0xF000 = 0x1F1000
//   Pico 2 (4 MB): 0x400000 - 0xF000 = 0x3F1000
#if PICO_FLASH_SIZE_BYTES == 0x400000
#define MEDIA_SETTINGS_FLASH_OFFSET 0x3F1000u
#elif PICO_FLASH_SIZE_BYTES == 0x200000
#define MEDIA_SETTINGS_FLASH_OFFSET 0x1F1000u
#else
#error "no frozen settings offset for this flash size - see media.h"
#endif

// Bottom edge of everything reserved at the top of the chip — the NVRAM sector,
// the header sector, the ROM image, sixteen stranded kilobytes where the old
// flat cartridge slot was, the frozen settings sector, and the cartridge
// region below it (see media.c's layout comment). Anything else wanting a
// sector of its own takes it from below this, rather than re-deriving the chain
// and risking the two drifting apart.
//
// Only the cartridge region grows, and it grows downward from a sector that
// does not move. Nothing above it ever moves again.
#define MEDIA_RESERVED_FLASH_BASE (MEDIA_SETTINGS_FLASH_OFFSET - MEDIA_CART_MAX)

// Reads the header record left in flash by an earlier load, so a cartridge or
// ROM chosen before the last power-off is still in place. Call once at boot,
// before machine_init().
void media_init(void);

// The 32 KB ROM image the bus should read, indexed from a $8000 origin. Never
// NULL: the built-in BIOS unless a ROM has been loaded over it.
const uint8_t *media_rom(void);

// The cartridge image in flash, or NULL when the slot is empty.
//
// What it points at depends on what was loaded, which media_cart_size() says:
// a flat cart is the 16 KB the machine sees, indexed from a $C000 origin, and
// a banked one is the whole image, which only the mapper should be indexing.
const uint8_t *media_cart(void);

// The length of the file the cartridge was loaded from, which is what decides
// how it is mapped (6502-VCS PLAN.md §1 rule 2) and what the launcher reports.
// 0 when the slot is empty.
//
// Note that for a flat cart this is the length of the *file* and not of the
// region media_cart() points at: a 32 KB EEPROM image is 32,768 bytes of file
// and 16 KB of cartridge.
uint32_t media_cart_size(void);

// File names for the launcher's status lines; "" when nothing is loaded.
const char *media_rom_name(void);
const char *media_cart_name(void);

// True when a ROM loaded from the SD card is sitting over a built-in BIOS that
// has since been replaced — i.e. this firmware ships a different BIOS than the
// one that was in the socket when that ROM was chosen. It is a standing fact
// rather than an event, so it keeps reading true until the built-in BIOS is
// restored or the ROM is loaded again against this one; the launcher shows it
// on its header, and main.c says it out loud once per power-on
// (launcher_boot_notice()).
//
// A ROM override surviving a firmware update is exactly what it should do — it
// stands in for a chip in a socket — right up until the update is the one that
// changes the chip it is standing in front of, which is when nobody was told.
bool media_rom_override_stale(void);

// The built-in BIOS's own version, read out of its banner: "v1.6", or "" if the
// image does not word it the way the AC6502 BIOS does. Shown wherever the ROM
// in the socket is named, so which BIOS is actually running is answerable
// without a serial terminal.
const char *media_builtin_rom_version(void);

// The one 4 KB flash staging buffer in the firmware, shared on purpose.
//
// machine/media.c stages ROM and cartridge images through it on the way into
// flash; machine/cart.c uses it as the emulated flash chip's write-back sector
// cache. Both are "one sector, in RAM, on its way to flash", and a Pico 1 has
// well under a kilobyte of SRAM spare once everything in ram_bank.h's budget
// is placed — a second one would simply not fit.
//
// The rule that makes it safe: anything that stages an image flushes the
// cartridge's held sector first. Every path in media.c that touches the buffer
// does, and they are the only callers.
uint8_t *media_stage(void);

// Erase one 4 KB sector of the cartridge region and program the staging buffer
// into it. `sector` counts from the start of the image.
bool media_cart_write_sector(uint32_t sector);

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

// Why a cartridge would not load, so the launcher can tell "this cart does not
// fit" from "your SD card is bad" — which is worth the extra string.
typedef enum {
    MEDIA_CART_OK,
    MEDIA_CART_ERR_OPEN,    // the file could not be opened or read
    MEDIA_CART_ERR_TOO_BIG, // larger than this board reserves for a cartridge
    MEDIA_CART_ERR_SIZE,    // not a length a cartridge comes in (cart.h §1)
    MEDIA_CART_ERR_WRITE,   // flash would not take it; the slot is left empty
} media_cart_result_t;

media_cart_result_t media_load_cart(const char *path, const char *name,
                                    media_progress_fn progress);

// Empty the cartridge slot / go back to the built-in BIOS. Same reset rule.
void media_eject_cart(void);
void media_restore_builtin_rom(void);

#endif
