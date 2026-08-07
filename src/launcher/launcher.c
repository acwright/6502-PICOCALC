#include "launcher.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"
#include "bios_rom.h"
#include "kbd/kbd.h"
#include "lcd/lcd.h"
#include "machine/machine.h"
#include "machine/media.h"
#include "machine/ram_bank.h"
#include "machine/settings.h"
#include "machine/video_render.h"

// ---------------------------------------------------------------------
// Text, in the machine's own character set
// ---------------------------------------------------------------------
//
// The BIOS carries an IBM CP437 font at $B800-$BFFF, which InitVideo copies
// into the VDP's pattern table — so borrowing it here is why the launcher's
// text is the same text BASIC draws. Each glyph is 8 rows of 5 pixels, left
// aligned in the byte, drawn in a 6x8 cell exactly as the VDP's text mode
// spaces them. Always the built-in BIOS, never a loaded ROM override: this is
// the firmware's own screen, and it has to keep working when the ROM in the
// socket is one that does not boot.
#define FONT_OFFSET (0xB800 - 0x8000)

#define CELL_W   6
#define CELL_H   8
#define MARGIN_X 4
#define MARGIN_Y 4
#define COLS ((LCD_WIDTH - 2 * MARGIN_X) / CELL_W)   // 52
#define ROWS ((LCD_HEIGHT - 2 * MARGIN_Y) / CELL_H)  // 39

// Palette slots 0-8 are lcd.h's named colours and 16-31 are the TMS9918's
// (video_render.h). The seven in between are free, so the launcher's own
// colours live there and nothing has to be reloaded when it closes.
enum {
    COLOR_BG = 9,
    COLOR_DIM,
    COLOR_ACCENT,
};

// Row layout. The list panel gets everything between the header block and
// the key bar at the bottom.
#define ROW_TITLE  0
#define ROW_ROM    2
#define ROW_CART   3
#define ROW_HEAD   5
#define ROW_LIST   7
#define ROW_STATUS (ROWS - 3)
#define ROW_KEYS   (ROWS - 1)
#define LIST_ROWS  (ROW_STATUS - ROW_LIST - 1)

static void draw_char(int col, int row, char c, uint8_t fg, uint8_t bg) {
    if ((unsigned) col >= COLS || (unsigned) row >= ROWS) return;

    const uint8_t *glyph = &bios_rom[FONT_OFFSET + (uint8_t) c * 8];
    uint8_t cell[CELL_H][CELL_W];

    for (int y = 0; y < CELL_H; y++) {
        for (int x = 0; x < CELL_W; x++) {
            bool on = x < 5 && (glyph[y] & (0x80 >> x));
            cell[y][x] = on ? fg : bg;
        }
    }

    lcd_blit_indexed(MARGIN_X + col * CELL_W, MARGIN_Y + row * CELL_H,
                     CELL_W, CELL_H, &cell[0][0], CELL_W, 0);
}

static void draw_text(int col, int row, const char *s, uint8_t fg, uint8_t bg) {
    for (int i = 0; s[i] && col + i < COLS; i++) {
        draw_char(col + i, row, s[i], fg, bg);
    }
}

// A whole row in one colour, for the title and key bars and for wiping a line
// before redrawing it.
static void fill_row(int row, uint8_t color) {
    lcd_fill_rect(0, MARGIN_Y + row * CELL_H, LCD_WIDTH, CELL_H, color);
}

// Text on a bar of its own, padded out so the bar is the full width.
static void draw_bar(int row, const char *left, const char *right) {
    fill_row(row, COLOR_ACCENT);
    draw_text(1, row, left, LCD_COLOR_BLACK, COLOR_ACCENT);
    if (right) {
        int col = COLS - 1 - (int) strlen(right);
        if (col > 0) draw_text(col, row, right, LCD_COLOR_BLACK, COLOR_ACCENT);
    }
}

static void clear_rows(int first, int count) {
    lcd_fill_rect(0, MARGIN_Y + first * CELL_H, LCD_WIDTH, count * CELL_H, COLOR_BG);
}

// ---------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------

typedef enum {
    KEY_NONE = 0,
    KEY_UP,
    KEY_DOWN,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,
    KEY_BACK,
    KEY_ANY,
} launcher_key_t;

// One keyboard poll. Every event is still handed to kbd_decode(), whose
// return value is thrown away but whose bookkeeping is not: it tracks the
// Ctrl key and the keys standing in for the joystick, and skipping it while
// the launcher is up would leave a key released in here still held down as
// far as the machine is concerned.
static launcher_key_t poll_key(void) {
    uint8_t state, code;
    if (kbd_poll_raw(&state, &code) != 0) return KEY_NONE;

    (void) kbd_decode(state, code);

    // Presses only. The controller repeats a held key as a stream of hold
    // events, which at this poll rate would run a list off its end.
    if (state != KBD_STATE_PRESS) return KEY_NONE;

    // Left and Right are reported as themselves rather than folded into
    // "back" here, because the settings screen needs them to change a value.
    // The lists still treat Left as back — see run_list().
    switch (code) {
        case KBD_KEY_UP_RAW:        return KEY_UP;
        case KBD_KEY_DOWN_RAW:      return KEY_DOWN;
        case KBD_KEY_PAGE_UP:       return KEY_PAGE_UP;
        case KBD_KEY_PAGE_DOWN:     return KEY_PAGE_DOWN;
        case KBD_KEY_LEFT_RAW:      return KEY_LEFT;
        case KBD_KEY_RIGHT_RAW:     return KEY_RIGHT;
        case KBD_KEY_ENTER_RAW:     return KEY_ENTER;
        case KBD_KEY_ESC_RAW:       return KEY_BACK;
        default:                    return KEY_ANY;
    }
}

// kbd_poll_raw() spends ~16ms of its own on the I2C round trip, so this is
// already paced and needs no delay of its own.
static launcher_key_t wait_key(void) {
    for (;;) {
        launcher_key_t key = poll_key();
        if (key != KEY_NONE) return key;
    }
}

// ---------------------------------------------------------------------
// Status line
// ---------------------------------------------------------------------

static void show_status(const char *text) {
    clear_rows(ROW_STATUS, 1);
    draw_text(1, ROW_STATUS, text, LCD_COLOR_WHITE, COLOR_BG);
    lcd_present();
}

// Progress for the flash writes, which are slow enough to need it: a 32KB ROM
// is eight sector erases with Core 1 parked for each.
static void show_progress(uint32_t done, uint32_t total) {
    char line[COLS + 1];
    snprintf(line, sizeof(line), "WRITING TO FLASH... %lu%%",
             (unsigned long) (total ? done * 100 / total : 100));
    show_status(line);
}

// Waits for any key, so a message is read before the screen it is on goes.
static void show_message_and_wait(const char *text) {
    clear_rows(ROW_STATUS, 1);
    draw_text(1, ROW_STATUS, text, LCD_COLOR_WHITE, COLOR_BG);
    draw_bar(ROW_KEYS, "PRESS ANY KEY", NULL);
    lcd_present();
    wait_key();
}

// ---------------------------------------------------------------------
// File list
// ---------------------------------------------------------------------

// FatFs is built without long-name support (ffconf.h FF_USE_LFN 0), so what
// comes back is the 8.3 short name — a file a desktop machine wrote as
// "Space Invaders.bas" lists here as "SPACEI~1.BAS", and loads perfectly
// well under that name. Folder names are matched case-insensitively against
// the short name too, so the Teensy's mixed-case "Programs" opens as-is.
#define MAX_FILES 96
#define NAME_LEN  13

static char file_names[MAX_FILES][NAME_LEN];
static int file_count;

// Read into RAM a chunk at a time on the way to the machine's memory.
static uint8_t copy_buffer[512];

// Directory listing, sorted, dot-files and sub-folders left out. An insertion
// sort because the list is short and it costs no extra memory.
static FRESULT read_folder(const char *folder) {
    file_count = 0;

    DIR dir;
    FRESULT fr = f_opendir(&dir, folder);
    if (fr != FR_OK) return fr;

    for (;;) {
        FILINFO fno;
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;
        if (fno.fname[0] == '.' || (fno.fattrib & (AM_DIR | AM_HID | AM_SYS))) continue;
        if (file_count >= MAX_FILES) break;

        int at = file_count++;
        while (at > 0 && strcmp(file_names[at - 1], fno.fname) > 0) {
            memcpy(file_names[at], file_names[at - 1], NAME_LEN);
            at--;
        }
        strncpy(file_names[at], fno.fname, NAME_LEN - 1);
        file_names[at][NAME_LEN - 1] = '\0';
    }

    f_closedir(&dir);
    return FR_OK;
}

// ---------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------

// A program image is the raw bytes that belong at $0800: a tokenized BASIC
// line chain, and after it, in a .prg, the machine code its `10 SYS 2060`
// stub jumps to. Nothing here looks at the extension — .prg and .bas are the
// same format, and it is the verb that differs, exactly as on the real
// machine where LOAD means "program image to $0800" (see the desktop
// emulator's ProgramImage.ts).
static bool load_program(const char *path, const char *name) {
    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) {
        show_message_and_wait("CANNOT OPEN FILE");
        return false;
    }

    FSIZE_t size = f_size(&file);
    if (size == 0) {
        f_close(&file);
        show_message_and_wait("FILE IS EMPTY");
        return false;
    }
    if (size > MACHINE_PROGRAM_MAX_SIZE) {
        f_close(&file);
        show_message_and_wait("PROGRAM TOO LARGE FOR $0800-$7FFF");
        return false;
    }

    uint16_t addr = MACHINE_PROGRAM_ADDRESS;
    for (;;) {
        UINT got = 0;
        if (f_read(&file, copy_buffer, sizeof(copy_buffer), &got) != FR_OK) {
            f_close(&file);
            show_message_and_wait("READ ERROR");
            return false;
        }
        if (got == 0) break;
        for (UINT i = 0; i < got; i++) machine_poke(addr++, copy_buffer[i]);
    }
    f_close(&file);

    // Writing the image straight into RAM skips BASIC's own LOAD, which is
    // what would otherwise move the end-of-program pointers. Left alone,
    // BASIC still believes the program ends at $0802 and the first variable
    // it allocates — a 7-byte record — lands on the front of the program.
    if (!machine_set_program_end((uint16_t) size)) {
        // Only reachable if BASIC is not the thing running: a cartridge owns
        // the machine, or the Monitor was entered before BASIC ever started.
        // The bytes are in memory regardless, which is what BLOAD would have
        // done, so this is a warning rather than a failure.
        show_message_and_wait("LOADED, BUT BASIC IS NOT RUNNING");
        return true;
    }

    char line[COLS + 1];
    snprintf(line, sizeof(line), "LOADED %s (%lu BYTES) - TYPE RUN", name, (unsigned long) size);
    show_message_and_wait(line);
    return true;
}

// ---------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------

typedef enum {
    FOLDER_PROGRAMS,
    FOLDER_CARTS,
    FOLDER_ROMS,
} folder_t;

static const char *folder_path(folder_t which) {
    switch (which) {
        case FOLDER_CARTS: return "Carts";
        case FOLDER_ROMS:  return "ROMs";
        default:           return "Programs";
    }
}

static const char *folder_title(folder_t which) {
    switch (which) {
        case FOLDER_CARTS: return "CARTS";
        case FOLDER_ROMS:  return "ROMS";
        default:           return "PROGRAMS";
    }
}

static void draw_header(void) {
    char line[COLS + 1];
    const char *rom = media_rom_name();
    const char *cart = media_cart_name();

    draw_bar(ROW_TITLE, "6502-PICOCALC", "F1");

    clear_rows(ROW_ROM, 2);
    snprintf(line, sizeof(line), "ROM:  %s", rom[0] ? rom : "BUILT-IN BIOS");
    draw_text(1, ROW_ROM, line, COLOR_DIM, COLOR_BG);
    snprintf(line, sizeof(line), "CART: %s", cart[0] ? cart : "NONE");
    draw_text(1, ROW_CART, line, COLOR_DIM, COLOR_BG);
}

// Draws a scrolling list and runs it until something is chosen. Returns the
// selected index, or -1 for "went back". `selected` and `top` carry the
// position across calls so a list is where it was left.
static int run_list(const char *title, const char *const *items, int count,
                    int *selected, int *top, const char *keys) {
    for (;;) {
        if (*selected < 0) *selected = 0;
        if (*selected >= count) *selected = count - 1;
        if (*selected < *top) *top = *selected;
        if (*selected >= *top + LIST_ROWS) *top = *selected - LIST_ROWS + 1;
        if (*top < 0) *top = 0;

        // A list longer than the panel says where in it you are, in place of
        // the page numbers 6502-DEV's serial menu prints.
        char pos[16] = "";
        if (count > LIST_ROWS) snprintf(pos, sizeof(pos), "%d/%d", *selected + 1, count);
        draw_bar(ROW_HEAD, title, pos[0] ? pos : NULL);

        clear_rows(ROW_LIST, LIST_ROWS);
        clear_rows(ROW_STATUS, 1);

        for (int i = 0; i < LIST_ROWS && *top + i < count; i++) {
            int index = *top + i;
            bool on = index == *selected;
            uint8_t fg = on ? LCD_COLOR_BLACK : LCD_COLOR_WHITE;
            uint8_t bg = on ? LCD_COLOR_WHITE : COLOR_BG;
            if (on) lcd_fill_rect(0, MARGIN_Y + (ROW_LIST + i) * CELL_H, LCD_WIDTH, CELL_H, bg);
            draw_text(1, ROW_LIST + i, on ? ">" : " ", fg, bg);
            draw_text(3, ROW_LIST + i, items[index], fg, bg);
        }

        draw_bar(ROW_KEYS, keys, NULL);
        lcd_present();

        switch (wait_key()) {
            case KEY_UP:        if (*selected > 0) (*selected)--; break;
            case KEY_DOWN:      if (*selected < count - 1) (*selected)++; break;
            case KEY_PAGE_UP:   *selected -= LIST_ROWS; break;
            case KEY_PAGE_DOWN: *selected += LIST_ROWS; break;
            case KEY_ENTER:     return count > 0 ? *selected : -1;
            case KEY_LEFT:      return -1; // a list goes back on Left as well as Esc
            case KEY_BACK:      return -1;
            default:            break;
        }
    }
}

// The file browser for one of the three folders. Returns true if something
// was loaded, which closes the launcher.
static bool browse(folder_t which) {
    const char *folder = folder_path(which);

    show_status("READING SD CARD...");
    FRESULT fr = read_folder(folder);
    if (fr != FR_OK) {
        char line[COLS + 1];
        // The folders are the ones 6502-DEV keeps, and like it, this makes
        // them if they are missing, so a fresh card comes back with somewhere
        // to put files rather than just an error.
        if (fr == FR_NO_PATH && f_mkdir(folder) == FR_OK) {
            snprintf(line, sizeof(line), "CREATED %s/ ON THE SD CARD - IT IS EMPTY", folder);
        } else {
            snprintf(line, sizeof(line), "CANNOT READ %s/ (FATFS ERROR %d)", folder, (int) fr);
        }
        show_message_and_wait(line);
        return false;
    }
    if (file_count == 0) {
        char line[COLS + 1];
        snprintf(line, sizeof(line), "NOTHING IN %s/ ON THE SD CARD", folder);
        show_message_and_wait(line);
        return false;
    }

    static const char *items[MAX_FILES];
    for (int i = 0; i < file_count; i++) items[i] = file_names[i];

    int selected = 0, top = 0;
    for (;;) {
        int choice = run_list(folder_title(which), items, file_count, &selected, &top,
                              "ENTER LOAD   ESC BACK");
        if (choice < 0) return false;

        char path[8 + NAME_LEN + 2];
        snprintf(path, sizeof(path), "%s/%s", folder, file_names[choice]);

        bool loaded = false;
        switch (which) {
            case FOLDER_PROGRAMS:
                loaded = load_program(path, file_names[choice]);
                break;
            case FOLDER_CARTS:
                show_progress(0, 1);
                loaded = media_load_cart(path, file_names[choice], show_progress);
                show_message_and_wait(loaded ? "CARTRIDGE IN THE SLOT - RESETTING"
                                             : "COULD NOT LOAD CARTRIDGE");
                break;
            case FOLDER_ROMS:
                show_progress(0, 1);
                loaded = media_load_rom(path, file_names[choice], show_progress);
                show_message_and_wait(loaded ? "ROM REPLACED - RESETTING"
                                             : "COULD NOT LOAD ROM");
                break;
        }

        if (loaded) return true;
        // Failed: back to the list, with the header redrawn in case a
        // half-finished media write cleared what was in the slot.
        draw_header();
    }
}

// ---------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------

typedef enum {
    SETTING_CLOCK,
    SETTING_RAM,
    SETTING_BACKLIGHT,
    SETTING_SLEEP,
    SETTING_COUNT,
} setting_t;

// The value column, far enough right to clear the longest label.
#define SETTING_VALUE_COL 20

static void setting_value(setting_t which, char *out, size_t len) {
    switch (which) {
        case SETTING_CLOCK:
            snprintf(out, len, "%s", settings_clock_label(settings_clock_index()));
            break;
        case SETTING_RAM: {
            uint16_t banks = settings_ram_banks();
            if (banks == 0) snprintf(out, len, "NOT FITTED");
            else snprintf(out, len, "%u BANKS (%u KB)", (unsigned) banks,
                          (unsigned) (banks * RAM_BANK_WINDOW_SIZE / 1024));
            break;
        }
        case SETTING_BACKLIGHT:
            snprintf(out, len, "%u%%", (unsigned) (settings_backlight() * 100 / 255));
            break;
        case SETTING_SLEEP: {
            uint16_t seconds = settings_sleep_timeout_s();
            if (seconds == 0) snprintf(out, len, "NEVER");
            else if (seconds % 60 == 0) snprintf(out, len, "%u MIN", (unsigned) (seconds / 60));
            else snprintf(out, len, "%u SEC", (unsigned) seconds);
            break;
        }
        default:
            if (len) out[0] = '\0';
            break;
    }
}

// Left/Right on the highlighted row. Everything except the clock takes effect
// as it is adjusted, so the panel dims under your hand and the machine sees
// the smaller card immediately; the clock is only read once, at power-on, so
// it can only be staged (see settings.h).
static void setting_adjust(setting_t which, int delta) {
    switch (which) {
        case SETTING_CLOCK: {
            int index = (int) settings_clock_index() + delta;
            if (index < 0) index = 0;
            if (index >= settings_clock_count()) index = settings_clock_count() - 1;
            settings_set_clock_index((uint8_t) index);
            break;
        }
        case SETTING_RAM: {
            uint16_t banks = settings_ram_banks();
            if (banks == 0) break; // no card fitted in this build
            uint16_t next = delta > 0 ? (uint16_t) (banks << 1) : (uint16_t) (banks >> 1);
            if (next < 1) next = 1;
            settings_set_ram_banks(next);
            ram_bank_set_active(RAM_BANK_LOW, settings_ram_banks());
            break;
        }
        case SETTING_BACKLIGHT: {
            int value = (int) settings_backlight() + delta * SETTINGS_BACKLIGHT_STEP;
            if (value < SETTINGS_BACKLIGHT_MIN) value = SETTINGS_BACKLIGHT_MIN;
            if (value > 255) value = 255;
            settings_set_backlight((uint8_t) value);
            kbd_set_backlight(settings_backlight());
            break;
        }
        case SETTING_SLEEP: {
            int index = (int) settings_sleep_index() + delta;
            if (index < 0) index = 0;
            if (index >= settings_sleep_count()) index = settings_sleep_count() - 1;
            settings_set_sleep_index((uint8_t) index);
            break;
        }
        default:
            break;
    }
}

static void run_settings(void) {
    static const char *labels[SETTING_COUNT] = {
        "CLOCK SPEED",
        "EXPANSION RAM",
        "BACKLIGHT",
        "SLEEP AFTER",
    };

    int selected = 0;
    uint8_t clock_was = settings_clock_index();

    for (;;) {
        draw_bar(ROW_HEAD, "SETTINGS", NULL);
        clear_rows(ROW_LIST, LIST_ROWS);
        clear_rows(ROW_STATUS, 1);

        for (int i = 0; i < SETTING_COUNT; i++) {
            bool on = i == selected;
            uint8_t fg = on ? LCD_COLOR_BLACK : LCD_COLOR_WHITE;
            uint8_t bg = on ? LCD_COLOR_WHITE : COLOR_BG;
            if (on) lcd_fill_rect(0, MARGIN_Y + (ROW_LIST + i) * CELL_H, LCD_WIDTH, CELL_H, bg);
            draw_text(1, ROW_LIST + i, on ? ">" : " ", fg, bg);
            draw_text(3, ROW_LIST + i, labels[i], fg, bg);

            char value[32];
            setting_value((setting_t) i, value, sizeof(value));
            draw_text(SETTING_VALUE_COL, ROW_LIST + i, value, fg, bg);
        }

        // The battery reading, such as it is. Read here rather than on a timer
        // because this loop only comes round on a keypress, which is already
        // far slower than the ~16ms the I2C round trip costs.
        //
        // The controller hands back two bytes whose meaning is not documented
        // in anything this port has to go on, so they are shown raw rather
        // than dressed up as a percentage that might be a lie. Worth
        // revisiting against a real charge/discharge cycle.
        char battery[COLS + 1];
        int raw = kbd_read_battery();
        if (raw < 0) snprintf(battery, sizeof(battery), "BATTERY: NO ANSWER FROM CONTROLLER");
        else snprintf(battery, sizeof(battery), "BATTERY: RAW $%04X", (unsigned) raw);
        draw_text(1, ROW_LIST + SETTING_COUNT + 1, battery, COLOR_DIM, COLOR_BG);

        if (settings_clock_index() != clock_was) {
            draw_text(1, ROW_STATUS, "CLOCK CHANGES ON NEXT POWER-ON", LCD_COLOR_WHITE, COLOR_BG);
        }

        draw_bar(ROW_KEYS, "LEFT/RIGHT CHANGE   ESC BACK", NULL);
        lcd_present();

        launcher_key_t key = wait_key();
        if (key == KEY_BACK || key == KEY_ENTER) break;

        switch (key) {
            case KEY_UP:    if (selected > 0) selected--; break;
            case KEY_DOWN:  if (selected < SETTING_COUNT - 1) selected++; break;
            case KEY_LEFT:  setting_adjust((setting_t) selected, -1); break;
            case KEY_RIGHT: setting_adjust((setting_t) selected, +1); break;
            default:        break;
        }
    }

    // Straight to flash rather than waiting on the settle timer: the machine
    // is stopped and Core 1 is parked already, so the erase costs nothing that
    // is not already being paid, and the setting is safe against the power
    // going off before the outer loop next runs.
    settings_flush();
}

typedef enum {
    ITEM_PROGRAMS,
    ITEM_CARTS,
    ITEM_ROMS,
    ITEM_EJECT_CART,
    ITEM_BUILTIN_ROM,
    ITEM_SETTINGS,
    ITEM_RESET,
    ITEM_POWER_CYCLE,
    ITEM_RESUME,
} item_t;

void launcher_run(void) {
    // Core 1 draws the machine's screen with the same lcd_* calls; the panel
    // has to belong to one of us at a time.
    video_render_suspend();

    lcd_set_palette(COLOR_BG, lcd_rgb565(0, 0, 0));
    lcd_set_palette(COLOR_DIM, lcd_rgb565(150, 150, 150));
    lcd_set_palette(COLOR_ACCENT, lcd_rgb565(200, 200, 200));
    lcd_clear(COLOR_BG);

    int selected = 0, top = 0;
    bool reset_needed = false;
    bool cold_start = false;

    for (;;) {
        draw_header();

        item_t ids[9];
        const char *labels[9];
        int count = 0;

        ids[count] = ITEM_PROGRAMS;    labels[count++] = "LOAD PROGRAM";
        ids[count] = ITEM_CARTS;       labels[count++] = "LOAD CARTRIDGE";
        ids[count] = ITEM_ROMS;        labels[count++] = "LOAD ROM";
        if (media_cart_name()[0]) {
            ids[count] = ITEM_EJECT_CART;  labels[count++] = "EJECT CARTRIDGE";
        }
        if (media_rom_name()[0]) {
            ids[count] = ITEM_BUILTIN_ROM; labels[count++] = "RESTORE BUILT-IN BIOS";
        }
        ids[count] = ITEM_SETTINGS;    labels[count++] = "SETTINGS";
        ids[count] = ITEM_RESET;       labels[count++] = "RESET MACHINE";
        ids[count] = ITEM_POWER_CYCLE; labels[count++] = "POWER CYCLE (CLEARS RAM)";
        ids[count] = ITEM_RESUME;      labels[count++] = "RESUME";

        int choice = run_list("LAUNCHER", labels, count, &selected, &top,
                              "ENTER SELECT   ESC RESUME");
        if (choice < 0) break;

        bool loaded = false;
        switch (ids[choice]) {
            case ITEM_PROGRAMS:
                // A program goes straight into the machine's RAM, so it needs
                // no reset — and must not get one, which would clear it.
                if (browse(FOLDER_PROGRAMS)) goto done;
                break;
            case ITEM_CARTS:
                loaded = browse(FOLDER_CARTS);
                break;
            case ITEM_ROMS:
                loaded = browse(FOLDER_ROMS);
                break;
            case ITEM_EJECT_CART:
                show_status("EJECTING...");
                media_eject_cart();
                loaded = true;
                break;
            case ITEM_BUILTIN_ROM:
                show_status("RESTORING...");
                media_restore_builtin_rom();
                loaded = true;
                break;
            case ITEM_SETTINGS:
                run_settings();
                lcd_clear(COLOR_BG);
                break;
            case ITEM_RESET:
                reset_needed = true;
                goto done;
            case ITEM_POWER_CYCLE:
                reset_needed = true;
                cold_start = true;
                goto done;
            case ITEM_RESUME:
                goto done;
        }

        if (loaded) {
            reset_needed = true;
            goto done;
        }
        lcd_clear(COLOR_BG);
    }

done:
    // machine_reset() is also what re-reads the ROM and cartridge images, so
    // anything the launcher put in a socket takes effect here, on reset, and
    // not before — the same rule as swapping a chip with the power off. A
    // load or an eject only ever asks for a warm reset; clearing RAM is
    // reserved for the power-cycle item, which is the one thing a program
    // sitting in memory should not survive.
    if (reset_needed) machine_reset(cold_start);

    video_render_resume();
}

bool launcher_hotkey(uint8_t state, uint8_t code) {
    return state == KBD_STATE_PRESS && code == KBD_KEY_F1;
}
