#ifndef MACHINE_RAM_BANK_H
#define MACHINE_RAM_BANK_H

#include <stdint.h>

// Emulated banked expansion RAM, IO slots 0 and 1 ($8000-$83FF and
// $8400-$87FF, see PLAN.md Phase 9 and BIOS.inc's RAM_DATA_L/RAM_BANK_L and
// RAM_DATA_H/RAM_BANK_H equates). Register model ported from 6502-DEV's
// RAMCard (DB Emulator/lib/6502/IO/RAMCard.*), which the TypeScript
// emulator's RAMBank.ts matches.
//
// Each card presents a 1 KB window with the bank latch in its last byte:
//   $x000-$x3FE  data — the selected bank's 1 KB
//   $x3FF        bank latch — a write selects one of 256 banks, and the same
//                write also lands in the *new* bank's last byte (both
//                reference implementations do this, so the byte at offset
//                $3FF of a bank always holds the last latch value written
//                while that bank was selected)
//
// Reads decode no registers at all: every address in the window, $3FF
// included, reads the selected bank's byte.

// Size is a per-target compile budget (PLAN.md §4): the full pair of 256 KB
// cards is 512 KB, which neither chip has spare once the 32 KB of system RAM,
// the 16 KB of VRAM and its snapshot, and the 150 KB of canvas + framebuffer
// are accounted for. What actually fits in the SRAM left over after those:
//
//   RP2040 (264 KB, ~18 KB spare)  IO 1 at 16 banks / 16 KB, IO 2 absent
//   RP2350 (520 KB)                IO 1 at 256 banks / 256 KB (BANK 0 in
//                                  full, as on real hardware), IO 2 absent
//
// The RP2040 figure is the same 16-bank card 6502-DEV builds when its own
// MEM_EXTMEM budget is off. Trading it away to cache the BIOS in RAM instead
// (PLAN.md Phase 11 perf pass) was tried and measured short by ~6 KB even
// after dropping this card entirely; the same change also overflowed
// RP2350 by ~9 KB, so the "plenty of spare SRAM" this section used to claim
// for RP2350 was stale, not just optimistic. Closing either gap meant
// shrinking a core's stack sight-unseen on hardware there's no way to
// load-test here, which was judged not worth risking a silent stack-overflow
// corruption for on either target. Both keep reading the ROM straight from
// flash and stay slow but stable (see main.c's boot_mark() trail).
//
// Because the BIOS probes each slot and BASIC only ever uses BANK 0, every
// one of these choices boots correctly — only the amount of paged RAM
// differs, and `MEM`'s HW byte reports which cards answered.
//
// Bank counts must be a power of two so a short card aliases the full 256-bank
// address space over what it has, exactly as 6502-DEV's card masks its bank
// latch. Set either to 0 to leave that slot empty (open bus, so the BIOS's
// ProbeRAM read-back test fails and HW_RAM_L/HW_RAM_H stay clear); override
// either from CMake with target_compile_definitions().
#ifndef RAM_BANK_LOW_BANKS
#if PICO_RP2350
#define RAM_BANK_LOW_BANKS 256
#else
#define RAM_BANK_LOW_BANKS 16
#endif
#endif

#ifndef RAM_BANK_HIGH_BANKS
#define RAM_BANK_HIGH_BANKS 0
#endif

#define RAM_BANK_WINDOW_SIZE 0x400
#define RAM_BANK_LATCH_ADDR  0x3FF // last byte of the window

typedef enum {
    RAM_BANK_LOW = 0,  // IO 1, slot 0 ($8000)
    RAM_BANK_HIGH = 1, // IO 2, slot 1 ($8400)
} ram_bank_id_t;

// Cold start: clears every bank and selects bank 0 on both cards. Call once
// at boot, as the constructor does on the Teensy.
void ram_bank_init(void);

// Warm reset: deliberately does nothing. These are SRAM cards — the contents
// survive the reset button, and the bank latch is not wired to the bus's RES
// line, so neither is disturbed (6502-DEV's card likewise has an empty
// reset(), and the desktop emulator only clears on a cold start).
//
// The *contents* are what survive; how many banks the card presents is a
// separate, persisted setting (see ram_bank_set_active) that neither kind of
// reset touches.
void ram_bank_reset(void);

// Zeros both cards and returns their latches to bank 0 — the cold-start half
// of machine_reset(), standing in for the SRAM coming up empty after the
// power has actually been off. Leaves the active bank count alone, that being
// a setting rather than machine state.
void ram_bank_clear(void);

// addr is slot-local; only the low 10 bits are decoded.
uint8_t ram_bank_read(ram_bank_id_t card, uint16_t addr);
void ram_bank_write(ram_bank_id_t card, uint16_t addr, uint8_t value);

// Banks fitted to a card, 0 if the slot is empty. This is the compiled-in
// maximum (the size of the array behind it), not necessarily what the machine
// currently sees — see ram_bank_active().
uint16_t ram_bank_banks(ram_bank_id_t card);

// How many banks the card actually presents, which is what the BIOS's
// ProbeRAM finds and `MEM` reports. Never more than ram_bank_banks().
uint16_t ram_bank_active(ram_bank_id_t card);

// Narrows (or restores) the card to `banks`, clamped into [1, ram_bank_banks]
// and rounded down to a power of two — the addressing folds the 256-bank
// space over whatever is fitted by masking, which needs a power of two. A
// slot with no card compiled in ignores this entirely.
//
// Only the visible window changes: the array behind it is always the compiled
// maximum, so narrowing and later widening again does not lose what was in
// the banks that were briefly out of reach. There is no memory to reclaim by
// narrowing — this exists so a machine can be made to look like one with a
// smaller card fitted (PLAN.md Phase 11's settings screen).
void ram_bank_set_active(ram_bank_id_t card, uint16_t banks);

#endif
