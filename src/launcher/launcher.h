#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <stdbool.h>
#include <stdint.h>

// Phase 10: the on-device loader. Browses the SD card for programs,
// cartridges and ROMs — the same `Programs/`, `Carts/` and `ROMs/` folders
// 6502-DEV keeps, and the same three things the desktop emulator can load —
// and puts the chosen one into the machine.
//
// It is firmware, not part of the emulated machine: it draws on the LCD
// directly (in the BIOS's own character set, so it looks like the machine it
// belongs to) with the renderer suspended, and reads the PicoCalc's keyboard
// directly rather than through the VIA. That is what makes it always
// reachable — it runs from Core 0's outer loop, so it opens whatever the 6502
// is doing, including not running at all because a cartridge hung it.

// Whether a raw keyboard event (kbd_poll_raw()) is the key that opens the
// launcher. F1: the emulated machine has no function row, so kbd_decode()
// drops those keys and they cost the machine nothing.
bool launcher_hotkey(uint8_t state, uint8_t code);

// Show the launcher and run it until the user leaves. Blocks — the emulated
// machine is stopped for as long as the menu is up, and picks up where it
// left off afterwards unless something was loaded that needs a reset (in
// which case this resets it before returning).
void launcher_run(void);

#endif
