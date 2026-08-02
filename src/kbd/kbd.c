#include "kbd.h"

#include <stdbool.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"

#define KBD_I2C     i2c1
#define KBD_PIN_SDA 6
#define KBD_PIN_SCL 7
#define KBD_HZ      10000 // controller is fixed-speed; must not run the bus faster
#define KBD_ADDR    0x1F

#define KBD_REG_FIFO      0x09
#define KBD_REG_BACKLIGHT 0x0A
#define KBD_REG_BATTERY   0x0B

#define KBD_STATE_PRESS   1
#define KBD_STATE_RELEASE 3

static bool s_ctrl_held = false;

void kbd_init(void) {
    i2c_init(KBD_I2C, KBD_HZ);
    gpio_set_function(KBD_PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(KBD_PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(KBD_PIN_SDA);
    gpio_pull_up(KBD_PIN_SCL);
}

// Writes the register pointer, then reads back its 2-byte reply
// (byte0=state, byte1=payload), as the controller expects.
static int kbd_read_reg16(uint8_t reg, uint8_t *state, uint8_t *payload) {
    int ret = i2c_write_timeout_us(KBD_I2C, KBD_ADDR, &reg, 1, false, 500000);
    if (ret == PICO_ERROR_GENERIC || ret == PICO_ERROR_TIMEOUT) return -1;

    sleep_ms(16); // controller needs time to prepare its reply after the pointer write

    uint8_t buf[2];
    ret = i2c_read_timeout_us(KBD_I2C, KBD_ADDR, buf, 2, false, 500000);
    if (ret == PICO_ERROR_GENERIC || ret == PICO_ERROR_TIMEOUT) return -1;

    *state = buf[0];
    *payload = buf[1];
    return 0;
}

int kbd_poll_raw(uint8_t *state, uint8_t *code) {
    return kbd_read_reg16(KBD_REG_FIFO, state, code);
}

int kbd_decode(uint8_t state, uint8_t code) {
    if (state == 0 && code == 0) return -1; // nothing pending

    if (code == KBD_KEY_CTRL) {
        if (state == KBD_STATE_PRESS) s_ctrl_held = true;
        else if (state == KBD_STATE_RELEASE) s_ctrl_held = false;
        return -1;
    }

    if (state != KBD_STATE_PRESS) return -1;

    int c = code;
    if (c == KBD_KEY_ENTER_RAW) c = '\r'; // controller sends 0x0A, BIOS expects 0x0D
    if (c == KBD_KEY_ESC_RAW) c = 0x1B;    // controller sends 0xB1, BIOS expects 0x1B
    if (s_ctrl_held && c >= 'a' && c <= 'z') c = c - 'a' + 1;
    return c;
}

int kbd_poll(void) {
    uint8_t state, code;
    if (kbd_poll_raw(&state, &code) != 0) return -1;
    return kbd_decode(state, code);
}

int kbd_read_battery(void) {
    uint8_t state, payload;
    if (kbd_read_reg16(KBD_REG_BATTERY, &state, &payload) != 0) return -1;
    return (state << 8) | payload;
}

int kbd_set_backlight(uint8_t value) {
    uint8_t msg[2] = { KBD_REG_BACKLIGHT | 0x80, value };
    int ret = i2c_write_timeout_us(KBD_I2C, KBD_ADDR, msg, 2, false, 500000);
    if (ret == PICO_ERROR_GENERIC || ret == PICO_ERROR_TIMEOUT) return -1;

    sleep_ms(16);
    uint8_t buf[2];
    ret = i2c_read_timeout_us(KBD_I2C, KBD_ADDR, buf, 2, false, 500000);
    if (ret == PICO_ERROR_GENERIC || ret == PICO_ERROR_TIMEOUT) return -1;
    return 0;
}
