6502-PICOCALC — Build Plan
==========================

Port the A.C. Wright **AC6502** family emulator to the **ClockworkPi PicoCalc**,
as a complete replacement for the device firmware. The project is a Raspberry Pi
Pico SDK (C/C++) application that builds to a single **`.uf2`** and takes full
control of the device: LCD, keyboard, speaker, SD card, and serial.

The result boots exactly like the real machine and the desktop/web emulator: the
BIOS ROM probes its I/O slots, shows the splash, counts down, and drops into
BASIC — but on real PicoCalc hardware, in your hand.

---

## 1. Strategy

There are three existing implementations of this machine, in decreasing distance
from the target:

| Source | What it gives us |
|--------|------------------|
| [6502-BIOS](https://github.com/acwright/6502-BIOS) (`BIOS.bin`) | The 32 KB ROM the emulator runs. **Ships unchanged, embedded in flash.** The whole point of the port is to run this byte-for-byte. |
| [6502-EMULATOR](https://github.com/acwright/6502-EMULATOR) (TypeScript) | The reference semantics for every I/O card — VDP, SID, VIA, ACIA, RTC, CF storage, RAM banks. Behaviour to match; not code to port. |
| [6502-DEV](https://github.com/acwright/6502-DEV) Teensy firmware (C++) | **The closest analog and the primary source to port.** It already runs this emulator on a microcontroller. |

The Teensy design is split across **two** boards:

- **DB Emulator** (Teensy 4.1) — `vrEmu6502` CPU + RAM/ROM/Cart + eight I/O cards
  (2× RAM bank, RTC, Storage/SD, Serial, GPIO/VIA, Sound/SID, Video/TMS9918).
  Video and Sound cards only *accumulate* register/VRAM writes and stream them
  as "AV packets" to the second board.
- **DOB Display** (Teensy 4.0) — receives AV packets and does the actual
  **TMS9918 rendering to an ILI9341 LCD** and **SID synthesis to a PWM speaker**.

**The PicoCalc port collapses both boards onto one Pico.** The AV-packet link
disappears; the VDP renderer and SID synthesiser read the emulator's own card
state directly. Everything the two Teensys did together, one RP2040/RP2350 does
alone.

### Core split

- **Core 0** — the machine: `vrEmu6502` tick loop, memory map, and all I/O card
  `tick()`/`read()`/`write()` logic. This is the hard-real-time 6502.
- **Core 1** — rendering: rasterise the TMS9918 framebuffer and DMA it to the LCD
  over SPI1 at ~30–60 Hz, and synthesise SID audio into the PWM output. Reads a
  shared snapshot of VRAM + VDP/SID registers produced by Core 0.

USB CDC serial, the I2C keyboard poll, and SD access are serviced from Core 0's
outer loop (outside the tight tick batch), exactly as the Teensy `loop()` does.

### Language / build

C/C++ on the **Raspberry Pi Pico SDK** with CMake, per the PicoCalc wiki
(*Setting Up the Pico SDK*). `vrEmu6502` is C and drops in directly. The Teensy
I/O card classes are plain C++ and port with their Arduino dependencies
(`SD`, `Serial`, `IntervalTimer`, `EEPROM`) swapped for SDK equivalents
(`FatFs`/`no-OS-FatFS`, `stdio`/`tusb`, `hardware/timer`, flash).

Build targets **both** `pico` (RP2040) and `pico2` (RP2350) from one tree via a
board flag; the only differences are RAM budget (§4) and clock.

---

## 2. Hardware mapping (PicoCalc → emulated machine)

Pin assignments verified from `clockworkpi/PicoCalc` (`Code/` configs). All are
Pico GPIO numbers.

| Emulated part | PicoCalc hardware | Pins |
|---|---|---|
| **TMS9918 video** | ST7365P / ILI9488-compatible LCD, 320×320, 16 bpp | SPI1: SCK=10, MOSI=11, MISO=12, CS=13, DC=14, RST=15 (25 MHz, up to 50) |
| **Keyboard + joystick** | STM32 keyboard controller over I²C | I2C1: SDA=6, SCL=7; addr `0x1F`; read reg `0x09` |
| **SID audio** | PWM speaker (stereo) | AUDIO_L=26, AUDIO_R=27, `GPIO_FUNC_PWM` (one PWM slice) |
| **CompactFlash storage** | microSD card | SPI0: SCLK=18, MOSI=19, MISO=16, CS=17, DET=22 |
| **6551 serial console** | USB-C CDC **and** the side-header UART pins, both live at once | USB; UART0 TX=0, RX=1 (side header), at the ACIA's own baud/format |
| **DS1511Y RTC** | RP2040/RP2350 on-chip RTC/AON timer (`pico_aon_timer` covers both); NVRAM in the top flash sector | internal |
| Backlight / battery | Read/written through the I²C keyboard controller | I2C1 registers |
| Status LED | Pico onboard LED | GP25 |

The LCD is **320×320**. The TMS9918 active area is **256×192** (text mode
40×24 → 240×192). The renderer centres/letterboxes the VDP canvas on the panel
(integer scale — 256×192 fits with a border; optionally 1.25× to fill height).
Colour is converted from the 16-entry TMS palette to RGB565 once at init.

---

## 3. Emulated memory map (unchanged from the family)

```
$0000–$7FFF  RAM (32 KB)
$8000–$9FFF  I/O space — eight $400 slots
             slot0 $8000  RAM bank low  (IO 1, 256 KB paged)
             slot1 $8400  RAM bank high (IO 2, 256 KB paged)
             slot2 $8800  RTC (DS1511Y)
             slot3 $8C00  Storage (CF/IDE → SD image)
             slot4 $9000  Serial (6551 ACIA)
             slot5 $9400  GPIO/VIA (65C22, keyboard + joystick)
             slot6 $9800  Sound (SID)
             slot7 $9C00  Video (TMS9918)
$A000–$FFFF  ROM (BIOS) — cart overrides $C000–$FFFF when loaded
$FFFA–$FFFF  vectors
```

`BIOS.bin` is 32 KB (`$8000–$FFFF`); the I/O window at `$8000–$9FFF` is decoded
in front of ROM, exactly as the Teensy `read`/`write` fast-lookup does (high-byte
region table, then slot = `(addr-$8000)>>10`).

---

## 4. Resource budget (the one real constraint)

| | RP2040 (Pico 1) | RP2350 (Pico 2) |
|---|---|---|
| SRAM | 264 KB | 520 KB |
| System RAM (32 KB) | ✓ | ✓ |
| VRAM (16 KB) | ✓ | ✓ |
| LCD framebuffer 256×192×1B palette (48 KB) + line buffers | ✓ | ✓ |
| **RAM expansion IO 1+2 (2 × 256 KB = 512 KB)** | ✗ can't fit | one 256 KB bank fits, both are tight |

**This resolves the first open question.** Full dual-256 KB expansion does not fit
in RP2040 SRAM alongside the framebuffer and audio buffers. The plan therefore:

- Makes expansion-RAM size a **compile-time budget** per target.
- RP2040 default: **IO 1 present at a reduced size** (e.g. 64–128 KB) or absent;
  BIOS `HW_PRESENT` probing means the machine boots fine either way and BASIC
  (which the note says uses only BANK 0) is unaffected.
- RP2350 default: **IO 1 full 256 KB** (BANK 0 as on real hardware); IO 2
  optional. Boards with external PSRAM can host both in full — a later stretch.

Because the BIOS probes each slot and degrades gracefully, *any* of these choices
produces a correct machine; only the amount of paged RAM differs.

---

## 5. Phases

Each phase is independently flashable and leaves the device in a working state.
Phases 0–5 reach a fully interactive BASIC prompt on the physical device; the
rest restore the remaining cards and polish.

### Phase 0 — Toolchain, skeleton, first UF2
**Goal:** a build that boots on the PicoCalc and proves the pipeline.
- Pico SDK + CMake project; `pico`/`pico2` board selection flag.
- USB CDC stdio "hello"; onboard LED heartbeat.
- CI/build script producing `6502-picocalc.uf2` for both targets.
- **Done when:** UF2 flashes, enumerates as USB serial, LED blinks.

### Phase 1 — LCD driver
**Goal:** drive the ST7365P/ILI9488 panel.
- SPI1 init + reset sequence (port `pico_lcd_init` from the PicoCalc examples).
- 8bpp-palette framebuffer → RGB565 push, ideally DMA + double line buffer.
- Backlight on (via I²C keyboard MCU; may need Phase 2 first — otherwise default on).
- **Done when:** test pattern and text render full-screen at a stable frame rate.

### Phase 2 — Keyboard + SD storage
**Goal:** the two non-emulation peripherals.
- I²C1 keyboard driver (addr `0x1F`, poll reg `0x09`), decode to key events;
  battery + backlight registers.
- SD card on SPI0 with a FatFs port; mount, list, read a file.
- **Done when:** keypresses print over USB serial; a file on SD is read and dumped.

### Phase 3 — CPU + memory + serial boot (**first machine**)
**Goal:** the 6502 runs the real BIOS, console over USB serial.
- Port `vrEmu6502` and the `CPU` wrapper; `read`/`write` with the region/slot
  fast-lookup; RAM/ROM/Cart.
- Embed `BIOS.bin` in flash (or load `ROMs/BIOS.bin` from SD if present, like the
  Teensy).
- Boot with the **video slot reporting absent** so the BIOS auto-routes the
  console to the 6551 slot, bridged to USB CDC — this mirrors the emulator's
  `--console serial` and gives interactive BASIC *before* any VDP work.
- Tick loop batched like the Teensy `loop()` (N ticks per outer iteration);
  drive IRQ/NMI from the aggregated card `tick()` return.
- **Done when:** over USB serial you see the banner, `PRINT 2+2` → `4`, ESC → Monitor.

### Phase 4 — TMS9918 VDP on the LCD
**Goal:** boot to BASIC on the physical screen.
- Port `VideoCard` (register/VRAM/address latch, vblank IRQ timing) to Core 0.
- Port the DOB renderer (Graphics I/II, Text 40×24, Multicolor, sprites) to
  Core 1, targeting the 256×192 canvas centred on 320×320.
- Double-buffer VRAM/register snapshot handed Core 0 → Core 1 each frame.
- Flip BIOS video probe to **present**; console auto-selects video.
- **Done when:** the splash and BASIC render on the LCD; scrolling works.

### Phase 5 — Keyboard into the machine (**fully interactive on-device**)
**Goal:** type BASIC on the PicoCalc itself.
- Map decoded keys into the BIOS keyboard ring buffer via the GPIO/VIA card's
  keyboard-encoder path (port `GPIOKeyboardEncoderAttachment`), so the BIOS IRQ
  keyboard flow is exercised as on hardware.
- Handle modifiers, control codes, Enter=CR, ESC.
- **Done when:** the device alone (no host) boots, and you can write and `RUN` a
  BASIC program from the built-in keyboard.

### Phase 6 — SID audio
**Goal:** sound.
- Port `SoundCard` register model (Core 0) + the DOB SID synth (DDS + ADSR +
  noise LFSR, Core 1) at 44.1 kHz into PWM on AUDIO_L/R.
- `Beep`, `SOUND`, `VOL` produce tones.
- **Done when:** boot beep plays; a `SOUND` command is audible on the speaker.

### Phase 7 — CompactFlash storage (LOAD/SAVE/DIR)
**Goal:** file storage for BASIC programs. **Resolves the CF open question.**
- Port `StorageCard` (true-IDE/LBA register model) backed by a **CF disk image
  file on the SD card** (`CF.IMG`, up to 256 × 1 MB banks). The image is
  created one bank long and grows a bank at a time as higher LBAs are written;
  sectors past its end read back blank, so all 256 disks are addressable
  without pre-filling 256 MB over SPI (which the Teensy does, and which would
  take minutes here). An absent/unopenable image reads status `$00`, so the
  BIOS `StInit` probe times out and leaves `HW_CF` clear.
- BASIC `LOAD "x"`, `SAVE`, `DIR`, `DEL`, `DISK n`, `BLOAD`/`BSAVE` operate on it.
- Optionally build/seed images with [`cffs`](https://github.com/acwright/cffs).
- **Done when:** `SAVE"T"` then power-cycle then `LOAD"T"`/`RUN` round-trips.

### Phase 8 — RTC, VIA/joystick, real serial
**Goal:** the remaining probed cards.
- **RTC** (`RTCCard`) backed by the on-chip always-on timer, which is the
  *source of truth* rather than the cycle-counted copy the Teensy card keeps —
  so `TIME`/`DATE` stay right whatever rate the tick loop happens to be running
  at (nothing paces it to a real clock until Phase 11). `SETTIME`/`SETDATE`
  commit through the DS1511Y's own transfer-enable freeze/thaw and set the
  hardware clock. NVRAM is the last sector of the Pico's flash, written back a
  couple of seconds after the machine stops writing to it (Core 1 is parked for
  the erase, so the write is batched rather than done per store). Neither part's
  clock is battery-backed on this board, so a cold boot starts from the firmware
  build timestamp and a reset carries the running time across.
  Answers the RTC open question: **yes, via on-chip clock.**
- **VIA/joystick** — the arrows plus Space/Z/X/C drive `JOY(1)` on Port B,
  through the port itself rather than a side channel: the keys set the port's
  input bits, and `ReadJoystick1`'s "disable the encoders, read the raw port"
  path (as a C64 reads a CIA) sees them. Port A carries `JOY(2)` the same way,
  with nothing wired to it — this hardware has no second input device and no
  USB host port for a gamepad, so it reads idle ($FF).
- **6551 serial** — the ACIA also drives UART0 on the side header (GP0 TX,
  GP1 RX), at whatever baud and framing the control register asks for (the BIOS
  sets 19200 8-N-1), with the USB CDC link live in parallel. Debug `printf`
  stays on USB only, so the UART carries nothing but the machine's own bytes and
  XMODEM comes through clean. Transmit is queued and paced by the line rate,
  with TDRE going low when the queue fills — the same flow control real hardware
  gives, so no byte is ever dropped.
- **Done when:** `TIME`/`DATE` work; `JOY()` reads the keys; a terminal on the
  UART pins talks to the ACIA.

### Phase 9 — Expansion RAM banks (IO 1 / IO 2)
**Goal:** paged RAM per the target budget (§4). **Resolves the IO 1/2 question.**
- Port `RAMBank` (256 banks × 1 KB window at `$8000`, control reg at `$83FF`).
- Size by target flag; probe reports only what is instantiated.
- **Done when:** `BANK n` + `POKE`/`PEEK` page correctly up to the built size;
  `MEM` reports it.

### Phase 10 — Loader / launcher UI + XMODEM
**Goal:** pick and run software, like the emulator's file loading.
- On-device menu (rendered on the LCD, driven by the keyboard) to browse SD for
  **ROMs**, **Carts**, and **Programs** (`.prg`/`.bas`), mirroring the Teensy
  `ROMs/`, `Carts/`, `Programs/` folders and the emulator's load model.
- Program image load to `$0800` with the `VARTAB`/`ARYTAB`/`STREND` fixup, and
  cart mapping at `$C000` with ROM override — exactly as `Machine`/`ProgramImage`
  and the Teensy `loadCart`/`loadProgram` do.
- XMODEM `LOAD`/`SAVE` (no filename) over the serial console for transfers.
- **Done when:** select a cart/program from the menu and it runs; XMODEM upload
  from a terminal loads a program.

### Phase 11 — Polish, performance, release
**Goal:** ship.
- Backlight/brightness + battery indicator via the keyboard MCU; power/sleep.
- Persist settings (clock speed, expansion size, last disk) to flash.
- Performance pass: overclock as needed; confirm sustained ~1–2 MHz 6502 with
  video+audio; tune the tick-batch size and frame cadence.
- Dual-target release UF2s (Pico 1 and Pico 2); README + flashing instructions;
  a fresh reset/power-cycle model matching the emulator's Reset vs Power-Cycle.
- **Done when:** both UF2s pass an on-device smoke test (boot → BASIC → LOAD →
  RUN → sound → save) and are tagged for release.

---

## 6. Open questions — resolutions carried into the plan

- **IO 1 & 2 (expansion RAM):** Size is a per-target compile budget (§4, Phase 9).
  RP2040 ships reduced/optional; RP2350 ships BANK 0 in full; PSRAM boards can do
  both. Graceful probing makes every choice boot correctly.
- **Cartridges / CF / DOS:** CF is emulated against a disk-image file on the SD
  card (Phase 7), so `LOAD`/`SAVE`/`DIR` and `BLOAD`/`BSAVE` all work. Cartridges
  load from SD via the launcher and map at `$C000` (Phase 10). XMODEM over serial
  remains as the terminal path (Phase 10).
- **RTC:** Supported via the Pico's on-chip RTC/AON timer, with NVRAM in the top
  sector of flash (Phase 8). The one thing the emulated part cannot have is a
  battery: the clock keeps time across a reset but not across a power cycle, and
  comes up at the firmware's build timestamp until `SETTIME`/`SETDATE`.

## 7. Primary risks

- **SPI1 LCD bandwidth** at 320×320×16 — mitigate with DMA, dirty-region /
  changed-line updates, and an 8bpp palette framebuffer expanded on the fly.
- **Single-core timing** if Core 1 stalls Core 0 — keep the shared VRAM/register
  snapshot lock-free (double buffer, single writer).
- **RP2040 SRAM pressure** — §4 budget; feature-flag expansion RAM and any large
  buffers.
- **Keyboard latency / ghosting** over a 10 kHz I²C link — poll in the outer loop,
  debounce in the encoder attachment as the Teensy does.

## 8. Reuse map (what to lift from where)

| Target module | Port from |
|---|---|
| CPU | `6502-DEV/Firmware/DB Emulator/lib/6502/CPU` + `vrEmu6502` |
| Memory map, RAM/ROM/Cart, tick loop | DB Emulator `src/main.cpp` (`read`/`write`/`tick`/`loop`) |
| VideoCard (register model) | DB Emulator `lib/6502/IO/VideoCard.*` |
| VDP renderer (modes + sprites) | DOB Display `src/main.cpp` (`renderGraphicsI/II`, `renderText`, `renderMulticolor`, `renderSprites`) |
| SoundCard + SID synth | DB Emulator `SoundCard.*` + DOB `sidISR`/`sidWrite` |
| VIA / keyboard / joystick | DB Emulator `GPIOCard` + `GPIOAttachments/*` |
| RTC, Storage, Serial, RAMBank | DB Emulator `RTCCard`, `StorageCard`, `SerialCard`, `RAMCard` |
| LCD driver, I²C keyboard, SD | `clockworkpi/PicoCalc` `Code/picocalc_helloworld` (`lcdspi/`, `i2ckbd/`) |
| Behaviour reference (any card) | `6502-EMULATOR/src/core/IO/*.ts` |
