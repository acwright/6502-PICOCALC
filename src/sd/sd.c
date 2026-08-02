// Phase 2: SD card block driver — SD bus in SPI mode on SPI0.
// See sd.h and PLAN.md §2/Phase 2. Implements the standard publicly
// documented SD-over-SPI command sequence (no vendor SDK/library):
// CMD0 (idle) -> CMD8 (interface condition, detects v2) -> CMD55+ACMD41
// (until ready) -> CMD58 (OCR, detects SDHC/SDXC block addressing) or
// CMD16 (fixes 512B blocks on SDSC/MMC) -> full-speed CMD17/CMD24 for
// single-block read/write.
#include "sd.h"

#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"

#define SD_SPI_PORT spi0
#define SD_PIN_SCK  18
#define SD_PIN_MOSI 19
#define SD_PIN_MISO 16
#define SD_PIN_CS   17
#define SD_PIN_DET  22

#define SD_INIT_HZ (400 * 1000)
#define SD_FULL_HZ (20 * 1000 * 1000)

#define CMD0   0  // GO_IDLE_STATE
#define CMD8   8  // SEND_IF_COND
#define CMD16  16 // SET_BLOCKLEN
#define CMD17  17 // READ_SINGLE_BLOCK
#define CMD24  24 // WRITE_BLOCK
#define CMD55  55 // APP_CMD
#define CMD58  58 // READ_OCR
#define ACMD41 41 // SD_SEND_OP_COND (must be preceded by CMD55)

static bool s_initialized = false;
static bool s_block_addressed = false; // true: SDHC/SDXC (CMD17/24 take block #)

static inline void cs_low(void)  { gpio_put(SD_PIN_CS, 0); }
static inline void cs_high(void) { gpio_put(SD_PIN_CS, 1); }

static uint8_t sd_xfer(uint8_t out) {
    uint8_t in = 0xFF;
    spi_write_read_blocking(SD_SPI_PORT, &out, &in, 1);
    return in;
}

static void sd_clock_idle(int bytes) {
    for (int i = 0; i < bytes; i++) sd_xfer(0xFF);
}

// Waits for the card to release MISO (idle high) — used both before issuing
// a command and after a write while the card is busy programming flash.
static bool sd_wait_ready(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!time_reached(deadline)) {
        if (sd_xfer(0xFF) == 0xFF) return true;
    }
    return false;
}

// Sends a 6-byte command frame and returns the R1 response byte.
static uint8_t sd_command(uint8_t cmd, uint32_t arg, uint8_t crc) {
    sd_wait_ready(50);

    uint8_t frame[6] = {
        (uint8_t) (0x40 | cmd),
        (uint8_t) (arg >> 24), (uint8_t) (arg >> 16), (uint8_t) (arg >> 8), (uint8_t) arg,
        crc,
    };
    for (int i = 0; i < 6; i++) sd_xfer(frame[i]);

    uint8_t r1 = 0xFF;
    for (int i = 0; i < 8; i++) { // R1 arrives within ~8 clocks (Ncr)
        r1 = sd_xfer(0xFF);
        if ((r1 & 0x80) == 0) break;
    }
    return r1;
}

void sd_bus_init(void) {
    spi_init(SD_SPI_PORT, SD_INIT_HZ);
    gpio_set_function(SD_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(SD_PIN_MISO); // some sockets leave DO floating when idle

    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    cs_high();

    gpio_init(SD_PIN_DET);
    gpio_set_dir(SD_PIN_DET, GPIO_IN);
    gpio_pull_up(SD_PIN_DET);
}

bool sd_card_detect(void) {
    return !gpio_get(SD_PIN_DET);
}

sd_status_t sd_card_init(void) {
    s_initialized = false;
    s_block_addressed = false;

    spi_set_baudrate(SD_SPI_PORT, SD_INIT_HZ);
    cs_high();
    sd_clock_idle(10); // >=74 clocks with CS high before the first command

    cs_low();
    uint8_t r1 = sd_command(CMD0, 0, 0x95);
    printf("sd: CMD0 r1=0x%02x\n", r1);
    if (r1 != 0x01) {
        cs_high();
        return SD_ERR_NO_CARD;
    }

    bool sdv2 = false;
    r1 = sd_command(CMD8, 0x1AA, 0x87);
    if (r1 == 0x01) {
        uint8_t echo[4];
        for (int i = 0; i < 4; i++) echo[i] = sd_xfer(0xFF);
        printf("sd: CMD8 r1=0x%02x echo=%02x%02x%02x%02x\n", r1, echo[0], echo[1], echo[2], echo[3]);
        if (echo[2] == 0x01 && echo[3] == 0xAA) sdv2 = true;
    } else {
        printf("sd: CMD8 r1=0x%02x (SD v1/MMC path)\n", r1);
    }
    // Any other r1 (illegal-command 0x05, etc.) means SD v1 or MMC; sdv2
    // stays false and we fall back to the older ACMD41/CMD16 path below.

    absolute_time_t deadline = make_timeout_time_ms(1000);
    int attempt = 0;
    do {
        sd_command(CMD55, 0, 0xFF);
        r1 = sd_command(ACMD41, sdv2 ? (1UL << 30) : 0, 0xFF);
        attempt++;
        if (r1 == 0x00) break;
        sleep_ms(10);
    } while (!time_reached(deadline));
    printf("sd: ACMD41 r1=0x%02x (sdv2=%d, %d attempts)\n", r1, sdv2, attempt);
    if (r1 != 0x00) {
        cs_high();
        return SD_ERR_TIMEOUT;
    }

    if (sdv2) {
        r1 = sd_command(CMD58, 0, 0xFF);
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) ocr[i] = sd_xfer(0xFF);
        printf("sd: CMD58 r1=0x%02x ocr=%02x%02x%02x%02x\n", r1, ocr[0], ocr[1], ocr[2], ocr[3]);
        if (r1 == 0x00) s_block_addressed = (ocr[0] & 0x40) != 0; // CCS bit
    } else {
        r1 = sd_command(CMD16, SD_BLOCK_SIZE, 0xFF); // SDSC/MMC: fix 512B blocks
        printf("sd: CMD16 r1=0x%02x\n", r1);
    }

    cs_high();
    sd_xfer(0xFF);
    spi_set_baudrate(SD_SPI_PORT, SD_FULL_HZ);

    s_initialized = true;
    return SD_OK;
}

bool sd_is_initialized(void) { return s_initialized; }
bool sd_is_block_addressed(void) { return s_block_addressed; }

sd_status_t sd_read_block(uint32_t lba, uint8_t *buf512) {
    if (!s_initialized) return SD_ERR_NO_CARD;
    uint32_t addr = s_block_addressed ? lba : lba * SD_BLOCK_SIZE;

    cs_low();
    uint8_t r1 = sd_command(CMD17, addr, 0xFF);
    if (r1 != 0x00) {
        cs_high();
        return SD_ERR_CMD;
    }

    absolute_time_t deadline = make_timeout_time_ms(200);
    uint8_t token;
    do {
        token = sd_xfer(0xFF);
    } while (token == 0xFF && !time_reached(deadline));
    if (token != 0xFE) {
        cs_high();
        return SD_ERR_TIMEOUT;
    }

    for (int i = 0; i < SD_BLOCK_SIZE; i++) buf512[i] = sd_xfer(0xFF);
    sd_xfer(0xFF);
    sd_xfer(0xFF); // discard CRC16

    cs_high();
    sd_xfer(0xFF);
    return SD_OK;
}

sd_status_t sd_write_block(uint32_t lba, const uint8_t *buf512) {
    if (!s_initialized) return SD_ERR_NO_CARD;
    uint32_t addr = s_block_addressed ? lba : lba * SD_BLOCK_SIZE;

    cs_low();
    uint8_t r1 = sd_command(CMD24, addr, 0xFF);
    if (r1 != 0x00) {
        cs_high();
        return SD_ERR_CMD;
    }

    sd_xfer(0xFE); // data start token
    for (int i = 0; i < SD_BLOCK_SIZE; i++) sd_xfer(buf512[i]);
    sd_xfer(0xFF);
    sd_xfer(0xFF); // dummy CRC16 (CRC checking is off by default in SPI mode)

    uint8_t resp = sd_xfer(0xFF);
    if ((resp & 0x1F) != 0x05) { // data response token: 0bxxx00101 = accepted
        cs_high();
        return SD_ERR_IO;
    }

    if (!sd_wait_ready(500)) { // card busy programming flash
        cs_high();
        return SD_ERR_TIMEOUT;
    }

    cs_high();
    sd_xfer(0xFF);
    return SD_OK;
}
