#ifndef MACHINE_CART_H
#define MACHINE_CART_H

#include <stdbool.h>
#include <stdint.h>

// The banked Flash Cart: one or two SST39SF0x0 chips behind a bank register,
// in place of the single 16 KB window a ROM cart fills.
//
// The contract is **6502-VCS `PLAN.md` §§1-3**, which is the normative copy —
// three emulators and one programmer have to agree on it bit for bit, so the
// rules live in that plan rather than in any one implementation. The section
// markers below cite it. The shared oracle is that plan's fixture set.
//
// **The 32K ROM cart is not replaced.** A flat cart never reaches this module
// at all: machine.c keeps its one-line index for that, exactly as it was.
//
// The image lives in the Pico's own flash (machine/media.c) and is read
// through the XIP window, which is the same cost the BIOS ROM already pays.
// There is no copy in SRAM and no cache: a read is a translation and a fetch,
// and the only state is the one-byte register. That matters on a Pico 1, where
// ram_bank.h's budget leaves under a kilobyte spare.

// The switchable window, $C000-$DFFF, and the fixed region above it.
#define CART_WINDOW_BASE 0xC000u
#define CART_FIXED_BASE  0xE000u
#define CART_BANK_SIZE   0x2000u

// The SST39SF0x0's erase unit, and so the granularity of everything that
// tracks what a cartridge has written to itself.
#define CART_SECTOR_SIZE 0x1000u

// The five lengths an image may legally be (§1). A .crt is a byte-exact image
// of the chip or chips on a cart — no header, no footer — so the mapper is
// decided by the file's size and never by its name.
#define CART_SIZE_FLAT 0x8000u
#define CART_SIZE_128K 0x20000u
#define CART_SIZE_256K 0x40000u
#define CART_SIZE_512K 0x80000u
#define CART_SIZE_1M   0x100000u

// True for a length this port will load: one of the four banked sizes, or
// anything up to 32 KB, which is the flat cart it has always taken (a bare
// 16 KB build, or a 32 KB EEPROM image whose first half is padding).
bool cart_size_valid(uint32_t size);

// True for a length that goes through this module rather than through
// machine.c's flat index.
bool cart_size_banked(uint32_t size);

// "16K", "32K", "128K", "256K", "512K" or "1M" — §1's table, for the launcher's
// status line. "?" for a length that is not a cartridge at all.
const char *cart_size_name(uint32_t size);

// Point the mapper at an image in flash and reset it. `size` is the length of
// the file it came from; a flat size leaves cart_banked() false and nothing
// here is used.
void cart_init(const uint8_t *image, uint32_t size);

// RESB: the bank register clears to 0 and both chips return to read mode. The
// register's /MR is on RESB, so a warm reset clears it too — a cart that boots
// the first time and not the second is the symptom of leaving this out.
void cart_reset(void);

// True when the image in the slot is banked.
bool cart_banked(void);

// The latched register, for the launcher's status line.
uint8_t cart_bank(void);

// A read through the mapper. `addr` is a CPU address in $C000-$FFFF.
uint8_t cart_read(uint16_t addr);

// A write through the mapper. $E000-$FFFF latches the bank register and
// reaches no flash; $C000-$DFFF is a flash write.
void cart_write(uint16_t addr, uint8_t data);

#endif
