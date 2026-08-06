#include "kbd.h"

#include <stdbool.h>
#include <stddef.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"

// For the joystick bit layout. This driver already speaks the emulated
// machine's language rather than the controller's — kbd_decode() below is the
// ACE's keyboard encoder, not the PicoCalc's — and the stick is fed from the
// same key events, so it reports its state in the VIA card's bits directly.
#include "machine/gpio.h"

#define KBD_I2C     i2c1
#define KBD_PIN_SDA 6
#define KBD_PIN_SCL 7
#define KBD_HZ      10000 // controller is fixed-speed; must not run the bus faster
#define KBD_ADDR    0x1F

#define KBD_REG_FIFO      0x09
#define KBD_REG_BACKLIGHT 0x0A
#define KBD_REG_BATTERY   0x0B

// Which keys stand in for the joystick's eight switches. The four arrows are
// the obvious directions; Space is fire, with Z/X/C alongside it for the
// three remaining buttons, all within reach of the left hand while the right
// works the arrows.
static const struct {
    uint8_t code;
    uint8_t button;
} joystick_map[] = {
    { KBD_KEY_UP_RAW,    GPIO_JOY_UP },
    { KBD_KEY_DOWN_RAW,  GPIO_JOY_DOWN },
    { KBD_KEY_LEFT_RAW,  GPIO_JOY_LEFT },
    { KBD_KEY_RIGHT_RAW, GPIO_JOY_RIGHT },
    { ' ',               GPIO_JOY_A },
    { 'z',               GPIO_JOY_B },
    { 'x',               GPIO_JOY_X },
    { 'c',               GPIO_JOY_Y },
};

static bool s_ctrl_held = false;
static uint8_t s_joystick = 0;

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

bool kbd_wait_ready(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    do {
        uint8_t state, code;
        if (kbd_poll_raw(&state, &code) == 0) return true;
        sleep_ms(50);
    } while (!time_reached(deadline));
    return false;
}

// Follows the press/hold/release events of the joystick keys. Shift or Caps
// sends the letter keys up as capitals, which is still the same switch.
static void track_joystick(uint8_t state, uint8_t code) {
    uint8_t key = (code >= 'A' && code <= 'Z') ? (uint8_t) (code + ('a' - 'A')) : code;

    for (size_t i = 0; i < sizeof(joystick_map) / sizeof(joystick_map[0]); i++) {
        if (joystick_map[i].code != key) continue;
        if (state == KBD_STATE_RELEASE) s_joystick &= (uint8_t) ~joystick_map[i].button;
        else s_joystick |= joystick_map[i].button;
        return;
    }
}

uint8_t kbd_joystick_state(void) {
    return s_joystick;
}

// The control code Ctrl produces alongside a key, for the six non-letter
// combinations the reference encoder supports. Letters are handled by
// arithmetic below; everything not listed here loses the Ctrl and types
// itself, as it does on the reference encoder.
static int ctrl_symbol(uint8_t c) {
    switch (c) {
        case '2':  return 0x00; // NUL
        case '6':  return 0x1E; // RS
        case '-':  return 0x1F; // US
        case '[':  return 0x1B; // ESC
        case '\\': return 0x1C; // FS
        case ']':  return 0x1D; // GS
        default:   return -1;
    }
}

int kbd_decode(uint8_t state, uint8_t code) {
    if (state == 0 && code == 0) return -1; // nothing pending

    track_joystick(state, code);

    if (code == KBD_KEY_MOD_CTRL || code == KBD_KEY_MOD_CTRL_ALT) {
        if (state == KBD_STATE_PRESS) s_ctrl_held = true;
        else if (state == KBD_STATE_RELEASE) s_ctrl_held = false;
        return -1;
    }
    // Shift and Sym are applied by the controller before the code reaches us,
    // and Alt has nothing to apply, so all three are simply not keys as far as
    // the machine is concerned.
    if (code == KBD_KEY_MOD_SHL || code == KBD_KEY_MOD_SHR ||
        code == KBD_KEY_MOD_SYM || code == KBD_KEY_MOD_ALT) {
        return -1;
    }

    if (state != KBD_STATE_PRESS) return -1;

    // The controller's own codes for keys that are not ASCII, mapped to what
    // the machine's encoder puts on the port for the same key.
    int c;
    switch (code) {
        case KBD_KEY_ENTER_RAW: c = 0x0D; break; // CR, not the LF sent
        case KBD_KEY_ESC_RAW:   c = 0x1B; break;
        case KBD_KEY_LEFT_RAW:  c = 0x1C; break;
        case KBD_KEY_RIGHT_RAW: c = 0x1D; break;
        case KBD_KEY_UP_RAW:    c = 0x1E; break;
        case KBD_KEY_DOWN_RAW:  c = 0x1F; break;
        case KBD_KEY_INSERT:    c = 0x1A; break;
        case KBD_KEY_DEL:       c = 0x7F; break;
        default:                c = code; break;
    }

    // Anything still outside ASCII is a key this machine's keyboard does not
    // have — the function row, Caps Lock, Home/End/PgUp/PgDn, Break. The
    // controller sees the press and passes no character on.
    if (c > 0x7F) return -1;

    // Everything is in capitals. The ACE's encoder hands over $41-$5A for a
    // letter whether or not Shift is down, so the lower case the PicoCalc's
    // controller sends for an unshifted letter is folded up here — there is no
    // lower case from this keyboard on the real machine.
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';

    if (s_ctrl_held) {
        if (c >= 'A' && c <= 'Z') return c - 'A' + 1; // Ctrl-A = 1 .. Ctrl-Z = 26
        int sym = ctrl_symbol((uint8_t) c);
        if (sym >= 0) return sym;
    }

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
