#ifndef MACHINE_VIDEO_H
#define MACHINE_VIDEO_H

#include <stdint.h>

// Emulated TMS9918A VDP, IO slot 7 ($9C00-$9C01, see PLAN.md Phase 4 and
// BIOS.inc VC_DATA/VC_REG/VC_STATUS). Register/VRAM/address-latch model
// ported from 6502-DEV's VideoCard (DB Emulator/lib/6502/IO/VideoCard.*);
// the AV-packet link to the Teensy's separate DOB Display board is dropped
// since this port renders directly from the same VRAM (see src/machine/
// video_render.c, which runs on Core 1).
#define VIDEO_VRAM_SIZE 16384

void video_reset(void);

// addr is slot-local (0-1 used): 0 = data port (VRAM R/W, auto-increment),
// 1 = control port (write: two-byte address/register latch sequence;
// read: status register, clears the frame-interrupt flag).
uint8_t video_read(uint16_t addr);
void video_write(uint16_t addr, uint8_t value);

// Call once per emulated CPU clock tick. Times the ~60Hz vblank interrupt
// off an assumed CPU clock (see video.c VIDEO_CPU_HZ) since this port does
// not yet pace the 6502 tick loop to a real clock rate (PLAN.md Phase 11
// will revisit); refreshes the Core-1-visible VRAM/register snapshot once
// per emulated frame. Returns 0x80 when the VDP wants an IRQ serviced
// (frame interrupt, R1 bit 5 enabled), 0x00 otherwise.
uint8_t video_tick(void);

// Read-only snapshot of vram[]/registers[8], refreshed once per emulated
// frame by video_tick() (see above). Core 1's renderer reads these directly
// with no locking: the copy is a plain memcpy racing at most one in-flight
// CPU write, so a render pass can occasionally use a half-updated snapshot
// (a rare, single-frame cosmetic tear, self-correcting next frame) — traded
// deliberately for not stalling either core with a lock (see PLAN.md Phase 4
// notes in repo memory).
const uint8_t *video_snapshot_vram(void);
const uint8_t *video_snapshot_registers(void);

#endif
