#ifndef BIOS_ROM_H
#define BIOS_ROM_H

#include <stdint.h>

#define BIOS_ROM_SIZE 0x8000 // 32KB; see bios_rom.c for the machine-address mapping

extern const uint8_t bios_rom[BIOS_ROM_SIZE];

#endif
