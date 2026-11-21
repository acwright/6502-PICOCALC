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
// Ready reads $50, RDY and DSC (seek complete), as a real card does.
#define ST_STATUS_ERR 0x01
#define ST_STATUS_DRQ 0x08
#define ST_STATUS_DSC 0x10
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
// 8 heads x 32 sectors = 524288 sectors). The same table 6502-DEV's
// StorageCard::generateIdentity() builds; the remaining 388 bytes are zero.
// The BIOS never issues $EC (StInit uses SET FEATURES), but the desktop
// emulator answers it, so this card does too.
//
// Laid out as a real card sends it: every word low byte first, and in the
// strings the first character of each pair in the high byte, so the pair "AC"
// is sent 'C', 'A' and a driver swaps each word back.
static const uint8_t identity_head[124] = {
    0x8A, 0x84,             // general configuration $848A: a CompactFlash card
    0x00, 0x08,             // 2048 cylinders
    0x00, 0x00,             // reserved
    0x08, 0x00,             // 8 heads
    0x00, 0x40,             // 16384 unformatted bytes per track
    0x00, 0x02,             // 512 unformatted bytes per sector
    0x20, 0x00,             // 32 sectors per track
    0x08, 0x00, 0x00, 0x00, // 524288 sectors per card
    0x00, 0x00,             // reserved
    'C','A','D','W','5','6','2','0','M','E','C','U','1','F','1','0','1','0','1','0', // serial # ACWD6502EMUCF1010101
    0x01, 0x00,             // buffer type
    0x04, 0x00,             // buffer size in 512-byte increments
    0x04, 0x00,             // ECC bytes passed
    '.','1',' ','0',' ',' ',' ',' ', // firmware revision "1.0"
    'C','A','D','W','5','6','2','0','M','E','C','U',' ','F', // model # ACWD6502EMUCF, space padded
    ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
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
    status = ST_STATUS_RDY | ST_STATUS_DSC;
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

// Ends any transfer with an error, as a real card does: DRQ clears and the
// data register stops moving bytes.
static void fail(uint8_t err) {
    status |= ST_STATUS_ERR;
    status &= (uint8_t) ~ST_STATUS_DRQ;
    error = err;
    transferring = false;
    identifying = false;
    buffer_index = 0;
}

// The sectors this command moves: its count, with 0 meaning 256 (ATA).
static uint32_t transfer_sectors(void) {
    return command_data_size / STORAGE_SECTOR_SIZE;
}

static bool reading(void) { return command == ST_CMD_READ || command == ST_CMD_READ_ALT; }
static bool writing(void) { return command == ST_CMD_WRITE || command == ST_CMD_WRITE_ALT; }

static void execute_command(void) {
    if (!available) {
        fail(ST_ERR_AMNF);
        return;
    }
    if (transferring || identifying) {
        // A command in the middle of another one's transfer ends that transfer
        // and is refused, as a real card does.
        fail(ST_ERR_ABRT);
        return;
    }

    // New command, so clear the previous one's error and data-request flags.
    status &= (uint8_t) ~ST_STATUS_ERR;
    status &= (uint8_t) ~ST_STATUS_DRQ;
    error = 0x00;
    command_data_size = (uint32_t) STORAGE_SECTOR_SIZE * (sector_count ? sector_count : 256);
    buffer_index = 0;
    sector_offset = 0;

    switch (command) {
        case ST_CMD_ERASE: // `count` sectors from the LBA
            for (uint32_t i = 0; i < transfer_sectors(); i++) {
                uint32_t sector = sector_index() + i;
                if (sector >= STORAGE_SECTOR_COUNT) {
                    fail(ST_ERR_ABRT | ST_ERR_IDNF);
                    break;
                }
                if (!image_write(sector, zero_sector)) {
                    fail(ST_ERR_UNC);
                    break;
                }
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
                fail(ST_ERR_ABRT | ST_ERR_IDNF);
            } else if (!image_read(sector_index(), buffer)) {
                fail(ST_ERR_UNC);
            } else {
                // First sector is in the buffer; the rest (if more than one)
                // are fetched as the machine drains it (see read_buffer()).
                status |= ST_STATUS_DRQ;
                transferring = true;
            }
            break;

        case ST_CMD_WRITE:
        case ST_CMD_WRITE_ALT:
            if (!sector_valid()) {
                fail(ST_ERR_ABRT | ST_ERR_IDNF);
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
            fail(ST_ERR_ABRT);
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

    if (transferring && reading()) {
        uint8_t data = buffer[buffer_index];

        if (buffer_index < STORAGE_SECTOR_SIZE - 1) {
            buffer_index++;
        } else {
            buffer_index = 0;
            sector_offset++;

            if (sector_offset < transfer_sectors()) { // multi-sector read: load the next one
                uint32_t sector = sector_index() + sector_offset;
                if (sector >= STORAGE_SECTOR_COUNT) fail(ST_ERR_ABRT | ST_ERR_IDNF);
                else if (!image_read(sector, buffer)) fail(ST_ERR_UNC);
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
    // Only a write command's transfer takes data: outside one (DRQ clear) a
    // real card ignores the data register.
    if (!transferring || !writing()) return;
    buffer[buffer_index] = value;

    if (buffer_index < STORAGE_SECTOR_SIZE - 1) {
        buffer_index++;
        return;
    }

    buffer_index = 0;
    if (!image_write(sector_index() + sector_offset, buffer)) {
        fail(ST_ERR_UNC);
        return;
    }

    sector_offset++;
    if (sector_offset >= transfer_sectors()) {
        transferring = false;
        status &= (uint8_t) ~ST_STATUS_DRQ;
    } else if (sector_index() + sector_offset >= STORAGE_SECTOR_COUNT) {
        fail(ST_ERR_ABRT | ST_ERR_IDNF); // the next sector is past the end of the card
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
