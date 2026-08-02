// Phase 2: SD card block driver — SD bus in SPI mode on SPI0, per PLAN.md §2
// (SCLK=18, MOSI=19, MISO=16, CS=17, DET=22). Implements just enough of the
// standard SD-over-SPI command set (CMD0/CMD8/CMD55+ACMD41/CMD58/CMD16/
// CMD17/CMD24) for FatFs's diskio.c (see src/fatfs/diskio.c) to sit on top;
// no vendor library, this is the well-known publicly documented protocol.
#ifndef SD_H
#define SD_H

#include <stdbool.h>
#include <stdint.h>

#define SD_BLOCK_SIZE 512

typedef enum {
    SD_OK = 0,
    SD_ERR_NO_CARD,
    SD_ERR_TIMEOUT,
    SD_ERR_CMD,
    SD_ERR_IO,
} sd_status_t;

// Reads the card-detect switch (SPI0 DET pin). true if a card appears to be
// present; wiring/polarity is unverified on hardware, so disk_initialize()
// does not gate on this alone (see sd.c).
bool sd_card_detect(void);

// Brings up SPI0 at init-safe (~400kHz) speed and idles CS high. Safe to
// call once at boot; does not talk to the card yet.
void sd_bus_init(void);

// Runs the SD SPI power-up/idle/ACMD41 handshake, detects SDHC/SDXC (block
// addressing) vs SDSC (byte addressing), and switches the bus to full
// operating speed on success. Returns SD_OK if a card is ready for
// block I/O.
sd_status_t sd_card_init(void);

bool sd_is_initialized(void);
bool sd_is_block_addressed(void); // true: SDHC/SDXC (CMD17/24 take block #)

sd_status_t sd_read_block(uint32_t lba, uint8_t *buf512);
sd_status_t sd_write_block(uint32_t lba, const uint8_t *buf512);

#endif
