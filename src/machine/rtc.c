#include "rtc.h"

#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "pico/aon_timer.h"
#include "pico/time.h"

#include "nvram.h"

// Register offsets within the slot (see BIOS.inc's RTC_* equates and
// 6502-DEV's RTCCard.h register map -- this must match both).
#define REG_SEC       0x00
#define REG_MIN       0x01
#define REG_HOUR      0x02
#define REG_DOW       0x03
#define REG_DATE      0x04
#define REG_MONTH     0x05
#define REG_YEAR      0x06
#define REG_CENTURY   0x07
#define REG_ALARM_SEC 0x08
#define REG_ALARM_MIN 0x09
#define REG_ALARM_HR  0x0A
#define REG_ALARM_DAY 0x0B
#define REG_WD_L      0x0C
#define REG_WD_H      0x0D
#define REG_CTRL_A    0x0E
#define REG_CTRL_B    0x0F
#define REG_RAM_ADDR  0x10
#define REG_RAM_DATA  0x13

// The month register carries three control bits above the month value
// (BIOS.inc RTC_MON_*). EOSC is the oscillator stop bit: 0 = running, which
// is the sense the DS1511Y datasheet and the BIOS both use (ProbeRTC clears
// it deliberately and comments that it is leaving the oscillator running).
// The Teensy card reads that bit the other way round; this port follows the
// BIOS, since that is the ROM actually being run.
#define MON_MASK 0x1F
#define MON_BB32 0x20
#define MON_E32K 0x40
#define MON_EOSC 0x80

// Control A: flags in the low nibble, cleared by reading the register.
#define CTRLA_IRQF 0x01
#define CTRLA_WDF  0x02
#define CTRLA_KSF  0x04
#define CTRLA_TDF  0x08

// Control B.
#define CTRLB_WDS 0x01 // watchdog steers to reset rather than interrupt
#define CTRLB_WDE 0x02 // watchdog enable
#define CTRLB_KIE 0x04 // kickstart interrupt enable
#define CTRLB_TIE 0x08 // time-of-day alarm interrupt enable
#define CTRLB_BME 0x20 // NVRAM burst mode (address auto-increment)
#define CTRLB_TE  0x80 // transfer enable (see the freeze/thaw notes below)

// How often the hardware clock is re-read, in emulated CPU ticks. The card
// only ever reports whole seconds, so this just has to be comfortably finer
// than one second at any plausible emulated clock rate; ~1ms at 1MHz.
#define RTC_TICKS_PER_REFRESH 1024

// User-facing registers: what the CPU reads. While transfer-enable (TE) is
// set these track the counters once a second; with TE clear they hold still,
// which is how the BIOS reads a consistent time across several registers and
// how it stages a whole time/date before committing it (Kernal.asm
// RtcFreeze/RtcThaw).
static uint8_t user_sec, user_min, user_hour, user_dow, user_date, user_month, user_year, user_century;
static uint8_t month_control;

// The counters themselves, mirrored from the always-on timer.
static uint8_t int_sec, int_min, int_hour, int_dow, int_date, int_month, int_year, int_century;

static uint8_t alarm_sec, alarm_min, alarm_hour, alarm_daydate;
static uint8_t watchdog_l, watchdog_h;
static uint64_t watchdog_deadline_us;
static uint8_t control_a, control_b;
static uint8_t ram_address;

static bool user_write_pending; // user registers written while frozen
static bool last_te;
static uint16_t refresh_counter;

static uint8_t bin_to_bcd(uint8_t bin) {
    return (uint8_t) (((bin / 10) << 4) | (bin % 10));
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (uint8_t) (((bcd >> 4) * 10) + (bcd & 0x0F));
}

// Sakamoto's method; 0 = Sunday, matching struct tm's tm_wday. The DS1511Y
// numbers its day-of-week register 1-7 with 1 = Sunday.
static uint8_t weekday_of(uint16_t year, uint8_t month, uint8_t day) {
    static const uint8_t offset[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    uint16_t y = year - (month < 3 ? 1 : 0);
    return (uint8_t) ((y + y / 4 - y / 100 + y / 400 + offset[month - 1] + day) % 7);
}

static void copy_internal_to_user(void) {
    user_sec = int_sec;
    user_min = int_min;
    user_hour = int_hour;
    user_dow = int_dow;
    user_date = int_date;
    user_month = int_month;
    user_year = int_year;
    user_century = int_century;
}

static void raise_irq_if_enabled(uint8_t flag_mask, uint8_t enable_mask) {
    if (!(control_a & flag_mask)) return;
    if (!(control_b & enable_mask)) return;
    control_a |= CTRLA_IRQF; // flag only: the card's interrupt line isn't wired (see rtc.h)
}

static void check_alarm(void) {
    bool am1 = (alarm_sec & 0x80) != 0;
    bool am2 = (alarm_min & 0x80) != 0;
    bool am3 = (alarm_hour & 0x80) != 0;
    bool am4 = (alarm_daydate & 0x80) != 0;

    if (am1 && am2 && am3 && am4) return; // all masked = alarm disabled

    if (!am1 && int_sec != (alarm_sec & 0x7F)) return;
    if (!am2 && int_min != (alarm_min & 0x7F)) return;
    if (!am3 && int_hour != (alarm_hour & 0x7F)) return;
    if (!am4) {
        uint8_t value = alarm_daydate & 0x3F;
        // DY/DT selects whether the register holds a day of week or a date.
        if (alarm_daydate & 0x40) {
            if (int_dow != value) return;
        } else if (int_date != value) {
            return;
        }
    }

    control_a |= CTRLA_TDF;
    raise_irq_if_enabled(CTRLA_TDF, CTRLB_TIE);
}

// The watchdog registers hold a countdown in BCD: $00.$00 hundredths and
// tenths of a second in the low register, seconds and tens of seconds in the
// high one.
static uint16_t watchdog_centis(void) {
    uint16_t seconds = (uint16_t) (((watchdog_h >> 4) & 0x0F) * 10 + (watchdog_h & 0x0F));
    uint16_t centis = (uint16_t) (((watchdog_l >> 4) & 0x0F) * 10 + (watchdog_l & 0x0F));
    return (uint16_t) (seconds * 100 + centis);
}

static void reload_watchdog(void) {
    uint16_t centis = watchdog_centis();
    watchdog_deadline_us = centis ? time_us_64() + (uint64_t) centis * 10000u : 0;
}

static void step_watchdog(void) {
    if (!(control_b & CTRLB_WDE)) return;
    if (watchdog_deadline_us == 0 || time_us_64() < watchdog_deadline_us) return;

    watchdog_deadline_us = 0;
    control_a |= CTRLA_WDF;

    if (control_b & CTRLB_WDS) {
        // WDS=1 steers an expiry to the reset pin. Nothing here resets the
        // emulated machine (the Teensy card does the same), so the watchdog
        // is just disarmed and the flag left for the CPU to find.
        control_b &= (uint8_t) ~CTRLB_WDE;
    } else {
        raise_irq_if_enabled(CTRLA_WDF, CTRLB_WDE);
    }
}

// Pushes the staged user registers into the hardware clock. Called when
// transfer-enable goes back up after a write, which is exactly how the BIOS
// commits SETTIME/SETDATE (Kernal.asm RtcWriteTime/RtcWriteDate).
static void commit_user_to_clock(void) {
    uint8_t sec = bcd_to_bin(user_sec);
    uint8_t min = bcd_to_bin(user_min);
    uint8_t hour = bcd_to_bin(user_hour);
    uint8_t date = bcd_to_bin(user_date);
    uint8_t month = bcd_to_bin(user_month & MON_MASK);
    uint16_t year = (uint16_t) (bcd_to_bin(user_century) * 100 + bcd_to_bin(user_year));

    // A DS1511Y accepts whatever BCD it is given and simply counts on from
    // it; the always-on timer does not, so an impossible date is dropped
    // rather than handed to it. The registers keep the written values until
    // the next transfer overwrites them.
    if (sec > 59 || min > 59 || hour > 23) return;
    if (month < 1 || month > 12 || date < 1 || date > 31) return;

    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_sec = sec;
    t.tm_min = min;
    t.tm_hour = hour;
    t.tm_mday = date;
    t.tm_mon = month - 1;
    t.tm_year = (int) year - 1900;
    // The BIOS's SETDATE never writes the day-of-week register, so it is
    // derived here rather than trusted.
    t.tm_wday = weekday_of(year, month, date);
    t.tm_isdst = 0;

    aon_timer_set_time_calendar(&t);
}

// Any write to a user time/date register stages a commit. With TE already
// set there is no thaw coming to trigger it, and the next transfer would
// simply overwrite the write a moment later, so it is committed on the spot
// -- which is what the real part does, its user registers and counters being
// continuously coupled while TE is high.
static void mark_user_write(void) {
    user_write_pending = true;
    if (control_b & CTRLB_TE) {
        commit_user_to_clock();
        user_write_pending = false;
    }
}

static void refresh_from_clock(void) {
    step_watchdog();

    if (month_control & MON_EOSC) return; // oscillator stopped: counters hold

    struct tm now;
    if (!aon_timer_get_time_calendar(&now)) return;

    uint8_t sec = bin_to_bcd((uint8_t) (now.tm_sec > 59 ? 59 : now.tm_sec)); // clamp a leap second
    bool second_changed = (sec != int_sec);

    uint16_t year = (uint16_t) (now.tm_year + 1900);
    int_sec = sec;
    int_min = bin_to_bcd((uint8_t) now.tm_min);
    int_hour = bin_to_bcd((uint8_t) now.tm_hour);
    int_dow = (uint8_t) (now.tm_wday + 1); // tm: 0 = Sunday; DS1511Y: 1 = Sunday
    int_date = bin_to_bcd((uint8_t) now.tm_mday);
    int_month = bin_to_bcd((uint8_t) (now.tm_mon + 1));
    int_year = bin_to_bcd((uint8_t) (year % 100));
    int_century = bin_to_bcd((uint8_t) ((year / 100) % 100));

    if (second_changed) check_alarm();
    if (control_b & CTRLB_TE) copy_internal_to_user();
}

// Cold-boot time for a clock that has nothing to restore. Neither the RP2040
// RTC nor the RP2350 powman timer is battery-backed on the PicoCalc, so the
// time survives a reset but not a power cycle; starting at the moment this
// firmware was built at least puts DATE in the right decade before the user
// runs SETTIME/SETDATE.
static void build_timestamp(struct tm *t) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__; // "Mmm dd yyyy", day space-padded
    const char *c = __TIME__; // "hh:mm:ss"

    uint8_t month = 1;
    for (uint8_t i = 0; i < 12; i++) {
        if (strncmp(d, months + i * 3, 3) == 0) {
            month = (uint8_t) (i + 1);
            break;
        }
    }
    uint8_t day = (uint8_t) ((d[4] == ' ' ? 0 : (d[4] - '0') * 10) + (d[5] - '0'));
    uint16_t year = (uint16_t) ((d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0'));

    memset(t, 0, sizeof(*t));
    t->tm_sec = (c[6] - '0') * 10 + (c[7] - '0');
    t->tm_min = (c[3] - '0') * 10 + (c[4] - '0');
    t->tm_hour = (c[0] - '0') * 10 + (c[1] - '0');
    t->tm_mday = day;
    t->tm_mon = month - 1;
    t->tm_year = (int) year - 1900;
    t->tm_wday = weekday_of(year, month, day);
    t->tm_isdst = 0;
}

void rtc_card_init(void) {
    nvram_init();

    // Already running means a warm reset (the RUN pin, a watchdog reboot, a
    // fresh UF2): leave the clock alone so the time carries across it.
    if (!aon_timer_is_running()) {
        struct tm t;
        build_timestamp(&t);
        aon_timer_start_calendar(&t);
    }

    month_control = 0x00; // oscillator running, SQW and battery-backed SQW off
    ram_address = 0;
    rtc_card_reset();
    copy_internal_to_user(); // so the registers read a real time before the BIOS sets TE
}

void rtc_card_reset(void) {
    alarm_sec = alarm_min = alarm_hour = alarm_daydate = 0;
    watchdog_l = watchdog_h = 0;
    watchdog_deadline_us = 0;
    control_a = 0;
    control_b = 0;
    user_write_pending = false;
    last_te = false;
    refresh_counter = 0;

    refresh_from_clock();

    control_a |= CTRLA_KSF; // kickstart: power (or reset) has been applied
    raise_irq_if_enabled(CTRLA_KSF, CTRLB_KIE);
}

uint8_t rtc_card_read(uint16_t addr) {
    switch (addr & 0x1F) {
        case REG_SEC:     return user_sec;
        case REG_MIN:     return user_min;
        case REG_HOUR:    return user_hour;
        case REG_DOW:     return user_dow & 0x07;
        case REG_DATE:    return user_date;
        case REG_MONTH:   return user_month | month_control;
        case REG_YEAR:    return user_year;
        case REG_CENTURY: return user_century;
        case REG_ALARM_SEC: return alarm_sec;
        case REG_ALARM_MIN: return alarm_min;
        case REG_ALARM_HR:  return alarm_hour;
        case REG_ALARM_DAY: return alarm_daydate;
        case REG_WD_L:    return watchdog_l;
        case REG_WD_H:    return watchdog_h;
        case REG_CTRL_A: {
            // Reading Control A clears its four flag bits.
            uint8_t value = control_a;
            control_a &= 0xF0;
            return value;
        }
        case REG_CTRL_B:   return control_b;
        case REG_RAM_ADDR: return ram_address;
        case REG_RAM_DATA: {
            uint8_t value = nvram_read(ram_address);
            if (control_b & CTRLB_BME) ram_address++;
            return value;
        }
        default: return 0x00; // includes the two reserved registers $11/$12
    }
}

void rtc_card_write(uint16_t addr, uint8_t value) {
    switch (addr & 0x1F) {
        case REG_SEC:     user_sec = value; mark_user_write(); break;
        case REG_MIN:     user_min = value; mark_user_write(); break;
        case REG_HOUR:    user_hour = value; mark_user_write(); break;
        case REG_DOW:     user_dow = value & 0x07; mark_user_write(); break;
        case REG_DATE:    user_date = value; mark_user_write(); break;
        case REG_MONTH:
            user_month = value & MON_MASK;
            month_control = value & (MON_EOSC | MON_E32K | MON_BB32);
            mark_user_write();
            break;
        case REG_YEAR:    user_year = value; mark_user_write(); break;
        case REG_CENTURY: user_century = value; mark_user_write(); break;
        case REG_ALARM_SEC: alarm_sec = value; break;
        case REG_ALARM_MIN: alarm_min = value; break;
        case REG_ALARM_HR:  alarm_hour = value; break;
        case REG_ALARM_DAY: alarm_daydate = value; break;
        case REG_WD_L: watchdog_l = value; reload_watchdog(); break;
        case REG_WD_H: watchdog_h = value; reload_watchdog(); break;
        case REG_CTRL_A:
            // Writing a 1 to a flag bit clears it; the control bits above
            // are written normally.
            control_a = (uint8_t) ((value & 0xF0) | ((control_a & 0x0F) & ~(value & 0x0F)));
            break;
        case REG_CTRL_B: {
            bool te = (value & CTRLB_TE) != 0;
            control_b = value;
            raise_irq_if_enabled(CTRLA_KSF, CTRLB_KIE);
            if (value & CTRLB_WDE) reload_watchdog();

            // The rising edge of TE is the commit point of a staged write.
            if (te && !last_te && user_write_pending) {
                commit_user_to_clock();
                user_write_pending = false;
            }
            last_te = te;
            break;
        }
        case REG_RAM_ADDR: ram_address = value; break;
        case REG_RAM_DATA:
            nvram_write(ram_address, value);
            if (control_b & CTRLB_BME) ram_address++;
            break;
        default: break; // reserved registers $11/$12 ignore writes
    }
}

uint8_t rtc_card_tick(void) {
    if (++refresh_counter < RTC_TICKS_PER_REFRESH) return 0x00;
    refresh_counter = 0;

    refresh_from_clock();
    return 0x00;
}
