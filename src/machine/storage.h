#ifndef MACHINE_STORAGE_H
#define MACHINE_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

// Emulated CompactFlash card in true-IDE (8-bit LBA) mode, IO slot 3
// ($8C00-$8C07, see PLAN.md Phase 7 and BIOS.inc's ST_* equates). Register
// model ported from 6502-DEV's StorageCard (DB Emulator/lib/6502/IO/
// StorageCard.*), which the TypeScript emulator's Storage.ts matches.
//
// The card is backed by a disk-image file on the PicoCalc's SD card
// (STORAGE_IMAGE_PATH), so the whole BIOS storage stack — LOAD/SAVE/DIR/DEL,
// BLOAD/BSAVE, DISK n — works against it and survives a power cycle. The
// image is the same format the `cffs` tool writes, so images can be built on
// a host and copied to the SD card.
//
// Register map (addr & 0x07), read / write:
//   0 data / data, 1 error / feature, 2 sector count, 3-6 LBA 0-3,
//   7 status / command.
#define STORAGE_IMAGE_PATH "CF.IMG"

// 512-byte sectors; 2048 sectors (1 MB) per BIOS disk bank, 256 banks.
#define STORAGE_SECTOR_SIZE   512
#define STORAGE_SECTOR_COUNT  0x80000 // 524288 sectors = 256 MB

// Opens the disk image on the SD card, creating a blank one-disk-bank image
// if none exists. Requires FatFs to be mounted already (see main.c). Returns
// true if the card should present itself to the machine; when it returns
// false the status register reads back 0x00, so the BIOS's StInit probe times
// out and leaves HW_CF clear — the machine boots fine, just with no storage.
bool storage_init(void);
bool storage_available(void);

// Resets the register file and any in-flight transfer (does not touch the
// backing image).
void storage_reset(void);

// addr is slot-local; only the low 3 bits are decoded.
uint8_t storage_read(uint16_t addr);
void storage_write(uint16_t addr, uint8_t value);

#endif
