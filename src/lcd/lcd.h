// Phase 1: LCD driver — ST7365P/ILI9488-compatible 320x320 SPI panel.
// See PLAN.md §2 (hardware mapping) and §5 Phase 1.
#ifndef LCD_H
#define LCD_H

#include <stdint.h>

#define LCD_WIDTH  320
#define LCD_HEIGHT 320

// Monospace debug font geometry (see lcd.c for the glyph table).
#define LCD_FONT_WIDTH   5
#define LCD_FONT_HEIGHT  8
#define LCD_CHAR_ADVANCE_X (LCD_FONT_WIDTH + 1)
#define LCD_CHAR_ADVANCE_Y (LCD_FONT_HEIGHT + 1)

// A handful of named entries in the 256-slot 8bpp palette; the rest are
// free for callers (e.g. the future TMS9918 palette in Phase 4).
enum {
    LCD_COLOR_BLACK = 0,
    LCD_COLOR_WHITE,
    LCD_COLOR_RED,
    LCD_COLOR_GREEN,
    LCD_COLOR_BLUE,
    LCD_COLOR_YELLOW,
    LCD_COLOR_CYAN,
    LCD_COLOR_MAGENTA,
    LCD_COLOR_GRAY,
};

// Brings up SPI1, resets and configures the panel, and clears the
// framebuffer to black. Must be called once before any other lcd_* call.
void lcd_init(void);

// Packs 8-bit channels into a panel-native RGB565 word.
uint16_t lcd_rgb565(uint8_t r, uint8_t g, uint8_t b);

// Sets palette slot `index` (0-255) to the given RGB565 colour.
void lcd_set_palette(uint8_t index, uint16_t rgb565);

void lcd_clear(uint8_t index);
void lcd_fill_rect(int x, int y, int w, int h, uint8_t index);
void lcd_put_pixel(int x, int y, uint8_t index);

// Copies an 8bpp `w`x`h` source image (row stride `src_stride` bytes) into
// the framebuffer at (x,y), adding `palette_base` to every source byte to
// reach the caller's reserved palette range (see Phase 4's VIDEO_PALETTE_BASE).
// Only rows whose content actually changed are written/marked dirty, so a
// caller (e.g. the VDP renderer) can call this every frame unconditionally
// without racing the panel's own GRAM scanout the way an unconditional
// full-frame lcd_present() would (see lcd_present() and PLAN.md Phase 1).
void lcd_blit_indexed(int x, int y, int w, int h, const uint8_t *src, int src_stride, uint8_t palette_base);

void lcd_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg);
void lcd_draw_string(int x, int y, const char *s, uint8_t fg, uint8_t bg);

// Converts the dirty region of the 8bpp framebuffer to RGB565 and DMAs it
// to the panel one scanline at a time, using a ping-ponged pair of line
// buffers so the next line is converted while the current one is still
// transferring. No-op if nothing has changed since the last call — only
// touching pixels that actually changed avoids racing the panel's own
// free-running GRAM scanout (which tears/flickers unchanged content).
void lcd_present(void);

// Forces a full-screen push regardless of the dirty region, for measuring
// sustained frame-rate/throughput. Prefer lcd_present() otherwise.
void lcd_present_full(void);

// Fills the screen with colour bars and a banner, for frame-rate testing.
void lcd_draw_test_pattern(void);

#endif
