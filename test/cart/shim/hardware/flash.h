// The three numbers machine/media.h wants from the SDK, so the real media.h —
// layout arithmetic, static assertions and all — compiles on a desktop.
#ifndef HARDWARE_FLASH_SHIM_H
#define HARDWARE_FLASH_SHIM_H

#define FLASH_SECTOR_SIZE 4096u
#define FLASH_PAGE_SIZE   256u

// A Pico 1. The layout is checked for both sizes at compile time on the real
// builds; this only has to pick one.
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES 0x200000u
#endif

#endif
