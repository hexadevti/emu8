# Guition JC1060P470 (ESP32-P4) port — `src/shared/p4/`

This folder holds the ESP32-P4-only hardware leaves for the **Guition JC1060P470** target
(`-DBOARD_JC1060P470`). The rest of the emulator (all platform cores + the `DisplayGFX` canvas /
UI / fill-screen logic) is shared and unchanged — only the hardware *outputs* differ from the
ESP32-S3 (JC4827W543) board:

| Subsystem | JC1060P470 (P4) | files here |
|---|---|---|
| Display | JD9165 1024×600 **MIPI-DSI** via `esp_lcd` (canvas flushed to it) | `jd9165_dsi.*` |
| Touch | **GT911** capacitive, I2C (in `../touchkeyboard.cpp`, `#if BOARD_TOUCH_GT911`) | `p4_i2c.*` |
| Audio | **ES8311** codec (I2C) → NS4150B amp, I2S w/ MCLK (in `../audio_amp.cpp`) | `es8311.*`, `p4_i2c.*` |
| microSD | **SD_MMC** (SDIO), 1-bit (in `../sd.cpp`, `#if BOARD_SD_MMC`) | — |
| Input host | **EspUsbHost** on native USB-HS OTG — vendored & patched for IDF 5.x (shared decoders in `../usbgamepad.cpp` / `../usbkeyboard.cpp`) | `usb/EspUsbHost.*` |

Pins live in [`board.h`](../../../board.h) under `#elif defined(BOARD_JC1060P470)`.

---

## ⚠️ Required: drop in the JD9165 esp_lcd vendor driver

`jd9165_dsi.cpp` `#include`s **`esp_lcd_jd9165.h`** and calls the vendor panel driver. That driver
(the panel's manufacturer init sequence + DSI/DPI timing macros) is NOT reproduced here — copy the
two files from the board's reference repo into THIS folder:

```
https://github.com/cheops/JC1060P470C_I_W
  1-Demo/Demo_Arduino/1_1_Lvgl_V8/esp32p4_arduino_mipi-dsi_lvgl/src/lcd/esp_lcd_jd9165.c  ->  src/shared/p4/esp_lcd_jd9165.c
  1-Demo/Demo_Arduino/1_1_Lvgl_V8/esp32p4_arduino_mipi-dsi_lvgl/src/lcd/esp_lcd_jd9165.h  ->  src/shared/p4/esp_lcd_jd9165.h
```

They are MIT/Apache (ESP IoT Solution). The struct/macro names used in `jd9165_dsi.cpp`
(`JD9165_PANEL_BUS_DSI_2CH_CONFIG`, `JD9165_PANEL_IO_DBI_CONFIG`,
`JD9165_1024_600_PANEL_60HZ_DPI_CONFIG`, `jd9165_vendor_config_t`, `esp_lcd_new_panel_jd9165`)
match that driver; if your copy differs, reconcile `jd9165_dsi.cpp` against its `jd9165_lcd.cpp`.

---

## Isolated build (Arduino-ESP32 core 3.x — keeps the 2.0.17 CYD/S3 builds untouched)

The ESP32-P4 needs core **3.x** (IDF 5.x); the rest of the repo builds on **2.0.17**. The P4 build
tasks point `ARDUINO_DIRECTORIES_DATA`/`USER` at `~/.emu6502-p4/` so its core + libraries are fully
separate. This dir MUST live OUTSIDE the sketch folder — if it is inside the repo, arduino-cli tries
to copy the deep core tree into its build staging and overruns the Windows 260-char path limit
(`miniz.h ... path not found`). One-time setup (PowerShell):

```powershell
$env:ARDUINO_DIRECTORIES_DATA="$HOME\.emu6502-p4\data"
$env:ARDUINO_DIRECTORIES_USER="$HOME\.emu6502-p4\user"
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.10         # any 3.x with ESP32-P4 support
arduino-cli lib install "GFX Library for Arduino"   # Arduino_GFX (canvas + fonts)
# (No EspUsbHost needed: USB host is deferred on the P4 — see "Fallbacks" below. Input is touch/OSK.)
arduino-cli board details --fqbn esp32:esp32:esp32p4   # confirm the FQBN menu options/values
```

Then build/flash from **Terminal → Run Task…** → *Arduino: Build [& Upload] (JC1060P470)*
(adjust the upload COM port in [`.vscode/tasks.json`](../../../.vscode/tasks.json), default `COM6`).

---

## Status: FULLY WORKING & user-verified on hardware (2026-06-25)

Display (JD9165 DSI 1024×600, colours + scale), GT911 touch + on-screen keyboard, ES8311 audio,
SD_MMC, and **USB host** (USB keyboard verified typing) are all confirmed working. Settings that were
verified (so the values in `board.h` are good for this exact board; keep these notes if porting to a
sibling panel):

- **microSD SD_MMC pins** 43/44/39/40/41/42 (mounted 1-bit: CLK/CMD/D0) — mounts a 7.5GB SDHC card. ✓
- **GT911** at I2C `0x5D` — point block at 0x8150 is `[x_lo, x_hi, y_lo, y_hi, …]` (NO track-id byte);
  decoded in `gt911Poll()` as `x=p[0]|p[1]<<8, y=p[2]|p[3]<<8`. Orientation is direct (top-left →
  logical 0,0), so `GT911_FLIP_X/Y/SWAP_XY` are all 0. Flip them only if porting to a rotated panel.
- **ES8311** detected (chip id 0x83/0x11); DAC volume `0x32` set to `0x90` (≈−23 dB) — 0xBF (0 dB) was
  too loud with the NS4150B 3W amp. Tune to taste. (`es8311.cpp` assumes MCLK = 256·Fs.)
- **USB host** runs on the native USB-HS OTG (the serial/flash console is the separate USB-Serial-JTAG,
  so the OTG controller is free). Plug the device into the OTG USB-C port (a USB-C→USB-A adapter for a
  normal keyboard/gamepad). Set `GT911_DEBUG 1` in `../touchkeyboard.cpp` to re-print touch coords.

## USB host: vendored EspUsbHost fork, patched for IDF 5.x

The S3 uses the external `EspUsbHost` apple2esp32 fork, which is **IDF-4.4-only** (its `EspUsbHost.h`
pulls `rom/usb/usb_common.h`, gone in the P4's IDF 5.x). Rather than depend on a patched external lib,
a copy lives in [`usb/`](usb/) here, compiled only on the P4 (`#if BOARD_INPUT_USB && BOARD_PANEL_DSI`).
Two IDF-4.4→5.x patches were enough: `<rom/usb/usb_common.h>` → `<usb/usb_types_ch9.h>`, and a handful
of descriptor-type constant macros (`USB_DEVICE_DESC` → `USB_B_DESCRIPTOR_TYPE_DEVICE`, etc.) — see the
top of `usb/EspUsbHost.h`. `../usbgamepad.cpp` / `../usbkeyboard.cpp` include this copy on the P4 and
the external fork on the S3 (`#if BOARD_PANEL_DSI`). To disable USB host, set `BOARD_INPUT_USB` 0 —
GT911 touch + the on-screen keyboard already cover all input.

## Fallbacks / risk levers (in `board.h`)

- If `Arduino_GFX`'s DSI path is unstable, the `DisplayGFX` output is isolated to the three
  `dsiPanel*()` functions in `jd9165_dsi.cpp` — they can be re-pointed at a pure `esp_lcd` framebuffer
  without touching the rest of the display code.
