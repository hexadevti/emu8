# emu8

**emu8** is a multi-platform 8-bit retro-console emulator — 6502-family and Z80 cores — for low-cost
ESP32 boards with a built-in TFT and microSD. Pick a system on the boot splash and it boots disk/cartridge images
straight off a microSD card — no PC, no external ROM files.

Nine systems share one firmware, dispatched at runtime from the boot splash — the classic 8-bit cores
plus the experimental **Apple IIGS**, **PC-XT** and **386** targets that are still in development:

| System | CPU | Status | Image formats |
| --- | --- | --- | --- |
| **Apple II+ / IIe** | 6502 | Full | `.dsk` `.do` `.po` (5.25″ floppy) · `.hdv` `.po` `.2mg` (ProDOS HD) |
| **Commodore 64** | 6510 | Playable (VIC-II + SID + CIA); `.crt` cartridge support is partial | `.prg` `.d64` `.crt` |
| **NES** | 2A03 (6502) | Playable; mappers 0–4 | `.nes` (iNES) |
| **Atari 2600** | 6507 (6502) | Playable | `.a26` `.bin` (2K/4K/8K/16K/32K) |
| **MSX1** | Z80 | Playable (TMS9918 VDP + AY-3-8910 PSG); BIOS from SD or embedded C-BIOS | `.rom` `.mx1` `.dsk` |
| **Sega Master System** | Z80 | Playable (Mode 4 VDP + SN76489 PSG); Sega mapper + line interrupts; boots cartridges directly (no BIOS) | `.sms` `.bin` |
| **Apple IIGS** | 65C816 | **In development** — boots ROM 01, 40-col text + HiRes/DHiRes, standard ProDOS 5.25″/800 KB disks, 1-bit speaker. SHR-heavy/protected titles and GS-native (Ensoniq) sound are not done. | `.dsk` `.po` `.2mg` `.hdv` |
| **PC-XT** | Intel 8086 | **In development** — fabgl-based IBM PC-XT: BIOS POST, CGA text/graphics, PC speaker; mounts floppy (A:) and hard-disk (C:) images and boots DOS. BIOS from `/roms/pcxt/bios.bin` | `.img` `.ima` `.dsk` `.vhd` `.hdd` |
| **386** | Intel i386 | **In development** — vendored [tiny386](https://github.com/hchunhui/tiny386) core + VGA, SeaBIOS/VGABIOS from `/roms/tiny386`; PS/2 keyboard + mouse, A:/C: disk mounts, boots DOS and heavier PC OSes. PSRAM-heavy, so it targets the **P4** (not built for the S3) | `.img` `.ima` `.vhd` `.hdd` |

> Derived from [hexadevti/Apple2Esp32](https://github.com/hexadevti/Apple2Esp32). The original was a
> single-system Apple II emulator; emu8 generalises the renderer, input, audio and SD layers into a
> shared core and adds C64, NES and Atari 2600 emulation plus a second hardware target.

---

## Supported boards

emu8 builds for **three boards** from the same source tree — the target is selected at *compile time*
by a single macro (`-DBOARD_JC4827W543` / `-DBOARD_JC1060P470`), defined per build task. The hardware
abstraction lives in [`board.h`](board.h) as capability macros (display backend, audio path, input
path, touch bus) that the shared code switches on.

| | **ESP32 CYD** (default) | **Guition JC4827W543** | **Guition JC1060P470** |
| --- | --- | --- | --- |
| MCU | ESP32-WROOM-32 (no PSRAM) | ESP32-S3 (OPI PSRAM) | ESP32-P4 (32MB PSRAM) |
| Display | ILI9341 320×240 SPI, via TFT_eSPI | NV3041A 480×272 QSPI, via Arduino_GFX | JD9165 1024×600 **MIPI-DSI**, via esp_lcd |
| Input | PS/2 keyboard + analog joystick/paddles | **USB SNES gamepad + USB keyboard** (USB-HID host) | USB-HID host + touch |
| On-screen keyboard | XPT2046 touch (display bus) | XPT2046 touch (dedicated SPI bus) | **GT911** capacitive (I2C) |
| Audio | Internal DAC (GPIO26) → on-board amp | I2S → NS4168 Class-D amp | I2S → **ES8311 codec** → NS4150B amp |
| Storage | microSD (VSPI) | microSD (shared HSPI w/ touch) | microSD (**SD_MMC** / SDIO) |

The "CYD" target is the [ESP32-2432S024](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024)
(2.4″, ILI9341); the 2.8″ ESP32-2432S028 shares the same driver and pin map.

> **JC1060P470 (ESP32-P4)** — working & verified on hardware (display, GT911 touch, on-screen keyboard,
> ES8311 audio, SD_MMC, and USB host via a vendored IDF-5-patched EspUsbHost fork). It is built against Arduino-ESP32
> **core 3.x** (IDF 5.x) on an *isolated* toolchain (its own `ARDUINO_DIRECTORIES_DATA/USER` at
> `~/.emu6502-p4`, outside the repo), so the CYD/S3 builds stay on 2.0.17 and unchanged. It needs the
> JD9165 `esp_lcd` vendor driver dropped in — see [`src/shared/p4/README.md`](src/shared/p4/README.md)
> for the one-time setup, status notes, and fallback levers.

---

## Table of contents

- [Features](#features)
- [Boot & platform selection](#boot--platform-selection)
- [Controls](#controls)
- [On-screen options menu](#on-screen-options-menu)
- [Software prerequisites](#software-prerequisites)
- [Build & flash](#build--flash)
- [Desktop (SDL2) debug build](#desktop-sdl2-debug-build)
- [microSD card preparation](#microsd-card-preparation)
- [Display configuration (CYD / TFT_eSPI)](#display-configuration-cyd--tft_espi)
- [Pin assignments (CYD)](#pin-assignments-cyd)
- [Board resources & schematics](#board-resources--schematics)
- [Project structure](#project-structure)
- [Experimental: Apple IIGS (in development)](#experimental-apple-iigs-in-development)
- [Credits & license](#credits--license)

---

## Features

### Shared core

- **Boot-splash platform selector** — tap **APPLE / C64 / NES / ATARI / IIGS / MSX / SMS / PCXT / 386**
  to choose a system; the selection persists in EEPROM and auto-boots next time. (**IIGS**, **PCXT** and
  **386** are experimental / in development.)
- **microSD storage** for every platform, with on-screen file browsers per system.
- **On-screen touch keyboard** (OSK) on both boards, plus PS/2 on the CYD and a **USB keyboard** on the JC4827W543.
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

### MSX1

- **Z80** CPU (shared with the SMS core), **TMS9918** VDP, **AY-3-8910** PSG and the **8255 PPI**;
  64 KB work RAM in slot 3.
- Loads `.rom` / `.mx1` cartridges and `.dsk` floppies. BIOS is read from an `MSXBIOS.ROM` on the SD
  card, or falls back to the **embedded C-BIOS** (no Disk BASIC in that case).
- USB keyboard / on-screen keyboard for typing, joystick/gamepad on controller port 1.

### Sega Master System

- **Z80** CPU, **315-5124** VDP (**Mode 4** tile/sprite mode plus the TMS9918 legacy modes and line
  interrupts) and the **SN76489** PSG.
- Boots cartridge ROMs directly at `PC=0x0000` — **no BIOS** needed. **Sega memory mapper** for
  bank-switched carts; `.sms` / `.bin` images.
- 8-way d-pad + two fire buttons on controller port 1; **F11** = the SMS **PAUSE** button (NMI).

### PC-XT *(in development)*

- fabgl-derived **Intel 8086** IBM PC-XT: runs the embedded PC BIOS (POST text in the CGA buffer),
  **CGA** text/graphics and the **PC speaker**; 1 MB main RAM.
- Mounts disk images as **A:** (floppy) or **C:** (hard disk) — `.img` / `.ima` / `.dsk` / `.vhd` /
  `.hdd` — and boots **MS-DOS**. Files are auto-routed to A:/C: by size. BIOS loads from
  `/roms/pcxt/bios.bin` on the SD card.
- USB keyboard → XT scancodes; gamepad → arrow/enter injection.

### 386 *(in development)*

- Vendored **[tiny386](https://github.com/hchunhui/tiny386)** (hchunhui, BSD-3) **Intel i386** PC with
  **VGA** (RGB565 framebuffer, nearest-scaled to the panel). **SeaBIOS** + **VGABIOS** are read from
  `/roms/tiny386` on the SD card.
- **PS/2 keyboard + mouse** emulation, A:/C: disk mounts (`.img` / `.ima` / `.vhd` / `.hdd`), boots
  MS-DOS and heavier PC operating systems.
- The machine is allocated in PSRAM, so it targets the **JC1060P470 (ESP32-P4)** — it is **not built
  for the S3** (too large for that toolchain).

---

## Boot & platform selection

On power-up emu8 shows a boot splash with one button per system — **APPLE**, **C64**, **NES**,
**ATARI**, **IIGS**, **MSX**, **SMS**, **PCXT** and **386** (the IIGS, PCXT and 386 still in
development). Tap one to switch systems (this saves the choice and reboots into it); tap elsewhere or
wait for the timeout to boot the currently-selected platform. On the CYD a joystick button also
dismisses the splash.

---

## Controls

Controls depend on the board:

**ESP32 CYD** — PS/2 keyboard + analog joystick. Global shortcuts:

| Keys | Action |
| --- | --- |
| `Ctrl` + `Esc` | Open / close the on-screen options menu |
| `Ctrl` + `F12` | Apple **Reset** (CPU reset) |
| `Ctrl` + `F5` | Reboot the ESP32 |

**Guition JC4827W543** — a **USB SNES gamepad** for gameplay and/or a **USB keyboard** for typing, plus
the on-screen touch keyboard (OSK). Plug either into the native USB port. The OSK and options menu are
also reachable through the touch UI.

USB keyboard mapping (works on every platform):

| Keys | Action |
| --- | --- |
| Letters / digits / symbols | Type into the active system (Apple/IIGS keycode, C64 keyboard matrix, …) |
| Arrow keys | Cursor / d-pad |
| `F10` | Open / close the options menu (arrows navigate, `Enter` activates) |
| `F11` | Apple / IIGS **Reset** (CPU reset) |
| NES: arrows + `X`=A · `Z`=B · `Enter`=Start · `Tab`=Select | NES controller 1 |
| Atari: arrows + `Space`/`X`=Fire · `Enter`=Reset · `Tab`=Select | Atari stick + console switches |
| C64 (when **JOYSTICK** is on): arrows + `Space`=Fire | C64 joystick (port per **JOY PORT**) |

> Only keyboards that expose the standard HID **boot** protocol are decoded, and one USB device works at
> a time (no hub). The native USB port has no VBUS switching, so **hot-swapping devices needs a tap of
> RST** — a freshly plugged keyboard/gamepad enumerates cleanly on a cold boot.

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

## Desktop (SDL2) debug build

The **same** emulator code also builds as a native PC program (`BOARD_DESKTOP`) so the cores can be run
under **gdb, sanitizers and second-by-second iteration** — it is a *debug target*, not a separate
emulator. Only the hardware leaves (display / audio / input / SD / EEPROM) are swapped, behind
`#if defined(BOARD_DESKTOP)`; the `arduino-cli` device build never defines `BOARD_DESKTOP`, so the
firmware binaries stay byte-for-byte identical.

Fidelity choices: **multi-thread faithful** (each FreeRTOS task → a `std::thread`), **32-bit `-m32`**
(same `long`/pointer widths as the Xtensa ESP32), and an **SDL2 interactive** window.

```sh
# Windows (MSYS2 MINGW32 shell) — native .exe + gdb:
cmake -G "MinGW Makefiles" -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j && ./build/emu8.exe

# WSL/Linux + ThreadSanitizer (catches the multi-thread races):
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DEMU_SANITIZE=thread
cmake --build build -j && ./build/emu8
```

Runtime: put ROM/disk images under `./sdcard` (the emulated SD card; override with `EMU_SD_DIR`);
settings persist to `./eeprom.bin`. `F10` opens the settings/file-browser menu, `F11`/`F12` are the
platform reset/menu keys. `ESP.restart()` maps to a process exit, so set `EMU_PLATFORM=<name>` to boot
a platform directly (also the deterministic choice for gdb).

**Status:** builds & runs (MinGW-w64 i686 / SDL2) and is smoke-tested booting **Apple II** and **MSX1**
to `Ready.`. Per-platform polish (upscale options, gamepad maps) is ongoing; the **PC-XT / fabgl**
runtime on desktop is **Phase 3** (currently a stub — selecting it on desktop is inert, though it runs
on the device). Full setup, toolchain and status notes live in
[`src/desktop/README.md`](src/desktop/README.md).

---

## microSD card preparation

1. Format a microSD card as **FAT32**.
2. Copy your images to the card root, mixing systems freely:
   - Apple: `.dsk` / `.do` / `.po` / `.hdv` / `.2mg`
   - C64: `.prg` / `.d64` / `.crt`
   - NES: `.nes`
   - Atari 2600: `.a26` / `.bin`
   - MSX1: `.rom` / `.mx1` / `.dsk` (plus an `MSXBIOS.ROM`, or it falls back to the embedded C-BIOS)
   - Sega Master System: `.sms` / `.bin`
   - PC-XT: `.img` / `.ima` / `.dsk` / `.vhd` / `.hdd` (plus the BIOS at `/roms/pcxt/bios.bin`)
   - 386: `.img` / `.ima` / `.vhd` / `.hdd` (plus SeaBIOS + VGABIOS under `/roms/tiny386/`)
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
| [`emu8.ino`](emu8.ino) | `setup()` / `loop()` — per-platform init and the main dispatch |
| [`board.h`](board.h) | Board selection (CYD vs JC4827W543), capability macros, pin map |
| [`emu.h`](emu.h) · [`proto.h`](proto.h) · [`globals.cpp`](globals.cpp) | Shared state (`extern`), prototypes, definitions |
| [`rom.h`](rom.h) | Embedded Apple II/IIe ROMs |
| [`User_Setup.h`](User_Setup.h) | TFT_eSPI configuration (CYD) |
| [`src/shared/`](src/shared/) | Display (TFT_eSPI + Arduino_GFX backends), video/splash, SD, EEPROM, options UI, touch keyboard, USB gamepad + USB keyboard, joystick, audio, logging |
| [`src/apple2/`](src/apple2/) | 6502 CPU, memory, language card, soft switches, Disk II, ProDOS HD, mouse |
| [`src/c64/`](src/c64/) | 6510, VIC-II, SID, CIA, keyboard, disk, `.crt` loader, ROMs |
| [`src/nes/`](src/nes/) | 2A03 CPU, PPU, APU, iNES loader, mappers 0–4 |
| [`src/atari/`](src/atari/) | 6507 CPU, TIA, RIOT, cartridge bank-switching, audio |
| [`src/z80/`](src/z80/) | Shared Z80 CPU core (MSX1 + SMS) |
| [`src/msx/`](src/msx/) | MSX1 — TMS9918 VDP, AY-3-8910 PSG, 8255 PPI, slot/BIOS, disk |
| [`src/sms/`](src/sms/) | Sega Master System — 315-5124 VDP (Mode 4), SN76489 PSG, Sega mapper, cart loader |
| [`src/pcxt/`](src/pcxt/) | PC-XT — fabgl i8086 machine, CGA, PC speaker, disk mount (in development) |
| [`src/tiny386/`](src/tiny386/) | 386 — vendored tiny386 i386 + VGA core and emu8 glue (in development) |
| [`src/iigs/`](src/iigs/) | Apple IIGS core — 65C816, banked memory, ROM 01 boot, video, disk (in development) + the original feasibility benchmark |
| [`src/desktop/`](src/desktop/) | SDL2 desktop debug build — Arduino/FreeRTOS shims, SDL display/audio/input backends (see its [README](src/desktop/README.md)) |
| [`host/`](host/) | Off-device debug harnesses (MSX / SMS / IIGS cores on a PC) |
| [`data/`](data/) · [`resources/`](resources/) | Sample disk images / test files |

---

## Experimental: Apple IIGS (in development)

The Apple IIGS is a **work-in-progress fifth platform** (`PLATFORM_IIGS`), selectable from the boot
splash on the ESP32-S3. It is **not finished** — treat its features as experimental and expect rough
edges.

What works today: 65C816 CPU core, banked memory, ROM 01 boots to its banner, 40-column text plus
HiRes / Double-HiRes video, the Apple II 1-bit speaker, and booting standard ProDOS 5.25″ and 800 KB
disks (`.dsk` / `.po` / `.2mg` / `.hdv`) from SD. A 1 MHz throttle makes Apple II software run at the
original speed.

Known gaps: GS-native super-hires-heavy or hardware-copy-protected titles don't run, GS-native Ensoniq
5503 sound isn't implemented, full 65816 edge-case validation is pending, the clock/Battery-RAM
self-test is stubbed, and boot is slow (~30 s). The core lives in
[`src/iigs/iigs_boot.cpp`](src/iigs/); a desktop debug harness ([`host/iigs_host.cpp`](host/)) runs the
same CPU core on a PC for fast iteration.

The original PSRAM-timing feasibility benchmark ([`src/iigs/m0_bench.*`](src/iigs/)) is still present:
it compiles to nothing unless built with `-DIIGS_M0_BENCH` (S3 only), then runs at the top of
`setup()`, prints results over serial, and halts.

---

## Credits & license

- Upstream emulator: [hexadevti/Apple2Esp32](https://github.com/hexadevti/Apple2Esp32).
- CYD board documentation: [jpduhen/CYD_2.4inch_ESP32-2432S024](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024).

Refer to the upstream project for licensing terms.
