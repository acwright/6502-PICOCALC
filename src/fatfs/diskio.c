// Phase 2: FatFs low-level disk I/O glue for the SD driver in src/sd — NOT
// the stock diskio.c template (which is left unimplemented stubs). Only
// physical drive 0 exists (the SD card on SPI0; see PLAN.md §2/Phase 2).
#include "ff.h"
#include "diskio.h"

#include "../sd/sd.h"

#define DEV_SD 0

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != DEV_SD) return STA_NOINIT;
    return sd_is_initialized() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != DEV_SD) return STA_NOINIT;
    if (sd_is_initialized()) return 0;
    return (sd_card_init() == SD_OK) ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != DEV_SD) return RES_PARERR;
    for (UINT i = 0; i < count; i++) {
        if (sd_read_block((uint32_t) sector + i, buff + i * SD_BLOCK_SIZE) != SD_OK) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != DEV_SD) return RES_PARERR;
    for (UINT i = 0; i < count; i++) {
        if (sd_write_block((uint32_t) sector + i, buff + i * SD_BLOCK_SIZE) != SD_OK) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != DEV_SD) return RES_PARERR;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK; // writes are not cached above the card itself
        case GET_SECTOR_SIZE:
            *(WORD *) buff = SD_BLOCK_SIZE;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *) buff = 1; // erase block size unknown; report 1 sector
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
