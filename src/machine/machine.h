#ifndef MACHINE_H
#define MACHINE_H

#include <stdint.h>

// Ticks per outer-loop batch, mirroring 6502-DEV main.cpp's loop() (PLAN.md
// Phase 3) so USB CDC polling stays responsive between batches.
#define MACHINE_TICKS_PER_BATCH 2048

// Allocates the vrEmu6502 core and wires it to the emulated bus. Call once at
// boot, before machine_reset()/machine_run().
void machine_init(void);

// Resets the CPU and all IO cards (equivalent to a hardware reset button).
void machine_reset(void);

// Runs `ticks` emulated clock cycles, servicing IO card ticks and IRQ/NMI
// lines each cycle.
void machine_run(uint32_t ticks);

#endif
