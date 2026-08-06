add_executable(6502-picocalc
    main.c
    lcd/lcd.c
    kbd/kbd.c
    launcher/launcher.c
    sd/sd.c
    fatfs/ff.c
    fatfs/diskio.c
    cpu/vrEmu6502/vrEmu6502.c
    machine/machine.c
    machine/serial.c
    machine/gpio.c
    machine/video.c
    machine/video_render.c
    machine/sound.c
    machine/sound_synth.c
    machine/storage.c
    machine/rtc.c
    machine/nvram.c
    machine/settings.c
    machine/ram_bank.c
    machine/media.c
    rom/bios_rom.c
)

target_include_directories(6502-picocalc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} fatfs cpu rom)

pico_set_program_name(6502-picocalc "6502-picocalc")
pico_set_program_version(6502-picocalc "0.1.0")

# The emulated 6502's own serial console (6551 ACIA, see src/machine/serial.c)
# is bridged onto this same USB CDC stdio stream (PLAN.md Phase 3).
pico_enable_stdio_usb(6502-picocalc 1)
pico_enable_stdio_uart(6502-picocalc 0)

target_compile_definitions(6502-picocalc PRIVATE
    PICOCALC_BOARD="${PICO_BOARD}"
    # USB CDC has no history buffer: without this, boot-time printf output is
    # lost whenever the host terminal is opened after power-on/reset. Blocks
    # stdio_init_all() up to this long for the host to be USB-configured (see
    # PICO_STDIO_USB_CONNECTION_WITHOUT_DTR below) -- which never happens at
    # all on a standalone boot with no host attached, so this is a guaranteed
    # tax on every power-on, not just a worst case. Kept short rather than 0
    # so a terminal already open at flash/reset time still catches the early
    # banner (PLAN.md Phase 11 perf pass — see repo memory boot-time notes).
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=1000
    # By default the SDK only writes CDC output while the host asserts DTR,
    # which most terminal apps (CoolTerm, screen, etc.) don't do unless you
    # dig up a "DTR on connect" option — with it off, every printf is
    # silently dropped forever even though the port shows as open. Treat
    # "USB configured" as connected instead, so plain port-open is enough.
    PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
)

target_link_libraries(6502-picocalc
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_i2c
    hardware_pwm
    hardware_uart
    # RTC card (machine/rtc.c): pico_aon_timer is the one clock API that
    # works on both targets — the RP2040's RTC block and the RP2350's powman
    # timer — and pico_flash carries its NVRAM (machine/nvram.c).
    pico_aon_timer
    pico_flash
    # Boot-time overclock (main.c, PLAN.md Phase 11): hardware_clocks for
    # set_sys_clock_khz(), hardware_vreg for the voltage bump that goes with
    # running above the 133MHz rated-safe default.
    hardware_clocks
    hardware_vreg
)

pico_add_extra_outputs(6502-picocalc)
