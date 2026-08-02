// Phase 1: LCD driver — ST7365P/ILI9488-compatible 320x320 SPI panel.
//
// Pin/panel init sequence ported from clockworkpi/PicoCalc's
// picocalc_helloworld/lcdspi example (see PLAN.md §8 reuse map), adapted to
// this project's 8bpp-palette framebuffer + DMA line-buffer presentation
// model. The panel is driven at 18bpp (3 bytes/pixel): the ILI9488/ST7365P
// controller does not support 16bpp RGB565 over 4-wire SPI, only 18bpp, so
// each palette entry is expanded to R/G/B bytes at present time.
#include <stdbool.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#include "lcd.h"

// PLAN.md §2: SPI1 SCK=10, MOSI=11, MISO=12, CS=13, DC=14, RST=15.
#define LCD_SPI      spi1
#define LCD_PIN_SCK  10
#define LCD_PIN_MOSI 11
#define LCD_PIN_MISO 12
#define LCD_PIN_CS   13
#define LCD_PIN_DC   14
#define LCD_PIN_RST  15

// PLAN.md §2: "25 MHz, up to 50". Start conservative; can be raised once
// signal integrity on real hardware is confirmed.
#define LCD_SPI_BAUDRATE (25 * 1000 * 1000)

static uint8_t framebuffer[LCD_HEIGHT][LCD_WIDTH];
static uint16_t palette[256];
// One converted scanline, 3 bytes/pixel (18bpp).
static uint8_t line_buf[LCD_WIDTH * 3];

// Bounding box of framebuffer writes since the last lcd_present(). Without
// this, re-sending the whole panel every frame races the panel's own
// internal GRAM scanout and tears/flickers anything on screen even when
// the pixels haven't changed (PLAN.md §7 risk: "dirty-region / changed-
// line updates").
static bool fb_dirty;
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1;

static void mark_dirty(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LCD_WIDTH) x1 = LCD_WIDTH;
    if (y1 > LCD_HEIGHT) y1 = LCD_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;
    if (!fb_dirty) {
        dirty_x0 = x0;
        dirty_y0 = y0;
        dirty_x1 = x1;
        dirty_y1 = y1;
        fb_dirty = true;
    } else {
        if (x0 < dirty_x0) dirty_x0 = x0;
        if (y0 < dirty_y0) dirty_y0 = y0;
        if (x1 > dirty_x1) dirty_x1 = x1;
        if (y1 > dirty_y1) dirty_y1 = y1;
    }
}

// ---------------------------------------------------------------------
// Low-level bus helpers
// ---------------------------------------------------------------------

static inline void lcd_cs_low(void)  { gpio_put(LCD_PIN_CS, 0); }
static inline void lcd_cs_high(void) { gpio_put(LCD_PIN_CS, 1); }

// PL022 is full-duplex: every TX word also shifts in an RX word. DMA only
// drives TX, so drain the RX FIFO and wait for the shifter to go idle
// before dropping CS / changing frame width, or the last few pixels of a
// region get truncated.
static void lcd_spi_drain(spi_inst_t *spi) {
    while (spi_is_readable(spi)) (void) spi_get_hw(spi)->dr;
    while (spi_get_hw(spi)->sr & SPI_SSPSR_BSY_BITS) tight_loop_contents();
    while (spi_is_readable(spi)) (void) spi_get_hw(spi)->dr;
    spi_get_hw(spi)->icr = SPI_SSPICR_RORIC_BITS;
}

static void lcd_write_cmd(uint8_t cmd) {
    gpio_put(LCD_PIN_DC, 0);
    lcd_cs_low();
    spi_write_blocking(LCD_SPI, &cmd, 1);
    lcd_cs_high();
}

static void lcd_write_data(const uint8_t *buf, size_t len) {
    gpio_put(LCD_PIN_DC, 1);
    lcd_cs_low();
    spi_write_blocking(LCD_SPI, buf, len);
    lcd_cs_high();
}

static void lcd_write_data1(uint8_t data) {
    lcd_write_data(&data, 1);
}

static void lcd_reset(void) {
    gpio_put(LCD_PIN_RST, 1);
    sleep_ms(10);
    gpio_put(LCD_PIN_RST, 0);
    sleep_ms(10);
    gpio_put(LCD_PIN_RST, 1);
    sleep_ms(200);
}

// Panel init sequence (gamma/power/vcom/interface tuning ported verbatim
// from the ClockworkPi example). COLMOD is 0x66 (18bpp): ILI9488/ST7365P
// only supports 18bpp — not 16bpp RGB565 — over 4-wire SPI.
static void lcd_panel_init(void) {
    lcd_reset();

    lcd_write_cmd(0xE0); // Positive Gamma Control
    static const uint8_t gamma_pos[] = {
        0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78,
        0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,
    };
    lcd_write_data(gamma_pos, sizeof(gamma_pos));

    lcd_write_cmd(0xE1); // Negative Gamma Control
    static const uint8_t gamma_neg[] = {
        0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45,
        0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,
    };
    lcd_write_data(gamma_neg, sizeof(gamma_neg));

    lcd_write_cmd(0xC0); // Power Control 1
    lcd_write_data((const uint8_t[]){0x17, 0x15}, 2);

    lcd_write_cmd(0xC1); // Power Control 2
    lcd_write_data1(0x41);

    lcd_write_cmd(0xC5); // VCOM Control
    lcd_write_data((const uint8_t[]){0x00, 0x12, 0x80}, 3);

    lcd_write_cmd(0x36); // Memory Access Control (MADCTL): MX + BGR
    lcd_write_data1(0x48);

    lcd_write_cmd(0x3A); // Pixel Interface Format: 18 bits/pixel (RGB666)
    lcd_write_data1(0x66);

    lcd_write_cmd(0xB0); // Interface Mode Control
    lcd_write_data1(0x00);

    lcd_write_cmd(0xB1); // Frame Rate Control
    lcd_write_data1(0xA0);

    lcd_write_cmd(0x21); // Display Inversion On

    lcd_write_cmd(0xB4); // Display Inversion Control
    lcd_write_data1(0x02);

    lcd_write_cmd(0xB6); // Display Function Control
    lcd_write_data((const uint8_t[]){0x02, 0x02, 0x3B}, 3);

    lcd_write_cmd(0xB7); // Entry Mode Set
    lcd_write_data1(0xC6);

    lcd_write_cmd(0xE9);
    lcd_write_data1(0x00);

    lcd_write_cmd(0xF7); // Adjust Control 3
    lcd_write_data((const uint8_t[]){0xA9, 0x51, 0x2C, 0x82}, 4);

    lcd_write_cmd(0x11); // Sleep Out
    sleep_ms(120);

    lcd_write_cmd(0x29); // Display On
    sleep_ms(120);
}

// ---------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------

void lcd_init(void) {
    gpio_init(LCD_PIN_CS);
    gpio_init(LCD_PIN_DC);
    gpio_init(LCD_PIN_RST);
    gpio_set_dir(LCD_PIN_CS, GPIO_OUT);
    gpio_set_dir(LCD_PIN_DC, GPIO_OUT);
    gpio_set_dir(LCD_PIN_RST, GPIO_OUT);
    gpio_put(LCD_PIN_CS, 1);
    gpio_put(LCD_PIN_RST, 1);

    spi_init(LCD_SPI, LCD_SPI_BAUDRATE);
    gpio_set_function(LCD_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_MISO, GPIO_FUNC_SPI);

    lcd_panel_init();

    static const struct { uint8_t index, r, g, b; } defaults[] = {
        {LCD_COLOR_BLACK,   0,   0,   0},
        {LCD_COLOR_WHITE, 255, 255, 255},
        {LCD_COLOR_RED,   255,   0,   0},
        {LCD_COLOR_GREEN,   0, 255,   0},
        {LCD_COLOR_BLUE,    0,   0, 255},
        {LCD_COLOR_YELLOW,255, 255,   0},
        {LCD_COLOR_CYAN,    0, 255, 255},
        {LCD_COLOR_MAGENTA,255,  0, 255},
        {LCD_COLOR_GRAY,  128, 128, 128},
    };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        palette[defaults[i].index] = lcd_rgb565(defaults[i].r, defaults[i].g, defaults[i].b);
    }

    lcd_clear(LCD_COLOR_BLACK);
}

uint16_t lcd_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void lcd_set_palette(uint8_t index, uint16_t rgb565) {
    palette[index] = rgb565;
}

// ---------------------------------------------------------------------
// Framebuffer drawing primitives
// ---------------------------------------------------------------------

void lcd_clear(uint8_t index) {
    memset(framebuffer, index, sizeof(framebuffer));
    mark_dirty(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

void lcd_fill_rect(int x, int y, int w, int h, uint8_t index) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > LCD_WIDTH ? LCD_WIDTH : x + w;
    int y1 = y + h > LCD_HEIGHT ? LCD_HEIGHT : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int row = y0; row < y1; row++) {
        memset(&framebuffer[row][x0], index, x1 - x0);
    }
    mark_dirty(x0, y0, x1, y1);
}

void lcd_put_pixel(int x, int y, uint8_t index) {
    if ((unsigned) x >= LCD_WIDTH || (unsigned) y >= LCD_HEIGHT) return;
    framebuffer[y][x] = index;
    mark_dirty(x, y, x + 1, y + 1);
}

void lcd_blit_indexed(int x, int y, int w, int h, const uint8_t *src, int src_stride, uint8_t palette_base) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > LCD_WIDTH ? LCD_WIDTH : x + w;
    int y1 = y + h > LCD_HEIGHT ? LCD_HEIGHT : y + h;
    if (x0 >= x1 || y0 >= y1) return;

    static uint8_t row_buf[LCD_WIDTH];
    bool any_changed = false;
    int changed_x0 = 0, changed_y0 = 0, changed_x1 = 0, changed_y1 = 0;

    for (int row = y0; row < y1; row++) {
        const uint8_t *src_row = src + (size_t) (row - y) * (size_t) src_stride + (x0 - x);
        int w0 = x1 - x0;
        for (int i = 0; i < w0; i++) {
            row_buf[i] = (uint8_t) (palette_base + src_row[i]);
        }
        if (memcmp(&framebuffer[row][x0], row_buf, (size_t) w0) == 0) continue;

        memcpy(&framebuffer[row][x0], row_buf, (size_t) w0);
        if (!any_changed) {
            changed_x0 = x0;
            changed_y0 = row;
            changed_x1 = x1;
            changed_y1 = row + 1;
            any_changed = true;
        } else {
            changed_y1 = row + 1;
        }
    }

    if (any_changed) mark_dirty(changed_x0, changed_y0, changed_x1, changed_y1);
}

// ---------------------------------------------------------------------
// Minimal 5x8 debug font — space, '-', '.', ',', ':', 0-9, A-Z (lowercase
// folds to uppercase). Enough for boot banners and stat lines; not a
// general-purpose console font.
// ---------------------------------------------------------------------

static const uint8_t GLYPH_SPACE[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t GLYPH_DASH[8]  = {0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x00};
static const uint8_t GLYPH_DOT[8]   = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00};
static const uint8_t GLYPH_COMMA[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08};
static const uint8_t GLYPH_COLON[8] = {0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00};

static const uint8_t GLYPH_0[8] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00};
static const uint8_t GLYPH_1[8] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00};
static const uint8_t GLYPH_2[8] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F, 0x00};
static const uint8_t GLYPH_3[8] = {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E, 0x00};
static const uint8_t GLYPH_4[8] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02, 0x00};
static const uint8_t GLYPH_5[8] = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E, 0x00};
static const uint8_t GLYPH_6[8] = {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E, 0x00};
static const uint8_t GLYPH_7[8] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08, 0x00};
static const uint8_t GLYPH_8[8] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E, 0x00};
static const uint8_t GLYPH_9[8] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C, 0x00};

static const uint8_t GLYPH_A[8] = {0x04, 0x0A, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x00};
static const uint8_t GLYPH_B[8] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E, 0x00};
static const uint8_t GLYPH_C[8] = {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F, 0x00};
static const uint8_t GLYPH_D[8] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E, 0x00};
static const uint8_t GLYPH_E[8] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F, 0x00};
static const uint8_t GLYPH_F[8] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x00};
static const uint8_t GLYPH_G[8] = {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F, 0x00};
static const uint8_t GLYPH_H[8] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x00};
static const uint8_t GLYPH_I[8] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00};
static const uint8_t GLYPH_J[8] = {0x03, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E, 0x00};
static const uint8_t GLYPH_K[8] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11, 0x00};
static const uint8_t GLYPH_L[8] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F, 0x00};
static const uint8_t GLYPH_M[8] = {0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11, 0x00};
static const uint8_t GLYPH_N[8] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11, 0x00};
static const uint8_t GLYPH_O[8] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00};
static const uint8_t GLYPH_P[8] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10, 0x00};
static const uint8_t GLYPH_Q[8] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D, 0x00};
static const uint8_t GLYPH_R[8] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11, 0x00};
static const uint8_t GLYPH_S[8] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E, 0x00};
static const uint8_t GLYPH_T[8] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00};
static const uint8_t GLYPH_U[8] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00};
static const uint8_t GLYPH_V[8] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04, 0x00};
static const uint8_t GLYPH_W[8] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11, 0x00};
static const uint8_t GLYPH_X[8] = {0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11, 0x00};
static const uint8_t GLYPH_Y[8] = {0x11, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00};
static const uint8_t GLYPH_Z[8] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F, 0x00};

static const uint8_t *font_glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char) (c - 32);
    switch (c) {
        case '-': return GLYPH_DASH;
        case '.': return GLYPH_DOT;
        case ',': return GLYPH_COMMA;
        case ':': return GLYPH_COLON;
        case '0': return GLYPH_0;
        case '1': return GLYPH_1;
        case '2': return GLYPH_2;
        case '3': return GLYPH_3;
        case '4': return GLYPH_4;
        case '5': return GLYPH_5;
        case '6': return GLYPH_6;
        case '7': return GLYPH_7;
        case '8': return GLYPH_8;
        case '9': return GLYPH_9;
        case 'A': return GLYPH_A;
        case 'B': return GLYPH_B;
        case 'C': return GLYPH_C;
        case 'D': return GLYPH_D;
        case 'E': return GLYPH_E;
        case 'F': return GLYPH_F;
        case 'G': return GLYPH_G;
        case 'H': return GLYPH_H;
        case 'I': return GLYPH_I;
        case 'J': return GLYPH_J;
        case 'K': return GLYPH_K;
        case 'L': return GLYPH_L;
        case 'M': return GLYPH_M;
        case 'N': return GLYPH_N;
        case 'O': return GLYPH_O;
        case 'P': return GLYPH_P;
        case 'Q': return GLYPH_Q;
        case 'R': return GLYPH_R;
        case 'S': return GLYPH_S;
        case 'T': return GLYPH_T;
        case 'U': return GLYPH_U;
        case 'V': return GLYPH_V;
        case 'W': return GLYPH_W;
        case 'X': return GLYPH_X;
        case 'Y': return GLYPH_Y;
        case 'Z': return GLYPH_Z;
        default: return GLYPH_SPACE;
    }
}

void lcd_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg) {
    const uint8_t *glyph = font_glyph(c);
    for (int row = 0; row < LCD_FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < LCD_FONT_WIDTH; col++) {
            bool set = (bits >> (LCD_FONT_WIDTH - 1 - col)) & 1;
            lcd_put_pixel(x + col, y + row, set ? fg : bg);
        }
    }
    mark_dirty(x, y, x + LCD_FONT_WIDTH, y + LCD_FONT_HEIGHT);
}

void lcd_draw_string(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    int cx = x, cy = y;
    for (; *s; s++) {
        if (*s == '\n') {
            cx = x;
            cy += LCD_CHAR_ADVANCE_Y;
            continue;
        }
        lcd_draw_char(cx, cy, *s, fg, bg);
        cx += LCD_CHAR_ADVANCE_X;
    }
}

// ---------------------------------------------------------------------
// Presentation: 8bpp framebuffer -> RGB666 (3 bytes/pixel) -> panel. A full
// 18bpp frame (320x320x3 = 307 KB) can't be buffered on the RP2040
// (264 KB SRAM), so the frame is streamed one converted scanline at a time.
// ---------------------------------------------------------------------

// Expands each palette entry (stored RGB565) to the panel's 3-byte RGB666
// format. Byte order R,G,B matches the ClockworkPi reference driver's
// working output with MADCTL 0x48.
static void lcd_convert_row(int y, int x0, int x1, uint8_t *out) {
    const uint8_t *row = framebuffer[y];
    for (int x = x0; x < x1; x++) {
        uint16_t v = palette[row[x]];
        *out++ = (uint8_t) ((v >> 8) & 0xF8); // R: 565 bits 15..11 -> byte 7..3
        *out++ = (uint8_t) ((v >> 3) & 0xFC); // G: 565 bits 10..5  -> byte 7..2
        *out++ = (uint8_t) ((v << 3) & 0xF8); // B: 565 bits 4..0   -> byte 7..3
    }
}

// Sends a single command byte with CS already held low by the caller.
static inline void lcd_stream_cmd(uint8_t cmd) {
    gpio_put(LCD_PIN_DC, 0);
    spi_write_blocking(LCD_SPI, &cmd, 1);
    gpio_put(LCD_PIN_DC, 1);
}

// Pushes framebuffer[y0..y1) x [x0..x1) to the panel. Callers keep this
// region as small as possible: re-sending pixels that haven't changed
// races the panel's own free-running GRAM scanout and tears/flickers
// (PLAN.md §7).
//
// CS is held low for the entire CASET/PASET/RAMWR/pixel transaction (only
// DC toggles), exactly like the ClockworkPi reference's define_region_spi.
// Releasing CS between the address commands lets the ILI9488 mis-latch the
// column/page registers for regions with non-zero high address bytes
// (e.g. x0=304 -> CASET {0x01,0x30,...}), shifting partial windows to the
// wrong place while a full frame (all-zero start bytes) still looked fine.
static void lcd_present_region(int x0, int y0, int x1, int y1) {
    if (x0 >= x1 || y0 >= y1) return;
    int xe = x1 - 1, ye = y1 - 1;
    uint8_t caset[4] = {(uint8_t) (x0 >> 8), (uint8_t) x0, (uint8_t) (xe >> 8), (uint8_t) xe};
    uint8_t paset[4] = {(uint8_t) (y0 >> 8), (uint8_t) y0, (uint8_t) (ye >> 8), (uint8_t) ye};

    lcd_cs_low();

    lcd_stream_cmd(0x2A); // CASET
    spi_write_blocking(LCD_SPI, caset, 4);
    lcd_stream_cmd(0x2B); // PASET
    spi_write_blocking(LCD_SPI, paset, 4);
    lcd_stream_cmd(0x2C); // RAMWR

    int bytes = (x1 - x0) * 3;
    for (int y = y0; y < y1; y++) {
        lcd_convert_row(y, x0, x1, line_buf);
        spi_write_blocking(LCD_SPI, line_buf, (size_t) bytes);
    }

    lcd_spi_drain(LCD_SPI);
    lcd_cs_high();
}

void lcd_present(void) {
    if (!fb_dirty) return;
    lcd_present_region(dirty_x0, dirty_y0, dirty_x1, dirty_y1);
    fb_dirty = false;
}

void lcd_present_full(void) {
    lcd_present_region(0, 0, LCD_WIDTH, LCD_HEIGHT);
    fb_dirty = false;
}

// ---------------------------------------------------------------------
// Test pattern
// ---------------------------------------------------------------------

void lcd_draw_test_pattern(void) {
    static const uint8_t bars[] = {
        LCD_COLOR_WHITE, LCD_COLOR_YELLOW, LCD_COLOR_CYAN, LCD_COLOR_GREEN,
        LCD_COLOR_MAGENTA, LCD_COLOR_RED, LCD_COLOR_BLUE, LCD_COLOR_BLACK,
    };
    int bar_count = (int) (sizeof(bars) / sizeof(bars[0]));
    int bar_w = LCD_WIDTH / bar_count;

    for (int i = 0; i < bar_count; i++) {
        lcd_fill_rect(i * bar_w, 0, bar_w, LCD_HEIGHT, bars[i]);
    }

    lcd_fill_rect(0, LCD_HEIGHT / 2 - 12, LCD_WIDTH, 24, LCD_COLOR_BLACK);
    lcd_draw_string(8, LCD_HEIGHT / 2 - 8, "6502-PICOCALC LCD TEST",
                     LCD_COLOR_WHITE, LCD_COLOR_BLACK);
}
