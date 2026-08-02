#include "video_render.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"

#include "lcd/lcd.h"
#include "video.h"

// TMS9918 active display area (PLAN.md §2): 256x192, centred on the 320x320
// panel with an equal border on all sides.
#define ACTIVE_W 256
#define ACTIVE_H 192
#define BORDER_X ((LCD_WIDTH - ACTIVE_W) / 2)
#define BORDER_Y ((LCD_HEIGHT - ACTIVE_H) / 2)

// Target ~30fps render attempts; lcd_blit_indexed()/lcd_present() are no-ops
// for anything that hasn't actually changed, so this just bounds how often
// we re-check (see PLAN.md Phase 1's dirty-rect lesson — repo memory).
#define FRAME_PERIOD_MS 33

// TMS9918A 16-color RGB565 palette, ported from 6502-DEV's constants.h
// (Firmware/DOB Display/lib/DOBDisplay/constants.h TMS_PALETTE).
static const uint16_t TMS_PALETTE[16] = {
    0x0000, // 0  Transparent / Black
    0x0000, // 1  Black
    0x2648, // 2  Medium Green
    0x5EEF, // 3  Light Green
    0x52BD, // 4  Dark Blue
    0x7BBF, // 5  Light Blue
    0xD289, // 6  Dark Red
    0x475E, // 7  Cyan
    0xFAAA, // 8  Medium Red
    0xFBCF, // 9  Light Red
    0xD60A, // 10 Dark Yellow
    0xE670, // 11 Light Yellow
    0x2587, // 12 Dark Green
    0xCAD7, // 13 Magenta
    0xCE79, // 14 Gray
    0xFFFF, // 15 White
};

static uint8_t canvas[ACTIVE_H][ACTIVE_W];
static const uint8_t *vram;
static const uint8_t *regs;

static void set_pixel(int x, int y, uint8_t color_index) {
    if ((unsigned) x >= ACTIVE_W || (unsigned) y >= ACTIVE_H) return;
    if (color_index == 0) return; // transparent - show backdrop
    canvas[y][x] = color_index;
}

// GRAPHICS I MODE - 32x24 tiles, 8x8 each.
static void render_graphics_i(void) {
    uint16_t name_table = (uint16_t) ((regs[2] & 0x0F) << 10);
    uint16_t color_table = (uint16_t) (regs[3] << 6);
    uint16_t pattern_table = (uint16_t) ((regs[4] & 0x07) << 11);

    for (uint8_t tile_y = 0; tile_y < 24; tile_y++) {
        for (uint8_t tile_x = 0; tile_x < 32; tile_x++) {
            uint8_t char_code = vram[(name_table + tile_y * 32 + tile_x) & 0x3FFF];

            uint8_t color_byte = vram[(color_table + (char_code >> 3)) & 0x3FFF];
            uint8_t fg = (color_byte >> 4) & 0x0F;
            uint8_t bg = color_byte & 0x0F;

            for (uint8_t row = 0; row < 8; row++) {
                uint8_t pat_byte = vram[(pattern_table + char_code * 8 + row) & 0x3FFF];
                for (uint8_t col = 0; col < 8; col++) {
                    uint8_t bit = (pat_byte >> (7 - col)) & 1;
                    set_pixel(tile_x * 8 + col, tile_y * 8 + row, bit ? fg : bg);
                }
            }
        }
    }
}

// GRAPHICS II MODE - 32x24 tiles, 3 sections of 256 patterns.
static void render_graphics_ii(void) {
    uint16_t name_table = (uint16_t) ((regs[2] & 0x0F) << 10);
    uint16_t color_table = (uint16_t) ((regs[3] & 0x80) << 6);
    uint16_t pattern_table = (uint16_t) ((regs[4] & 0x04) << 11);

    for (uint8_t tile_y = 0; tile_y < 24; tile_y++) {
        for (uint8_t tile_x = 0; tile_x < 32; tile_x++) {
            uint8_t char_code = vram[(name_table + tile_y * 32 + tile_x) & 0x3FFF];

            uint8_t section = tile_y / 8;
            uint16_t section_offset = (uint16_t) (section * 2048);

            for (uint8_t row = 0; row < 8; row++) {
                uint8_t pat_byte = vram[(pattern_table + section_offset + char_code * 8 + row) & 0x3FFF];
                uint8_t color_byte = vram[(color_table + section_offset + char_code * 8 + row) & 0x3FFF];
                uint8_t fg = (color_byte >> 4) & 0x0F;
                uint8_t bg = color_byte & 0x0F;

                for (uint8_t col = 0; col < 8; col++) {
                    uint8_t bit = (pat_byte >> (7 - col)) & 1;
                    set_pixel(tile_x * 8 + col, tile_y * 8 + row, bit ? fg : bg);
                }
            }
        }
    }
}

// TEXT MODE - 40x24 tiles, 6x8 each, centred in the 256px active width.
static void render_text(void) {
    uint16_t name_table = (uint16_t) ((regs[2] & 0x0F) << 10);
    uint16_t pattern_table = (uint16_t) ((regs[4] & 0x07) << 11);
    uint8_t fg = (regs[7] >> 4) & 0x0F;
    uint8_t bg = regs[7] & 0x0F;
    uint8_t x_offset = 8; // centre 240px (40*6) in 256px

    for (uint8_t tile_y = 0; tile_y < 24; tile_y++) {
        for (uint8_t tile_x = 0; tile_x < 40; tile_x++) {
            uint8_t char_code = vram[(name_table + tile_y * 40 + tile_x) & 0x3FFF];

            for (uint8_t row = 0; row < 8; row++) {
                uint8_t pat_byte = vram[(pattern_table + char_code * 8 + row) & 0x3FFF];
                for (uint8_t col = 0; col < 6; col++) {
                    uint8_t bit = (pat_byte >> (7 - col)) & 1;
                    set_pixel(x_offset + tile_x * 6 + col, tile_y * 8 + row, bit ? fg : bg);
                }
            }
        }
    }
}

// MULTICOLOR MODE - 64x48 fat pixels (4x4 each).
static void render_multicolor(void) {
    uint16_t name_table = (uint16_t) ((regs[2] & 0x0F) << 10);
    uint16_t pattern_table = (uint16_t) ((regs[4] & 0x07) << 11);

    for (uint8_t tile_y = 0; tile_y < 24; tile_y++) {
        for (uint8_t tile_x = 0; tile_x < 32; tile_x++) {
            uint8_t char_code = vram[(name_table + tile_y * 32 + tile_x) & 0x3FFF];

            for (uint8_t row = 0; row < 8; row++) {
                uint8_t pat_byte = vram[(pattern_table + char_code * 8 + row) & 0x3FFF];
                uint8_t left_color = (pat_byte >> 4) & 0x0F;
                uint8_t right_color = pat_byte & 0x0F;

                for (uint8_t dx = 0; dx < 4; dx++) {
                    set_pixel(tile_x * 8 + dx, tile_y * 8 + row, left_color);
                    set_pixel(tile_x * 8 + 4 + dx, tile_y * 8 + row, right_color);
                }
            }
        }
    }
}

// SPRITE RENDERING - up to 32 sprites, max 4 visible per scanline.
typedef struct {
    int16_t x;
    int16_t y;
    uint8_t pattern;
    uint8_t color;
} sprite_t;

static void render_sprites(void) {
    uint16_t sprite_attr_table = (uint16_t) ((regs[5] & 0x7F) << 7);
    uint16_t sprite_pattern_table = (uint16_t) ((regs[6] & 0x07) << 11);
    uint8_t size16 = (regs[1] >> 1) & 1;
    uint8_t magnify = regs[1] & 1;
    uint8_t sprite_size = size16 ? 16 : 8;
    uint8_t pixel_size = magnify ? 2 : 1;

    // Collect active sprites (stop at Y == 0xD0).
    static sprite_t sprites[32];
    uint8_t sprite_count = 0;

    for (uint8_t i = 0; i < 32; i++) {
        uint16_t attr_addr = (uint16_t) (sprite_attr_table + i * 4);
        uint8_t raw_y = vram[attr_addr & 0x3FFF];
        if (raw_y == 0xD0) break;

        uint8_t raw_x = vram[(attr_addr + 1) & 0x3FFF];
        uint8_t pattern = vram[(attr_addr + 2) & 0x3FFF];
        uint8_t color_byte = vram[(attr_addr + 3) & 0x3FFF];

        sprites[sprite_count].y = (int16_t) ((raw_y + 1) & 0xFF);
        sprites[sprite_count].x = (int16_t) ((color_byte & 0x80) ? (int16_t) raw_x - 32 : raw_x);
        sprites[sprite_count].pattern = pattern;
        sprites[sprite_count].color = color_byte & 0x0F;
        sprite_count++;
    }

    // Render in reverse order (higher index = lower priority, drawn first).
    static uint8_t scanline_count[ACTIVE_H];
    memset(scanline_count, 0, sizeof(scanline_count));

    for (int8_t si = (int8_t) (sprite_count - 1); si >= 0; si--) {
        sprite_t *sp = &sprites[si];

        for (uint8_t row = 0; row < sprite_size; row++) {
            uint8_t pat_bytes[2];
            uint8_t pat_byte_count;

            if (size16) {
                uint8_t pat_idx = sp->pattern & 0xFC;
                if (row < 8) {
                    pat_bytes[0] = vram[(sprite_pattern_table + pat_idx * 8 + row) & 0x3FFF];
                    pat_bytes[1] = vram[(sprite_pattern_table + (pat_idx + 2) * 8 + row) & 0x3FFF];
                } else {
                    pat_bytes[0] = vram[(sprite_pattern_table + (pat_idx + 1) * 8 + (row - 8)) & 0x3FFF];
                    pat_bytes[1] = vram[(sprite_pattern_table + (pat_idx + 3) * 8 + (row - 8)) & 0x3FFF];
                }
                pat_byte_count = 2;
            } else {
                pat_bytes[0] = vram[(sprite_pattern_table + sp->pattern * 8 + row) & 0x3FFF];
                pat_byte_count = 1;
            }

            for (uint8_t py = 0; py < pixel_size; py++) {
                int16_t screen_y = (int16_t) (sp->y + row * pixel_size + py);
                if (screen_y < 0 || screen_y >= ACTIVE_H) continue;
                if (scanline_count[screen_y] >= 4) continue;

                bool counted = false;
                uint8_t col_offset = 0;

                for (uint8_t bi = 0; bi < pat_byte_count; bi++) {
                    uint8_t pat = pat_bytes[bi];
                    for (uint8_t col = 0; col < 8; col++) {
                        if ((pat >> (7 - col)) & 1) {
                            for (uint8_t px = 0; px < pixel_size; px++) {
                                set_pixel(sp->x + (col_offset + col) * pixel_size + px, screen_y, sp->color);
                            }
                            counted = true;
                        }
                    }
                    col_offset += 8;
                }

                if (counted) scanline_count[screen_y]++;
            }
        }
    }
}

// Real TMS9918 hardware fills the whole overscan border with the backdrop
// colour, not just the active area. -1 forces the first frame to paint it
// (covers the boot test pattern left there by main.c); after that we only
// repaint when the backdrop register actually changes.
static int last_border_color = -1;

static void paint_border(uint8_t color_index) {
    lcd_fill_rect(0, 0, LCD_WIDTH, BORDER_Y, color_index);                              // top
    lcd_fill_rect(0, BORDER_Y + ACTIVE_H, LCD_WIDTH, LCD_HEIGHT - BORDER_Y - ACTIVE_H, color_index); // bottom
    lcd_fill_rect(0, BORDER_Y, BORDER_X, ACTIVE_H, color_index);                         // left
    lcd_fill_rect(BORDER_X + ACTIVE_W, BORDER_Y, LCD_WIDTH - BORDER_X - ACTIVE_W, ACTIVE_H, color_index); // right
}

static void render_frame(void) {
    vram = video_snapshot_vram();
    regs = video_snapshot_registers();

    uint8_t m1 = (regs[1] >> 4) & 1;
    uint8_t m2 = (regs[1] >> 3) & 1;
    uint8_t m3 = (regs[0] >> 1) & 1;
    uint8_t backdrop = regs[7] & 0x0F;

    if (backdrop != last_border_color) {
        paint_border((uint8_t) (VIDEO_PALETTE_BASE + backdrop));
        last_border_color = backdrop;
    }

    memset(canvas, backdrop, sizeof(canvas));

    if (m1 == 0 && m2 == 0 && m3 == 0) {
        render_graphics_i();
        render_sprites();
    } else if (m1 == 0 && m2 == 0 && m3 == 1) {
        render_graphics_ii();
        render_sprites();
    } else if (m1 == 1 && m2 == 0 && m3 == 0) {
        render_text();
    } else if (m1 == 0 && m2 == 1 && m3 == 0) {
        render_multicolor();
        render_sprites();
    }

    lcd_blit_indexed(BORDER_X, BORDER_Y, ACTIVE_W, ACTIVE_H, &canvas[0][0], ACTIVE_W, VIDEO_PALETTE_BASE);
    lcd_present(); // no-op unless lcd_blit_indexed actually changed something
}

static void video_render_init(void) {
    for (int i = 0; i < 16; i++) {
        lcd_set_palette((uint8_t) (VIDEO_PALETTE_BASE + i), TMS_PALETTE[i]);
    }
}

void video_render_core1_entry(void) {
    video_render_init();

    while (true) {
        absolute_time_t frame_start = get_absolute_time();
        render_frame();
        int64_t elapsed_ms = absolute_time_diff_us(frame_start, get_absolute_time()) / 1000;
        if (elapsed_ms < FRAME_PERIOD_MS) {
            sleep_ms((uint32_t) (FRAME_PERIOD_MS - elapsed_ms));
        }
    }
}
