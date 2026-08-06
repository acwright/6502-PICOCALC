#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"

// Error register bits.
#define ST_ERR_AMNF   0x01 // address mark not found (used here as "no media")
#define ST_ERR_ABRT   0x04 // aborted command
#define ST_ERR_IDNF   0x10 // sector ID not found (LBA out of range)
#define ST_ERR_UNC    0x40 // uncorrectable error (backing-file I/O failure)

// Status register bits. The emulated card is always RDY and never BSY, as on
// the Teensy firmware — every command completes inside the store to $8C07.
#define ST_STATUS_ERR 0x01
#define ST_STATUS_DRQ 0x08
#define ST_STATUS_RDY 0x40

// ATA commands the BIOS and the desktop emulator use.
#define ST_CMD_READ      0x20
#define ST_CMD_READ_ALT  0x21
#define ST_CMD_WRITE     0x30
#define ST_CMD_WRITE_ALT 0x31
#define ST_CMD_ERASE     0xC0
#define ST_CMD_IDENTIFY  0xEC
#define ST_CMD_SET_FEAT  0xEF

// A new image is created one disk bank long (2048 sectors), matching `cffs
// create`'s default. It grows a bank at a time as the machine writes higher
// LBAs (see image_grow_to()), rather than pre-filling all 256 MB the way the
// Teensy firmware does — that would take many minutes over SPI, and the BIOS
// filesystem reads unwritten space as blank either way (see image_read()).
#define STORAGE_BANK_SECTORS 2048

static uint8_t buffer[STORAGE_SECTOR_SIZE];
static uint32_t buffer_index;
static uint32_t command_data_size;
static uint32_t sector_offset;

static uint8_t error;
static uint8_t feature;
static uint8_t sector_count;
static uint8_t lba0, lba1, lba2, lba3;
static uint8_t status;
static uint8_t command;

static bool identifying;  // IDENTIFY DRIVE data is being read out of buffer[]
static bool transferring; // a read/write command is streaming sectors

static FIL image;
static bool available;

// One sector of zeros, in flash: used to fill new/extended image space.
static const uint8_t zero_sector[STORAGE_SECTOR_SIZE] = {0};

// IDENTIFY DRIVE response for the emulated 256 MB card (2048 cylinders x
// 8 heads x 32 sectors = 524288 sectors). Byte-for-byte the table 6502-DEV's
// StorageCard::generateIdentity() builds; the remaining 388 bytes are zero.
// The BIOS never issues $EC (StInit uses SET FEATURES), but the desktop
// emulator answers it, so this card does too.
static const uint8_t identity_head[124] = {
    0x84, 0x8A,             // removable disk, hard-sectored
    0x00, 0x08,             // 2048 cylinders
    0x00, 0x00,             // reserved
    0x08, 0x00,             // 8 heads
    0x00, 0x40,             // 16384 unformatted bytes per track
    0x00, 0x02,             // 512 unformatted bytes per sector
    0x20, 0x00,             // 32 sectors per track
    0x08, 0x00, 0x00, 0x00, // 524288 sectors per card
    0x00, 0x00,             // reserved
    'A','C','W','D','6','5','0','2','E','M','U','C','F','1','0','1','0','1','0','1', // serial #
    0x01, 0x00,             // buffer type
    0x04, 0x00,             // buffer size in 512-byte increments
    0x04, 0x00,             // ECC bytes passed
    '1','.','0',' ',' ',' ',' ',' ', // firmware revision
    'A','C','W','D','6','5','0','2','E','M','U','C','F', // model #, space padded
    ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
    ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
    0x01, 0x00,             // max 1 sector on read/write multiple
    0x00, 0x00,             // double word transfer not supported
    0x00, 0x02,             // capabilities: LBA supported
    0x00, 0x00,             // reserved
    0x00, 0x02,             // PIO data transfer cycle timing mode
    0x00, 0x00,             // DMA not supported
    0x01, 0x00,             // field validity
    0x00, 0x08,             // 2048 current cylinders
    0x08, 0x00,             // 8 current heads
    0x20, 0x00,             // 32 current sectors per track
    0x00, 0x00, 0x08, 0x00, // current capacity in sectors
    0x01, 0x01,             // multiple sector setting
    0x00, 0x00, 0x08, 0x00, // total sectors in LBA mode
};

//
// Backing image
//

static uint32_t image_sectors(void) {
    return (uint32_t) (f_size(&image) / STORAGE_SECTOR_SIZE);
}

// Appends zeroed sectors until the image covers `sectors`. New space always
// gets written out explicitly rather than seeking past EOF, which would leave
// whatever the SD card's free clusters happened to hold in the middle of a
// filesystem the BIOS is about to read back.
static bool image_grow_to(uint32_t sectors) {
    uint32_t have = image_sectors();
    if (have >= sectors) return true;

    if (f_lseek(&image, (FSIZE_t) have * STORAGE_SECTOR_SIZE) != FR_OK) return false;
    for (uint32_t s = have; s < sectors; s++) {
        UINT bw;
        if (f_write(&image, zero_sector, STORAGE_SECTOR_SIZE, &bw) != FR_OK ||
            bw != STORAGE_SECTOR_SIZE) {
            f_sync(&image);
            return false;
        }
    }
    return f_sync(&image) == FR_OK;
}

// Sectors past the end of the image read back blank: an image only as long as
// the disk banks actually used still presents all 256 banks to the machine,
// with the unused ones simply empty (a blank directory sector).
static bool image_read(uint32_t sector, uint8_t *dst) {
    if (!available) return false;

    if (sector >= image_sectors()) {
        memset(dst, 0, STORAGE_SECTOR_SIZE);
        return true;
    }

    UINT br;
    if (f_lseek(&image, (FSIZE_t) sector * STORAGE_SECTOR_SIZE) != FR_OK) return false;
    if (f_read(&image, dst, STORAGE_SECTOR_SIZE, &br) != FR_OK) return false;
    if (br < STORAGE_SECTOR_SIZE) memset(dst + br, 0, STORAGE_SECTOR_SIZE - br);
    return true;
}

static bool image_write(uint32_t sector, const uint8_t *src) {
    if (!available) return false;

    // Round the growth up to a whole disk bank so that walking up through the
    // banks doesn't rewrite the tail of the image on every SAVE.
    if (sector >= image_sectors()) {
        uint32_t banks = sector / STORAGE_BANK_SECTORS + 1;
        if (!image_grow_to(banks * STORAGE_BANK_SECTORS)) return false;
    }

    UINT bw;
    if (f_lseek(&image, (FSIZE_t) sector * STORAGE_SECTOR_SIZE) != FR_OK) return false;
    if (f_write(&image, src, STORAGE_SECTOR_SIZE, &bw) != FR_OK) return false;
    if (bw != STORAGE_SECTOR_SIZE) return false;
    // Flush per sector: the machine has no eject or shutdown step, so an image
    // left in FatFs's cache would be lost on the power cycle that the whole
    // point of this card is to survive.
    return f_sync(&image) == FR_OK;
}

bool storage_init(void) {
    available = false;

    FRESULT fr = f_open(&image, STORAGE_IMAGE_PATH, FA_READ | FA_WRITE);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        fr = f_open(&image, STORAGE_IMAGE_PATH, FA_CREATE_NEW | FA_READ | FA_WRITE);
        if (fr != FR_OK) {
            printf("storage: cannot create %s (FRESULT=%d) - CF card absent\n",
                   STORAGE_IMAGE_PATH, fr);
            return false;
        }
        printf("storage: creating blank %s (%d KB)...\n",
               STORAGE_IMAGE_PATH, STORAGE_BANK_SECTORS * STORAGE_SECTOR_SIZE / 1024);
        available = true; // image_grow_to() writes through the same guard
        if (!image_grow_to(STORAGE_BANK_SECTORS)) {
            printf("storage: write failed while creating %s - CF card absent\n",
                   STORAGE_IMAGE_PATH);
            f_close(&image);
            available = false;
            return false;
        }
    } else if (fr != FR_OK) {
        printf("storage: cannot open %s (FRESULT=%d) - CF card absent\n",
               STORAGE_IMAGE_PATH, fr);
        return false;
    }

    available = true;
    printf("storage: %s mounted, %lu sectors (%lu KB, %lu disk bank(s))\n",
           STORAGE_IMAGE_PATH, (unsigned long) image_sectors(),
           (unsigned long) (f_size(&image) / 1024),
           (unsigned long) ((image_sectors() + STORAGE_BANK_SECTORS - 1) / STORAGE_BANK_SECTORS));
    return true;
}

bool storage_available(void) {
    return available;
}

//
// Register model
//

void storage_reset(void) {
    buffer_index = 0;
    command_data_size = STORAGE_SECTOR_SIZE;
    sector_offset = 0;

    error = 0x00;
    feature = 0x00;
    sector_count = 0x00;
    lba0 = 0x00;
    lba1 = 0x00;
    lba2 = 0x00;
    lba3 = 0xE0; // LBA mode, master drive
    status = ST_STATUS_RDY;
    command = 0x00;

    identifying = false;
    transferring = false;

    memset(buffer, 0, sizeof(buffer));
}

static uint32_t sector_index(void) {
    return ((uint32_t) (lba3 & 0x0F) << 24) | ((uint32_t) lba2 << 16) |
           ((uint32_t) lba1 << 8) | lba0;
}

static bool sector_valid(void) {
    return sector_index() < STORAGE_SECTOR_COUNT;
}

static void execute_command(void) {
    // New command, so clear the previous one's error and data-request flags.
    status &= (uint8_t) ~ST_STATUS_ERR;
    status &= (uint8_t) ~ST_STATUS_DRQ;
    error = 0x00;
    command_data_size = (uint32_t) STORAGE_SECTOR_SIZE * sector_count;
    buffer_index = 0;
    sector_offset = 0;

    if (!available) {
        status |= ST_STATUS_ERR;
        error |= ST_ERR_AMNF;
        return;
    }
    if (transferring || identifying) {
        // Already mid-transfer: abort rather than interleave commands.
        status |= ST_STATUS_ERR;
        error |= ST_ERR_ABRT;
        return;
    }

    switch (command) {
        case ST_CMD_ERASE:
            if (!sector_valid()) {
                status |= ST_STATUS_ERR;
                error |= ST_ERR_ABRT | ST_ERR_IDNF;
            } else if (!image_write(sector_index(), zero_sector)) {
                status |= ST_STATUS_ERR;
                error |= ST_ERR_UNC;
            }
            break;

        case ST_CMD_IDENTIFY:
            memcpy(buffer, identity_head, sizeof(identity_head));
            memset(buffer + sizeof(identity_head), 0,
                   STORAGE_SECTOR_SIZE - sizeof(identity_head));
            command_data_size = STORAGE_SECTOR_SIZE; // always one sector of identity data
            status |= ST_STATUS_DRQ;
            identifying = true;
            break;

        case ST_CMD_READ:
        case ST_CMD_READ_ALT:
            if (!sector_valid()) {
                status |= ST_STATUS_ERR;
                error |= ST_ERR_ABRT | ST_ERR_IDNF;
            } else if (!image_read(sector_index(), buffer)) {
                status |= ST_STATUS_ERR;
                error |= ST_ERR_UNC;
            } else {
                // First sector is in the buffer; the rest (if sector_count > 1)
                // are fetched as the machine drains it (see read_buffer()).
                status |= ST_STATUS_DRQ;
                transferring = true;
            }
            break;

        case ST_CMD_WRITE:
        case ST_CMD_WRITE_ALT:
            if (!sector_valid()) {
                status |= ST_STATUS_ERR;
                error |= ST_ERR_ABRT | ST_ERR_IDNF;
            } else {
                status |= ST_STATUS_DRQ;
                transferring = true;
            }
            break;

        case ST_CMD_SET_FEAT:
            // Features (the BIOS sets $01, 8-bit data I/O) are accepted without
            // error; this card is 8-bit only regardless. StInit takes the
            // command completing as proof a card is fitted and sets HW_CF.
            break;

        default:
            status |= ST_STATUS_ERR;
            error |= ST_ERR_ABRT;
            break;
    }
}

static uint8_t read_buffer(void) {
    if (identifying) {
        uint8_t data = buffer[buffer_index];

        if (buffer_index < command_data_size - 1) {
            buffer_index++;
        } else {
            buffer_index = 0;
            identifying = false;
            status &= (uint8_t) ~ST_STATUS_DRQ;
        }

        return data;
    }

    if (transferring) {
        uint8_t data = buffer[buffer_index];

        if (buffer_index < STORAGE_SECTOR_SIZE - 1) {
            buffer_index++;
        } else {
            buffer_index = 0;
            sector_offset++;

            if (sector_offset < sector_count) { // multi-sector read: load the next one
                if (!image_read(sector_index() + sector_offset, buffer)) {
                    status |= ST_STATUS_ERR;
                    error |= ST_ERR_UNC;
                    transferring = false;
                    status &= (uint8_t) ~ST_STATUS_DRQ;
                }
            } else {
                transferring = false;
                status &= (uint8_t) ~ST_STATUS_DRQ;
            }
        }

        return data;
    }

    return 0x00;
}

static void write_buffer(uint8_t value) {
    buffer[buffer_index] = value;

    if (buffer_index < STORAGE_SECTOR_SIZE - 1) {
        buffer_index++;
        return;
    }

    buffer_index = 0;
    if (!image_write(sector_index() + sector_offset, buffer)) {
        status |= ST_STATUS_ERR;
        error |= ST_ERR_UNC;
    }

    sector_offset++;
    if (sector_offset >= sector_count) {
        transferring = false;
        status &= (uint8_t) ~ST_STATUS_DRQ;
    }
}

uint8_t storage_read(uint16_t addr) {
    switch (addr & 0x07) {
        case 0: return read_buffer();
        case 1: return error;
        case 2: return sector_count;
        case 3: return lba0;
        case 4: return lba1;
        case 5: return lba2;
        case 6: return lba3;
        case 7:
            // With no image behind it the card reads back as an empty socket:
            // RDY never comes up, so the BIOS's StInit probe times out and
            // leaves HW_CF clear instead of hanging the storage stack later.
            return available ? status : 0x00;
        default: return 0x00;
    }
}

void storage_write(uint16_t addr, uint8_t value) {
    switch (addr & 0x07) {
        case 0: write_buffer(value); break;
        case 1: feature = value; break;
        case 2: sector_count = value; break;
        case 3: lba0 = value; break;
        case 4: lba1 = value; break;
        case 5: lba2 = value; break;
        case 6: lba3 = (uint8_t) ((value & 0x0F) | 0xE0); break; // LBA mode, master only
        case 7:
            command = value;
            execute_command();
            break;
    }
}
