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
//
// R3 and R4 double as masks here as well as base addresses: R3's low bits mask
// the name byte, and R4's low two bits select which of the three thirds of the
// screen get their own pattern/colour page. A program that leaves them all set
// (which the BIOS and everything built on it do) gets the plain three-section
// layout; one that clears them repeats a single page down the screen.
static void render_graphics_ii(void) {
    uint16_t name_table = (uint16_t) ((regs[2] & 0x0F) << 10);
    uint16_t color_table = (uint16_t) ((regs[3] & 0x80) << 6);
    uint16_t pattern_table = (uint16_t) ((regs[4] & 0x04) << 11);

    uint16_t name_mask = (uint16_t) (((regs[3] & 0x7F) << 3) | 0x07);
    uint16_t color_page_mask = (uint16_t) ((regs[3] & 0x60) << 6);

    for (uint8_t tile_y = 0; tile_y < 24; tile_y++) {
        uint16_t page = (uint16_t) ((((tile_y & 0x18) >> 3) & (regs[4] & 0x03)) << 11);
        uint16_t patt_base = (uint16_t) (pattern_table + page);
        uint16_t color_base = (uint16_t) (color_table + (page & color_page_mask));

        for (uint8_t tile_x = 0; tile_x < 32; tile_x++) {
            uint16_t char_code = vram[(name_table + tile_y * 32 + tile_x) & 0x3FFF] & name_mask;

            for (uint8_t row = 0; row < 8; row++) {
                uint16_t offset = (uint16_t) (char_code * 8 + row);
                uint8_t pat_byte = vram[(patt_base + offset) & 0x3FFF];
                uint8_t color_byte = vram[(color_base + offset) & 0x3FFF];
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
//
// One pattern byte covers a whole 8x4 half-tile: high nibble the left 4x4
// block, low nibble the right one. Which of the eight bytes a scanline uses is
// not the row within the tile — it steps once every four scanlines and is
// offset by the tile row's position in a group of four, so the same 32 names
// paint four different sets of blocks down the screen:
//
//   byte = (tile_y & 3) * 2 + ((y / 4) & 1)
static void render_multicolor(void) {
    uint16_t name_table = (uint16_t) ((regs[2] & 0x0F) << 10);
    uint16_t pattern_table = (uint16_t) ((regs[4] & 0x07) << 11);

    for (uint8_t y = 0; y < ACTIVE_H; y++) {
        uint8_t tile_y = (uint8_t) (y >> 3);
        uint8_t pat_row = (uint8_t) (((y >> 2) & 1) + (tile_y & 3) * 2);

        for (uint8_t tile_x = 0; tile_x < 32; tile_x++) {
            uint8_t char_code = vram[(name_table + tile_y * 32 + tile_x) & 0x3FFF];
            uint8_t color_byte = vram[(pattern_table + char_code * 8 + pat_row) & 0x3FFF];
            uint8_t left_color = (uint8_t) ((color_byte >> 4) & 0x0F);
            uint8_t right_color = (uint8_t) (color_byte & 0x0F);

            for (uint8_t dx = 0; dx < 4; dx++) {
                set_pixel(tile_x * 8 + dx, y, left_color);
                set_pixel(tile_x * 8 + 4 + dx, y, right_color);
            }
        }
    }
}

// SPRITE RENDERING - up to 32 sprites, at most four on any one scanline.
//
// Done a scanline at a time rather than a sprite at a time, because both of
// the rules that matter are per-scanline and depend on the sprites being
// visited in index order: sprite 0 has the highest priority and wins a pixel
// outright, and once four sprites have been found on a line the fifth and
// everything after it is dropped from that line. Sweeping the table backwards
// (which is one way to get the priority right by overpainting) gets the
// four-sprite rule exactly wrong — it keeps the four *lowest*-priority sprites
// and drops the ones that should have been drawn.
static void render_sprites(void) {
    uint16_t attr_table = (uint16_t) ((regs[5] & 0x7F) << 7);
    uint16_t patt_table = (uint16_t) ((regs[6] & 0x07) << 11);
    bool mag = (regs[1] & 0x01) != 0;
    bool size16 = (regs[1] & 0x02) != 0;
    uint8_t sprite_size = size16 ? 16 : 8;
    int16_t sprite_px = (int16_t) (sprite_size * (mag ? 2 : 1));

    // Which sprite (colour + 1) has claimed each pixel of the line so far. A
    // transparent sprite still claims its pixel — it just leaves a 1 here,
    // which a later sprite is allowed to paint over, while a coloured one
    // leaves 2 or more and keeps it.
    static uint8_t row_owner[ACTIVE_W];

    for (int16_t y = 0; y < ACTIVE_H; y++) {
        uint8_t shown = 0;

        for (uint8_t i = 0; i < 32; i++) {
            uint16_t attr = (uint16_t) (attr_table + i * 4);
            int16_t ypos = vram[attr & 0x3FFF];
            if (ypos == 0xD0) break; // sentinel: no sprites past this one

            if (ypos > 0xE0) ypos -= 256; // partly off the top of the screen
            ypos += 1;                    // first visible row is Y + 1

            int16_t patt_row = (int16_t) (y - ypos);
            if (mag) patt_row >>= 1;
            if (patt_row < 0 || patt_row >= sprite_size) continue;

            if (shown == 0) memset(row_owner, 0, sizeof(row_owner));
            if (++shown > 4) break; // fifth sprite on this line: stop here

            uint8_t color_byte = vram[(attr + 3) & 0x3FFF];
            uint8_t color = (uint8_t) (color_byte & 0x0F);
            // Bit 7 of the colour byte is "early clock": shift 32px left, so a
            // sprite can walk in from off the left edge.
            int16_t xpos = (int16_t) (vram[(attr + 1) & 0x3FFF] + ((color_byte & 0x80) ? -32 : 0));

            uint16_t patt_off = (uint16_t) (patt_table + vram[(attr + 2) & 0x3FFF] * 8 + patt_row);
            uint8_t patt = vram[patt_off & 0x3FFF];

            int16_t end_x = (int16_t) (xpos + sprite_px);
            if (end_x > ACTIVE_W) end_x = ACTIVE_W;

            uint8_t patt_bit = 0;
            for (int16_t x = xpos, bit = 0; x < end_x; x++, bit++) {
                if (x >= 0 && (patt & 0x80)) {
                    if (color != 0 && row_owner[x] < 2) canvas[y][x] = color;
                    if (!row_owner[x]) row_owner[x] = (uint8_t) (color + 1);
                }
                // Magnified sprites hold each pattern bit for two pixels.
                if (!mag || (bit & 1)) {
                    patt = (uint8_t) (patt << 1);
                    if (++patt_bit == 8 && size16) {
                        // Second 8 columns of a 16x16 sprite: quadrants C and
                        // D sit two patterns (16 bytes) after A and B.
                        patt_bit = 0;
                        patt = vram[(patt_off + 16) & 0x3FFF];
                    }
                }
            }
        }
    }
}

// Real TMS9918 hardware fills the whole overscan border with the backdrop
// colour, not just the active area. -1 forces the first frame to paint it
// over whatever the panel came up holding; after that we only repaint when
// the backdrop register actually changes.
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

    // R1 bit 6 blanks the active area to the backdrop without disturbing
    // anything in VRAM — how a program hides a screen while redrawing it.
    if (regs[1] & 0x40) {
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
    }

    lcd_blit_indexed(BORDER_X, BORDER_Y, ACTIVE_W, ACTIVE_H, &canvas[0][0], ACTIVE_W, VIDEO_PALETTE_BASE);
    lcd_present(); // no-op unless lcd_blit_indexed actually changed something
}

static void video_render_init(void) {
    for (int i = 0; i < 16; i++) {
        lcd_set_palette((uint8_t) (VIDEO_PALETTE_BASE + i), TMS_PALETTE[i]);
    }
}

// Handover to the launcher on Core 0, which draws with the same lcd_* calls.
// Two flags rather than a lock: Core 0 asks, Core 1 answers when it is
// between frames, and neither ever waits on the other for long. Core 1 keeps
// its interrupts on while it idles so the SID sample interrupt carries on and
// so flash_safe_execute() can still park it for a media write.
static volatile bool suspend_requested;

// Starts true so a request arriving before Core 1 has reached the frame loop
// is already satisfied — the renderer cannot be holding the LCD if it has not
// started — and Core 1 then sees the request on its way in.
static volatile bool suspended = true;

void video_render_suspend(void) {
    suspend_requested = true;
    while (!suspended) tight_loop_contents();
}

void video_render_resume(void) {
    // The launcher has drawn over the framebuffer, so nothing on screen can be
    // assumed to still match the last frame rendered. lcd_blit_indexed()
    // compares against the framebuffer itself and repaints the active area of
    // its own accord; only the border is tracked separately, so only it has to
    // be told to forget what it last painted.
    last_border_color = -1;
    suspend_requested = false;
    // No wait for Core 1 to pick the screen back up: Core 0 giving up the LCD
    // is the whole of the handover, and the next frame is 33ms away at worst.
}

void video_render_core1_entry(void) {
    video_render_init();

    while (true) {
        if (suspend_requested) {
            suspended = true;
            // Idling rather than spinning flat out: the menu can be up for a
            // long time, and this core has nothing to do but stay reachable
            // by the SID's sample interrupt and by flash_safe_execute()'s
            // lockout, both of which are interrupts.
            while (suspend_requested) sleep_ms(1);
            continue;
        }
        suspended = false;

        absolute_time_t frame_start = get_absolute_time();
        render_frame();
        int64_t elapsed_ms = absolute_time_diff_us(frame_start, get_absolute_time()) / 1000;
        if (elapsed_ms < FRAME_PERIOD_MS) {
            sleep_ms((uint32_t) (FRAME_PERIOD_MS - elapsed_ms));
        }
    }
}
