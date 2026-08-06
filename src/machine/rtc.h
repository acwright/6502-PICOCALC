#ifndef MACHINE_RTC_H
#define MACHINE_RTC_H

#include <stdint.h>

// Emulated DS1511Y real-time clock, IO slot 2 ($8800-$8813, see PLAN.md
// Phase 8 and BIOS.inc's RTC_* equates). Register model ported from
// 6502-DEV's RTCCard (DB Emulator/lib/6502/IO/RTCCard.*), which the
// TypeScript emulator's RTC.ts matches.
//
// Where the Teensy card counts CPU cycles to advance its own copy of the
// time, this one keeps the Pico's always-on timer (the RP2040 RTC / RP2350
// powman timer, via pico_aon_timer) as the single source of truth and just
// presents it in the DS1511Y's BCD registers. That way the clock stays right
// no matter how fast or slowly the emulated 6502 is actually being ticked --
// which matters here, since nothing yet paces the tick loop to a real clock
// rate (PLAN.md Phase 11).
//
// (The rtc_card_ prefix, rather than the rtc_ the other cards' naming would
// suggest, keeps these clear of the SDK's own rtc_init() and friends in
// hardware_rtc, which pico_aon_timer pulls in on RP2040.)
//
// SETTIME/SETDATE therefore set the Pico's clock, and TIME/DATE read it back.
// The 256 bytes of battery-backed NVRAM behind registers $10/$13 live in
// flash (see src/machine/nvram.c), so NVRAM -- and the BIOS's ProbeRTC
// read-back probe that decides HW_RTC -- survive a power cycle.
void rtc_card_init(void);

// Clears the alarm/watchdog/control state and raises the kickstart flag, as
// the real part does when power is applied. NVRAM and the running clock are
// deliberately left alone: on the real card both are battery-backed, and a
// reset that wiped them would take the persistence with it.
void rtc_card_reset(void);

// addr is slot-local; only the low 5 bits are decoded (the DS1511Y's 32
// registers mirror every $20 bytes across the slot).
uint8_t rtc_card_read(uint16_t addr);
void rtc_card_write(uint16_t addr, uint8_t value);

// Call once per emulated CPU clock tick (mirrors serial_tick()/video_tick()).
// Re-reads the hardware clock every RTC_TICKS_PER_REFRESH ticks and, while
// transfer-enable is set, republishes it in the user-facing registers.
// Always returns 0x00: on real hardware the card's interrupt reaches the CPU
// as an NMI through the AB Controller's SQW input, which this port does not
// wire up (nor does the Teensy firmware), so alarm and watchdog events set
// their Control A flags but pull no line.
uint8_t rtc_card_tick(void);

#endif
