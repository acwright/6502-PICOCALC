#ifndef MACHINE_H
#define MACHINE_H

#include <stdbool.h>
#include <stdint.h>

// Ticks per outer-loop batch, mirroring 6502-DEV main.cpp's loop() (PLAN.md
// Phase 3) so USB CDC polling stays responsive between batches.
#define MACHINE_TICKS_PER_BATCH 2048

// Where a program image lands, and the most of one that fits below the I/O
// window (PLAN.md Phase 10, matching the desktop emulator's ProgramImage.ts).
#define MACHINE_PROGRAM_ADDRESS  0x0800
#define MACHINE_PROGRAM_MAX_SIZE (0x8000 - MACHINE_PROGRAM_ADDRESS)

// Allocates the vrEmu6502 core and wires it to the emulated bus. Call once at
// boot, before machine_reset()/machine_run().
void machine_init(void);

// Resets the CPU and all IO cards, and re-reads which ROM and cartridge
// images the bus should be showing (machine/media.h), since a chip is only
// ever swapped with the power off.
//
// `cold_start` is the difference between the two things the desktop emulator
// offers (Machine.reset(coldStart) there, PLAN.md Phase 11):
//
//   false — the reset button. Only the CPU's RESET line is pulsed; system RAM
//           and the expansion banks keep their contents, so a BASIC program
//           in memory survives and can be RUN again afterwards.
//   true  — a power cycle. RAM is cleared as well, so BASIC always comes up
//           cold, exactly as it would after the power had really been off.
//
// The ROM and cartridge images live in flash and survive both, as the chips
// they stand in for would.
void machine_reset(bool cold_start);

// Runs `ticks` emulated clock cycles, servicing IO card ticks and IRQ/NMI
// lines each cycle.
void machine_run(uint32_t ticks);

// Bus access from outside the emulated CPU, for the launcher's program
// loading (PLAN.md Phase 10). Only safe to call while the machine is stopped,
// i.e. from the outer loop between machine_run() batches.
uint8_t machine_peek(uint16_t addr);
void machine_poke(uint16_t addr, uint8_t value);

// True once BASIC has *finished* initialising its workspace, so the
// end-of-program pointers written by machine_set_program_end() will survive.
//
// The warm-start magic is the load-bearing part: BasColdInit sets TXTTAB and
// MEMSIZ early but VARTAB/ARYTAB/STREND later, leaving a window where the
// workspace looks ready and is not. BAS_WARM is the last byte it writes. The
// other two are still checked so a stray $A5 in uninitialised memory is not
// mistaken for a booted BASIC. (Ported from the desktop emulator's
// ProgramImage.isBasicReady.)
bool machine_basic_ready(void);

// Point BASIC's end-of-program pointers past a `length`-byte image written at
// $0800, the way BASIC's own LOAD does, and report whether BASIC was up to
// take them. Without this BASIC still believes the program ends at $0802 and
// the first variable it allocates lands on top of it.
//
// ARYTAB and STREND are set alongside VARTAB: RUN re-derives them from VARTAB
// via CLR, but a direct-mode assignment typed before RUN allocates at ARYTAB
// and would otherwise land inside the program.
bool machine_set_program_end(uint16_t length);

#endif
