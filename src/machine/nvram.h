// Phase 8: the DS1511Y's 256 bytes of battery-backed NVRAM (PLAN.md Phase 8).
// The PicoCalc has no coin cell behind the Pico, so "battery-backed" is
// emulated by keeping the bytes in the last sector of the Pico's own flash:
// the machine sees a normal NVRAM that survives power cycles, which is what
// BASIC's NVRAM statement/function and the BIOS's ProbeRTC read-back test
// expect (see src/machine/rtc.c).
#ifndef MACHINE_NVRAM_H
#define MACHINE_NVRAM_H

#include <stdbool.h>
#include <stdint.h>

#define NVRAM_SIZE 256

// Loads the saved bytes from flash, or zero-fills if the sector has never
// been written (or fails its checksum). Call once at boot.
void nvram_init(void);

uint8_t nvram_read(uint8_t addr);
void nvram_write(uint8_t addr, uint8_t value);

// Call from Core 0's outer loop. Writes the bytes back to flash once they
// have been left alone for NVRAM_SETTLE_MS -- BASIC's NVRAM statement pokes
// one byte per call, so a program filling the whole 256 bytes would
// otherwise erase the sector 256 times over.
void nvram_task(void);

// Forces the pending write out now. Returns true if flash matches RAM
// afterwards (including "nothing to do"). Erasing flash needs Core 1 parked
// (see nvram.c), so this must only be called from Core 0.
bool nvram_flush(void);

#endif
