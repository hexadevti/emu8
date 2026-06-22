# emu6502

**emu6502** is a multi-platform 6502/65xx retro-console emulator for low-cost ESP32 boards with a
built-in TFT and microSD. Pick a system on the boot splash and it boots disk/cartridge images
straight off a microSD card — no PC, no external ROM files.

Four cores share one firmware, dispatched at runtime from the boot splash:

| System | CPU | Status | Image formats |
| --- | --- | --- | --- |
| **Apple II+ / IIe** | 6502 | Full | `.dsk` `.do` `.po` (5.25″ floppy) · `.hdv` `.po` `.2mg` (ProDOS HD) |
| **Commodore 64** | 6510 | Playable (VIC-II + SID + CIA); `.crt` cartridge support is partial | `.prg` `.d64` `.crt` |
| **NES** | 2A03 (6502) | Playable; mappers 0–4 | `.nes` (iNES) |
| **Atari 2600** | 6507 (6502) | Playable | `.a26` `.bin` (2K/4K/8K/16K/32K) |

> Derived from [hexadevti/Apple2Esp32](https://github.com/hexadevti/Apple2Esp32). The original was a
> single-system Apple II emulator; emu6502 generalises the renderer, input, audio and SD layers into a
> shared core and adds C64, NES and Atari 2600 emulation plus a second hardware target.

---

## Supported boards

emu6502 builds for **two boards** from the same source tree — the target is selected at *compile time*
by a single macro (`-DBOARD_JC4827W543`), defined per build task. The hardware abstraction lives in
[`board.h`](board.h) as capability macros (display backend, audio path, input path, touch bus) that the
shared code switches on.

| | **ESP32 CYD** (default) | **Guition JC4827W543** |
| --- | --- | --- |
| MCU | ESP32-WROOM-32 (no PSRAM) | ESP32-S3 (OPI PSRAM) |
| Display | ILI9341 320×240 SPI, via TFT_eSPI | NV3041A 480×272 QSPI, via Arduino_GFX |
| Input | PS/2 keyboard + analog joystick/paddles | **USB SNES gamepad** (USB-HID host) |
| On-screen keyboard | XPT2046 touch (display bus) | XPT2046 touch (dedicated SPI bus) |
| Audio | Internal DAC (GPIO26) → on-board amp | I2S → NS4168 Class-D amp |
| Storage | microSD (VSPI) | microSD (shared HSPI w/ touch) |

The "CYD" target is the [ESP32-2432S024](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024)
(2.4″, ILI9341); the 2.8″ ESP32-2432S028 shares the same driver and pin map.

---

## Table of contents

- [Features](#features)
- [Boot & platform selection](#boot--platform-selection)
- [Controls](#controls)
- [On-screen options menu](#on-screen-options-menu)
- [Software prerequisites](#software-prerequisites)
- [Build & flash](#build--flash)
- [microSD card preparation](#microsd-card-preparation)
- [Display configuration (CYD / TFT_eSPI)](#display-configuration-cyd--tft_espi)
- [Pin assignments (CYD)](#pin-assignments-cyd)
- [Board resources & schematics](#board-resources--schematics)
- [Project structure](#project-structure)
- [Experimental: Apple IIGS feasibility](#experimental-apple-iigs-feasibility)
- [Credits & license](#credits--license)

---

## Features

### Shared core

- **Boot-splash platform selector** — tap **APPLE / C64 / NES / ATARI** to choose a system; the
  selection persists in EEPROM and auto-boots next time.
- **microSD storage** for every platform, with on-screen file browsers per system.
- **On-screen touch keyboard** (OSK) on both boards, plus PS/2 on the CYD.
- **Audio** routed to the board's amplifier — internal DAC on the CYD, I2S Class-D on the S3.
- **Settings persistence in EEPROM** (platform, machine type, speed, sound/volume, joystick,
  video options and the last-loaded image per platform).
- **Up-scaling / smooth up-scaling**, plus a fill-screen mode and NES frame-skip on the 480×272 S3 panel.

### Apple II+ / IIe

- 6502 core with the full documented opcode set; II+ and IIe models switchable at runtime
  (80-column / aux-RAM, language card, IOU soft switches).
- **Disk II** — boots 5.25″ 140 KB floppies (`.dsk` / `.do` / `.po`) with track write-back to SD.
- **ProDOS hard disk** (block device) — large `.hdv` / `.po` / `.2mg` volumes.
- Video: LoRes, HiRes, text 40/80-column, page 1/2, mixed/split, colour or mono.
- Speaker, analog joystick/paddles, AppleMouse-style mouse plumbing.
- Built-in **6502 debugger** (step / breakpoint / stack trace). ROMs embedded in [`rom.h`](rom.h).

### Commodore 64

- 6510 CPU, **VIC-II** video (incl. raster IRQ), **SID** 3-voice audio, two **CIA** chips, keyboard
  matrix and joystick (port 1/2 selectable).
- Loads `.prg` programs, `.d64` disk images and `.crt` cartridges (cartridge cold-reset + VIC raster
  IRQ work; bank-switched / EasyFlash cartridge types are still incomplete).
- Optional boot-autoload of the last image.

### NES

- 2A03 CPU, **PPU** rendering and **APU** audio.
- iNES `.nes` ROMs with **mappers 0–4**: NROM (0), MMC1 (1), UxROM (2), CNROM (3), MMC3 (4, with
  scanline IRQ). PRG is streamed from SD; large ROMs that exceed the DRAM budget are skipped with an
  on-screen note.
- Analog stick / USB gamepad → controller 1.

### Atari 2600

- 6507 CPU, **TIA** (video + audio) and **RIOT** (RAM + timer + I/O).
- Cartridge bank-switching: 2K/4K (non-banked), 8K **F8**, 16K **F6**, 32K **F4**, with Superchip RAM
  auto-detection. Whole ROM held in RAM (no SD streaming).
- Analog stick / USB gamepad → joystick + console switches.

---

## Boot & platform selection

On power-up emu6502 shows a boot splash with four buttons — **APPLE**, **C64**, **NES**, **ATARI**.
Tap one to switch systems (this saves the choice and reboots into it); tap elsewhere or wait for the
timeout to boot the currently-selected platform. On the CYD a joystick button also dismisses the splash.

---

## Controls

Controls depend on the board:

**ESP32 CYD** — PS/2 keyboard + analog joystick. Global shortcuts:

| Keys | Action |
| --- | --- |
| `Ctrl` + `Esc` | Open / close the on-screen options menu |
| `Ctrl` + `F12` | Apple **Reset** (CPU reset) |
| `Ctrl` + `F5` | Reboot the ESP32 |

**Guition JC4827W543** — USB SNES gamepad for gameplay + the on-screen touch keyboard (OSK) for typing
and menus. The OSK and options menu are reachable through the touch UI.

---

## On-screen options menu

Open with `Ctrl`+`Esc` (CYD) or the touch UI (S3); emulation pauses while it is open. The file list
shows the images for the **current platform**; function keys / touch toggles cover the settings:

| Key | Toggle |
| --- | --- |
| `F1` | **HD** ↔ **DISK** (Apple: which device the file list manages) |
| `F2` | **IIe** ↔ **II+** machine model |
| `F3` | **Fast** ↔ **1 MHz** CPU speed |
| `F4` | **Speaker** ↔ **Mute** |
| `F5` | **Joystick** on ↔ off |
| `F6` | **Color** ↔ **Mono** video |
| `F7` | **Upscale** ↔ **Regular** |
| `F8` | **Smooth upscale** ↔ **Regular** |
| `Enter` | Mount / load the highlighted image |
| `Ctrl` + `Enter` | Save settings/selection to EEPROM and reboot |
| `Esc` | Exit the menu |

A built-in **6502 debugger** (Apple core) offers pause/continue, single-step (`F10`), breakpoint by
address, and a live instruction/stack trace.

---

## Software prerequisites

- **arduino-cli** (or the Arduino IDE).
- **ESP32 Arduino core** `esp32:esp32` — tested with **2.0.17**.
- **Libraries:**
  - `TFT_eSPI` **2.5.43** — CYD display (configured at library level, see below).
  - `Arduino_GFX` (`GFX Library for Arduino`) — JC4827W543 NV3041A QSPI display.

```bash
arduino-cli core install esp32:esp32@2.0.17
arduino-cli lib install "TFT_eSPI@2.5.43"
arduino-cli lib install "GFX Library for Arduino"
# CYD only: copy this repo's User_Setup.h into the TFT_eSPI library folder (see below)
```

---

## Build & flash

Build/upload commands live in [`.vscode/tasks.json`](.vscode/tasks.json) — one pair of tasks per board.
The board is selected purely by *which task you run*: the JC4827W543 tasks add
`-DBOARD_JC4827W543`, which flips [`board.h`](board.h) to the ESP32-S3 / Arduino_GFX / USB / I2S
configuration. The CYD tasks pass no such define and produce the original ESP32 + TFT_eSPI firmware.

In VS Code: **Terminal → Run Task…** then pick the build (or build & upload) task for your board.
`Ctrl+Shift+B` runs the default CYD build. Serial monitor: **115200** baud.

### ESP32 CYD (default, COM4)

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32:PSRAM=disabled,PartitionScheme=huge_app,CPUFreq=240,FlashMode=qio,FlashFreq=80,FlashSize=4M,UploadSpeed=921600,LoopCore=1,EventsCore=1,DebugLevel=none \
  --build-property compiler.optimization_flags=-O2 \
  .
arduino-cli upload -p COM4 --fqbn esp32:esp32:esp32:PSRAM=disabled,PartitionScheme=huge_app,CPUFreq=240,FlashMode=qio,FlashFreq=80,FlashSize=4M,UploadSpeed=921600,LoopCore=1,EventsCore=1,DebugLevel=none .
```

### Guition JC4827W543 / ESP32-S3 (COM5)

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app,CPUFreq=240,FlashMode=qio,FlashSize=4M,UploadSpeed=921600,USBMode=default,CDCOnBoot=default,DebugLevel=none \
  --build-property compiler.optimization_flags=-O2 \
  --build-property compiler.cpp.extra_flags=-DBOARD_JC4827W543 \
  .
arduino-cli upload -p COM5 --fqbn esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app,CPUFreq=240,FlashMode=qio,FlashSize=4M,UploadSpeed=921600,USBMode=default,CDCOnBoot=default,DebugLevel=none .
```

> The JC4827W543 has no auto-reset circuit: to upload, hold **BOOT**, tap **RST**, release **BOOT**;
> tap **RST** alone to run. Both boards use the `huge_app` partition scheme.

---

## microSD card preparation

1. Format a microSD card as **FAT32**.
2. Copy your images to the card root, mixing systems freely:
   - Apple: `.dsk` / `.do` / `.po` / `.hdv` / `.2mg`
   - C64: `.prg` / `.d64` / `.crt`
   - NES: `.nes`
   - Atari 2600: `.a26` / `.bin`
3. Insert the card, power on, pick a platform on the splash, then choose an image from its on-screen
   file browser.

Sample Apple II disks live in [`data/`](data/) (DOS 3.3, ProDOS 2.4.2, Lode Runner, Karateka,
Ghostbusters); copy them to the SD root. A few C64 test files are in [`resources/`](resources/).

---

## Display configuration (CYD / TFT_eSPI)

The CYD build configures TFT_eSPI at **library** level, so before building you must make the library
use this repo's [`User_Setup.h`](User_Setup.h):

1. Locate the installed `TFT_eSPI` library folder (e.g. `Documents/Arduino/libraries/TFT_eSPI`).
2. Replace its `User_Setup.h` with the one from this repo (back up the original first).

Key settings: `ILI9341_2_DRIVER`, `TFT_WIDTH 240`, `TFT_HEIGHT 320`, `SPI_FREQUENCY 80000000`
(drop to 55/40 MHz on display corruption), backlight `TFT_BL 27` active-HIGH.

The JC4827W543 display needs **no** `User_Setup.h` — Arduino_GFX is configured in code
([`src/shared/display_gfx.cpp`](src/shared/display_gfx.cpp)) with the QSPI pin map from
[`board.h`](board.h).

---

## Pin assignments (CYD)

### Display — ILI9341 (in [`User_Setup.h`](User_Setup.h))

| Signal | GPIO | | Signal | GPIO |
| --- | --- | --- | --- | --- |
| MISO (SDO) | 12 | | DC (RS) | 2 |
| MOSI (SDI) | 13 | | Backlight (BL) | 27 (active HIGH) |
| SCLK | 14 | | Touch CS (XPT2046) | 33 |
| CS | 15 | | Touch IRQ | 36 |

### Emulator peripherals & storage (in [`board.h`](board.h))

| Function | GPIO | | Function | GPIO |
| --- | --- | --- | --- | --- |
| microSD SCK | 18 | | PS/2 keyboard DATA | 21 |
| microSD MISO | 19 | | PS/2 keyboard CLK/IRQ | 22 |
| microSD MOSI | 23 | | Joystick analog X | 4 (shared w/ RGB-LED red) |
| microSD CS | 5 | | Joystick analog Y | 35 (input-only) |
| Speaker / audio out | 26 (DAC ch.2) | | Joystick buttons | 34 (resistor ladder; LDR pin) |
| Status LED | 17 | | | |

The JC4827W543 pin map (QSPI display, I2S amp, shared HSPI touch+SD) is documented inline in
[`board.h`](board.h).

---

## Board resources & schematics

CYD board documentation lives in the manufacturer repo
[**jpduhen/CYD_2.4inch_ESP32-2432S024**](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024):

| Resource | Location |
| --- | --- |
| Schematic sheet 1 (power, charging, SD, RGB LED, ADC) | [`5-Schematic/ESP32-2432S024-1-V1.0.png`](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024/blob/main/5-Schematic/ESP32-2432S024-1-V1.0.png) |
| Schematic sheet 2 (ESP32, display, touch, audio) | [`5-Schematic/2432S024-2-V1.0.png`](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024/blob/main/5-Schematic/2432S024-2-V1.0.png) |
| Board specification (EN) | [`2-Specification/`](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024/blob/main/2-Specification/ESP32-2432S024%20Specifications-EN.pdf) |
| Datasheets (ESP32, flash, amp, charger) | [`4-Driver_IC_Data_Sheet/`](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024/tree/main/4-Driver_IC_Data_Sheet) |

Both boards can run from a single-cell **3.7 V Li-ion/LiPo** on the JST 1.25 mm connector and charge
over USB. ⚠️ Verify **BAT+/BAT− polarity against the silkscreen** before plugging in — reversed leads
can destroy the board. The connector is **JST 1.25 mm**, not the common JST-PH 2.0 mm.

---

## Project structure

The code is organised as separate translation units under `src/` — a shared layer plus one folder per
emulated system. The top-level sketch wires them together:

| Path | Purpose |
| --- | --- |
| [`emu6502.ino`](emu6502.ino) | `setup()` / `loop()` — per-platform init and the main dispatch |
| [`board.h`](board.h) | Board selection (CYD vs JC4827W543), capability macros, pin map |
| [`emu.h`](emu.h) · [`proto.h`](proto.h) · [`globals.cpp`](globals.cpp) | Shared state (`extern`), prototypes, definitions |
| [`rom.h`](rom.h) | Embedded Apple II/IIe ROMs |
| [`User_Setup.h`](User_Setup.h) | TFT_eSPI configuration (CYD) |
| [`src/shared/`](src/shared/) | Display (TFT_eSPI + Arduino_GFX backends), video/splash, SD, EEPROM, options UI, touch keyboard, USB gamepad, joystick, audio, logging |
| [`src/apple2/`](src/apple2/) | 6502 CPU, memory, language card, soft switches, Disk II, ProDOS HD, mouse |
| [`src/c64/`](src/c64/) | 6510, VIC-II, SID, CIA, keyboard, disk, `.crt` loader, ROMs |
| [`src/nes/`](src/nes/) | 2A03 CPU, PPU, APU, iNES loader, mappers 0–4 |
| [`src/atari/`](src/atari/) | 6507 CPU, TIA, RIOT, cartridge bank-switching, audio |
| [`src/iigs/`](src/iigs/) | Apple IIGS memory-feasibility benchmark (experimental, opt-in) |
| [`data/`](data/) · [`resources/`](resources/) | Sample disk images / test files |

---

## Experimental: Apple IIGS feasibility

[`src/iigs/m0_bench.*`](src/iigs/) holds a throwaway PSRAM-timing benchmark used to evaluate whether a
65C816 Apple IIGS core is viable on the ESP32-S3. It compiles to nothing unless built with
`-DIIGS_M0_BENCH` (S3 only); when enabled it runs at the top of `setup()`, prints results over serial,
then halts. No IIGS emulation is implemented — this is research scaffolding only.

---

## Credits & license

- Upstream emulator: [hexadevti/Apple2Esp32](https://github.com/hexadevti/Apple2Esp32).
- CYD board documentation: [jpduhen/CYD_2.4inch_ESP32-2432S024](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024).

Refer to the upstream project for licensing terms.
