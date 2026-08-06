#include "nvram.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/time.h"

// The store lives in the last sector of flash. The firmware image starts at
// the bottom, and even with the BIOS ROM embedded it is nowhere near either
// board's 2MB/4MB, so the top sector is free on both targets.
#define NVRAM_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

// 'N','V','R','1' -- bumped if the record layout below ever changes, so an
// old record is treated as absent rather than misread.
#define NVRAM_MAGIC 0x3152564Eu

// How long the bytes must go untouched before they are written back.
#define NVRAM_SETTLE_MS 2000

typedef struct {
    uint32_t magic;
    uint32_t checksum; // sum of data[], to reject a half-written record
    uint8_t data[NVRAM_SIZE];
} nvram_record_t;

// flash_range_program() writes whole 256-byte pages, so the record is staged
// in a page-multiple buffer with the tail left as erased flash (0xFF).
#define NVRAM_PROGRAM_SIZE ((sizeof(nvram_record_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)

static uint8_t nvram[NVRAM_SIZE];
static bool dirty;
static absolute_time_t last_write;
static uint8_t program_buffer[NVRAM_PROGRAM_SIZE];

static uint32_t checksum_of(const uint8_t *data) {
    uint32_t sum = 0;
    for (size_t i = 0; i < NVRAM_SIZE; i++) sum += data[i];
    return sum;
}

// Runs with interrupts off on this core and Core 1 parked out of flash by
// flash_safe_execute() -- it must not call anything that lives in flash
// itself, which is what __not_in_flash_func() below guarantees.
static void __not_in_flash_func(nvram_program)(void *param) {
    (void) param;
    flash_range_erase(NVRAM_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(NVRAM_FLASH_OFFSET, program_buffer, NVRAM_PROGRAM_SIZE);
}

void nvram_init(void) {
    const nvram_record_t *stored = (const nvram_record_t *) (XIP_BASE + NVRAM_FLASH_OFFSET);

    if (stored->magic == NVRAM_MAGIC && stored->checksum == checksum_of(stored->data)) {
        memcpy(nvram, stored->data, NVRAM_SIZE);
    } else {
        memset(nvram, 0x00, NVRAM_SIZE);
    }

    dirty = false;
    last_write = get_absolute_time();
}

uint8_t nvram_read(uint8_t addr) {
    return nvram[addr];
}

void nvram_write(uint8_t addr, uint8_t value) {
    if (nvram[addr] == value) return; // no flash wear for a no-op write

    nvram[addr] = value;
    dirty = true;
    last_write = get_absolute_time();
}

bool nvram_flush(void) {
    if (!dirty) return true;

    nvram_record_t *record = (nvram_record_t *) program_buffer;
    memset(program_buffer, 0xFF, sizeof(program_buffer));
    record->magic = NVRAM_MAGIC;
    record->checksum = checksum_of(nvram);
    memcpy(record->data, nvram, NVRAM_SIZE);

    // The BIOS's ProbeRTC writes a test byte into NVRAM address 0 and puts
    // the original back on every boot, which leaves the bytes dirty but
    // unchanged; without this the sector would be erased and rewritten once
    // per power-on for nothing.
    const void *stored = (const void *) (XIP_BASE + NVRAM_FLASH_OFFSET);
    if (memcmp(stored, program_buffer, sizeof(nvram_record_t)) == 0) {
        dirty = false;
        return true;
    }

    // Core 1 is running the renderer and the SID sample interrupt out of
    // flash, so it has to be parked before the sector can be erased; that is
    // what flash_safe_execute() does (Core 1 opts in by calling
    // flash_safe_execute_core_init(), see main.c). The whole erase+program
    // takes tens of milliseconds, during which the 6502 stops and the
    // display and audio hold still -- the reason writes are batched by
    // nvram_task() rather than done per store.
    int rc = flash_safe_execute(nvram_program, NULL, 1000);
    if (rc != PICO_OK) return false;

    dirty = false;
    return true;
}

void nvram_task(void) {
    if (!dirty) return;
    if (absolute_time_diff_us(last_write, get_absolute_time()) < NVRAM_SETTLE_MS * 1000) return;

    if (!nvram_flush()) {
        // Couldn't park Core 1 (it may be mid-DMA with interrupts masked).
        // Leave the bytes dirty and retry after another settle period rather
        // than spinning on it every pass.
        last_write = get_absolute_time();
    }
}
