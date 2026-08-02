#ifndef MACHINE_VIDEO_RENDER_H
#define MACHINE_VIDEO_RENDER_H

// Phase 4: TMS9918 renderer, ported from 6502-DEV's DOB Display firmware
// (Firmware/DOB Display/src/main.cpp renderGraphicsI/II/Text/Multicolor/
// Sprites) to run on Core 1, reading the VRAM/register snapshot video.c
// refreshes each emulated frame (see video.h). Targets the LCD's 8bpp
// palette framebuffer via lcd_blit_indexed() instead of the Teensy's
// ILI9341_t3n::writeRect8BPP.

// Reserves palette slots [VIDEO_PALETTE_BASE, VIDEO_PALETTE_BASE+16) for the
// TMS9918's 16-color palette; must not collide with lcd.h's LCD_COLOR_*
// debug palette (0-8).
#define VIDEO_PALETTE_BASE 16

// Core 1 entry point (pass directly to multicore_launch_core1() from
// main.c). Never returns: loads the TMS9918 palette into the LCD, then
// repeatedly renders the current VDP mode into a 256x192 canvas centred on
// the 320x320 panel and presents only the rows that actually changed.
void video_render_core1_entry(void);

#endif
