#include "settings.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/time.h"

#include "media.h"
#include "ram_bank.h"

// One sector, immediately below everything machine/media.c reserves (its
// NVRAM sector, its header, and the ROM and cartridge images) — see
// MEDIA_RESERVED_FLASH_BASE in media.h, which exists so this offset and
// media.c's own layout cannot drift apart.
#define SETTINGS_FLASH_OFFSET (MEDIA_RESERVED_FLASH_BASE - FLASH_SECTOR_SIZE)

// 'S','E','T','1' — bumped if the record below ever changes shape, so an old
// one is treated as absent (and the defaults apply) rather than misread.
#define SETTINGS_MAGIC 0x31544553u

// How long a value must go untouched before it is written back, so holding a
// key on the brightness row costs one erase rather than one per step.
#define SETTINGS_SETTLE_MS 2000

// The clock steps, fastest last. Index 1 is what every build up to now ran at
// unconditionally, so it stays the default and nothing changes for a machine
// that has never opened the settings screen.
//
// Deliberately just two: see settings.h on why a persisted clock speed is not
// the place for an untested one.
typedef struct {
    uint32_t khz;
    enum vreg_voltage vreg;
    const char *label;
} clock_step_t;

static const clock_step_t clock_steps[] = {
#if PICO_RP2350
    { 150000, VREG_VOLTAGE_1_10, "150 MHZ (STOCK)" },
#else
    { 125000, VREG_VOLTAGE_1_10, "125 MHZ (STOCK)" },
#endif
    { 200000, VREG_VOLTAGE_1_15, "200 MHZ" },
};

#define CLOCK_STEP_COUNT   ((uint8_t) (sizeof(clock_steps) / sizeof(clock_steps[0])))
#define CLOCK_STEP_DEFAULT 1

// Offered sleep timeouts. 0 is "never", and is the default so that a machine
// that has never been into the settings screen behaves exactly as it did
// before this setting existed.
static const uint16_t sleep_steps[] = { 0, 30, 60, 120, 300, 600 };

#define SLEEP_STEP_COUNT ((uint8_t) (sizeof(sleep_steps) / sizeof(sleep_steps[0])))

#define BACKLIGHT_DEFAULT 200

typedef struct {
    uint8_t clock_index;
    uint16_t ram_banks;
    uint8_t backlight;
    uint16_t sleep_timeout_s;
} settings_data_t;

typedef struct {
    uint32_t magic;
    uint32_t checksum; // over data, to reject a half-written record
    settings_data_t data;
} settings_record_t;

// flash_range_program() writes whole 256-byte pages, so the record is staged
// in a page-multiple buffer with the tail left as erased flash (0xFF).
#define SETTINGS_PROGRAM_SIZE \
    ((sizeof(settings_record_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)

static settings_data_t settings;
static bool dirty;
static absolute_time_t last_change;
static uint8_t program_buffer[SETTINGS_PROGRAM_SIZE];

static uint32_t checksum_of(const settings_data_t *data) {
    const uint8_t *bytes = (const uint8_t *) data;
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof(*data); i++) sum += bytes[i];
    return sum;
}

// Largest power of two not greater than `value`, for the bank count — the
// card's addressing folds a short bank count over the full 256-bank space by
// masking, which only works on a power of two (see ram_bank.c).
static uint16_t floor_pow2(uint16_t value) {
    uint16_t result = 1;
    while ((result << 1) && (result << 1) <= value) result <<= 1;
    return result;
}

// Forced on every load, not just in the setters. A record can be checksum-
// valid and still illegal for the firmware now reading it — a rebuild that
// lowers the compiled bank count or drops a clock step leaves a perfectly
// intact record describing a machine this build cannot make. Clamping here
// means a stale value can never reach set_sys_clock_khz() or leave the panel
// dark.
static bool clamp_settings(settings_data_t *data) {
    settings_data_t before = *data;

    if (data->clock_index >= CLOCK_STEP_COUNT) data->clock_index = CLOCK_STEP_DEFAULT;

    uint16_t built = ram_bank_banks(RAM_BANK_LOW);
    if (built == 0) {
        data->ram_banks = 0; // card not compiled in at all
    } else {
        if (data->ram_banks > built) data->ram_banks = built;
        if (data->ram_banks < 1) data->ram_banks = 1;
        data->ram_banks = floor_pow2(data->ram_banks);
    }

    if (data->backlight < SETTINGS_BACKLIGHT_MIN) data->backlight = SETTINGS_BACKLIGHT_MIN;

    // Must be one of the offered steps, so the settings screen's cycling can
    // always find its way back to the stored value.
    bool known = false;
    for (uint8_t i = 0; i < SLEEP_STEP_COUNT; i++) {
        if (sleep_steps[i] == data->sleep_timeout_s) { known = true; break; }
    }
    if (!known) data->sleep_timeout_s = 0;

    return memcmp(&before, data, sizeof(before)) != 0;
}

static void mark_dirty(void) {
    dirty = true;
    last_change = get_absolute_time();
}

// Runs with interrupts off on this core and Core 1 parked out of flash by
// flash_safe_execute() -- it must not call anything that lives in flash
// itself, which is what __not_in_flash_func() below guarantees.
static void __not_in_flash_func(settings_program)(void *param) {
    (void) param;
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, program_buffer, SETTINGS_PROGRAM_SIZE);
}

void settings_init(void) {
    const settings_record_t *stored = (const settings_record_t *) (XIP_BASE + SETTINGS_FLASH_OFFSET);

    bool valid = stored->magic == SETTINGS_MAGIC &&
                 stored->checksum == checksum_of(&stored->data);

    if (valid) {
        memcpy(&settings, &stored->data, sizeof(settings));
    } else {
        // Named defaults rather than a zero fill: zero would mean a dark
        // backlight and whatever clock step happened to be first, neither of
        // which is a state anyone asked for.
        settings.clock_index = CLOCK_STEP_DEFAULT;
        settings.ram_banks = ram_bank_banks(RAM_BANK_LOW); // however much this build has
        settings.backlight = BACKLIGHT_DEFAULT;
        settings.sleep_timeout_s = 0; // never
    }

    bool changed = clamp_settings(&settings);

    dirty = false;
    last_change = get_absolute_time();

    // Only worth a write if what is in flash does not already say this. A
    // first boot has nothing stored; a rebuild may have clamped something.
    if (!valid || changed) mark_dirty();
}

bool settings_flush(void) {
    if (!dirty) return true;

    settings_record_t *record = (settings_record_t *) program_buffer;
    memset(program_buffer, 0xFF, sizeof(program_buffer));
    record->magic = SETTINGS_MAGIC;
    record->checksum = checksum_of(&settings);
    memcpy(&record->data, &settings, sizeof(settings));

    // Nothing to do if flash already holds exactly this.
    const void *stored = (const void *) (XIP_BASE + SETTINGS_FLASH_OFFSET);
    if (memcmp(stored, program_buffer, sizeof(settings_record_t)) == 0) {
        dirty = false;
        return true;
    }

    // Core 1 runs the renderer and the SID sample interrupt out of flash, so
    // it has to be parked before the sector can be erased; that is what
    // flash_safe_execute() does (Core 1 opts in via
    // flash_safe_execute_core_init(), see main.c).
    if (flash_safe_execute(settings_program, NULL, 1000) != PICO_OK) return false;

    dirty = false;
    return true;
}

void settings_task(void) {
    if (!dirty) return;
    if (absolute_time_diff_us(last_change, get_absolute_time()) < SETTINGS_SETTLE_MS * 1000) return;

    if (!settings_flush()) {
        // Couldn't park Core 1 (it may be mid-DMA with interrupts masked).
        // Leave it dirty and retry after another settle period rather than
        // spinning on it every pass.
        last_change = get_absolute_time();
    }
}

uint8_t settings_clock_count(void) {
    return CLOCK_STEP_COUNT;
}

uint8_t settings_clock_index(void) {
    return settings.clock_index;
}

uint32_t settings_clock_khz(void) {
    return clock_steps[settings.clock_index].khz;
}

enum vreg_voltage settings_clock_vreg(void) {
    return clock_steps[settings.clock_index].vreg;
}

const char *settings_clock_label(uint8_t index) {
    if (index >= CLOCK_STEP_COUNT) index = CLOCK_STEP_DEFAULT;
    return clock_steps[index].label;
}

void settings_set_clock_index(uint8_t index) {
    if (index >= CLOCK_STEP_COUNT) return;
    if (settings.clock_index == index) return;
    settings.clock_index = index;
    mark_dirty();
}

uint16_t settings_ram_banks(void) {
    return settings.ram_banks;
}

void settings_set_ram_banks(uint16_t banks) {
    uint16_t built = ram_bank_banks(RAM_BANK_LOW);
    if (built == 0) return;

    if (banks > built) banks = built;
    if (banks < 1) banks = 1;
    banks = floor_pow2(banks);

    if (settings.ram_banks == banks) return;
    settings.ram_banks = banks;
    mark_dirty();
}

uint8_t settings_backlight(void) {
    return settings.backlight;
}

void settings_set_backlight(uint8_t value) {
    if (value < SETTINGS_BACKLIGHT_MIN) value = SETTINGS_BACKLIGHT_MIN;
    if (settings.backlight == value) return;
    settings.backlight = value;
    mark_dirty();
}

uint16_t settings_sleep_timeout_s(void) {
    return settings.sleep_timeout_s;
}

void settings_set_sleep_timeout_s(uint16_t seconds) {
    if (settings.sleep_timeout_s == seconds) return;
    settings.sleep_timeout_s = seconds;
    mark_dirty();
}

uint8_t settings_sleep_count(void) {
    return SLEEP_STEP_COUNT;
}

uint16_t settings_sleep_at(uint8_t index) {
    if (index >= SLEEP_STEP_COUNT) return 0;
    return sleep_steps[index];
}

uint8_t settings_sleep_index(void) {
    for (uint8_t i = 0; i < SLEEP_STEP_COUNT; i++) {
        if (sleep_steps[i] == settings.sleep_timeout_s) return i;
    }
    return 0;
}

void settings_set_sleep_index(uint8_t index) {
    if (index >= SLEEP_STEP_COUNT) return;
    settings_set_sleep_timeout_s(sleep_steps[index]);
}
