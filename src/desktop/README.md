# emu8 — Desktop (SDL2) debug build

Run the **same** emulator code the ESP32 devices run, but on a PC, so you can debug it with **gdb,
sanitizers, and second-by-second iteration**. This is a *debug target*, not a separate emulator: the
desktop is just another "board" (`BOARD_DESKTOP`) — only the hardware leaves (display/audio/input/
SD/EEPROM) are swapped, behind `#if defined(BOARD_DESKTOP)`. The `arduino-cli` device build never
sees any of this and never defines `BOARD_DESKTOP`, so the device binaries stay identical.

Fidelity choices: **multi-thread faithful** (each FreeRTOS task → a `std::thread`), **32-bit
`-m32`** (ILP32, same `long`/pointer widths as the Xtensa ESP32), **SDL2 interactive** window.

## Layout

```
src/desktop/
  arduino_shim/        Fake <Arduino.h>, SD.h, FS.h, EEPROM.h, SPI.h, BLEDevice.h, EspUsbHost.h,
                       driver/{i2s,adc,dac}.h, esp_*.h  — the HAL contract (prepended to -I)
  hal.cpp              FreeRTOS-on-std::thread, Serial/ESP/EEPROM instances, time base, semaphores
  sd_host.cpp          FSSetup(): the emulated SD card = a host directory (g_sdRoot)
  display_sdl.{h,cpp}  [TODO] class DisplayGFX over SDL2 (mirrors src/shared/display_gfx)
  audio_sdl.cpp        [TODO] ampBegin/ampWriteDac8/ampWriteMono + speaker over SDL2 audio
  input_sdl.cpp        [TODO] SDL keysym -> HID -> usbKeyboardReport(); gamepad/mouse -> joy/touch
  main.cpp             [TODO] SDL_Init -> setup() -> CPU thread -> renderLoop() on main thread
  pcxt_stub.cpp        [TODO] empty PC-XT entry points (fabgl port deferred to Phase 3)
CMakeLists.txt (repo root)
```

## Toolchain

There is **no host compiler installed yet** — install one of these. (`cmake` 4.x is already present;
WSL Ubuntu-24.04 exists but its VHDX currently fails to mount — `wsl --shutdown` then retry, or fix
the `D:` drive permission.)

### A) Windows native `.exe` + gdb (primary) — MSYS2 / MinGW-w64 **32-bit**
```sh
winget install MSYS2.MSYS2
# then in the "MSYS2 MINGW32" shell:
pacman -S --needed mingw-w64-i686-gcc mingw-w64-i686-gdb mingw-w64-i686-cmake mingw-w64-i686-SDL2 make
```

### B) Sanitizers rail (TSan for the multi-thread races; ASan/UBSan) — WSL/Linux 32-bit
```sh
sudo apt install build-essential gcc-multilib g++-multilib cmake libsdl2-dev:i386
# (clang gives the best sanitizer output: sudo apt install clang)
```

## Build & run

```sh
# Windows (MSYS2 MINGW32 shell):
cmake -G "MinGW Makefiles" -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/emu8.exe

# WSL/Linux + ThreadSanitizer:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DEMU_SANITIZE=thread
cmake --build build -j && ./build/emu8
```

Confirm 32-bit: `file build/emu8.exe` → **PE32** (not PE32+).

Runtime: put ROM/disk images under `./sdcard` (the emulated SD card; override with `EMU_SD_DIR`).
Settings persist to `./eeprom.bin`. F10 opens the settings/file-browser menu (mapped in the input
backend), F11/F12 are platform reset/menu keys (see `src/shared/usbkeyboard.cpp`).

## Status

- **BUILDS & RUNS** (MinGW-w64 i686, g++ 16, SDL2 2.32): `emu8.exe` = PE32 (32-bit). Smoke-tested
  booting **Apple II** (embedded ROM) and **MSX1** (embedded C-BIOS) to `Ready.` — SDL window, PSG
  audio, USB-style keyboard, host SD dir, and a persistent `eeprom.bin` all working.
- **Done:** board branch, `emu.h` hook, full Arduino/ESP/FreeRTOS shim (+ `dirent.h` over `<io.h>`),
  FS/SD + EEPROM shims, `hal.cpp`, `sd_host.cpp`, and all SDL backends (`display_sdl`, `audio_sdl`,
  `input_sdl`, `main.cpp`, `pcxt_stub`). Guarded edits to `video.cpp`, `touchkeyboard.cpp`,
  `eprom.cpp`, `msx2_v9938.cpp` (all behind `#if [!]defined(BOARD_DESKTOP)` — device builds unchanged).
- **Next:** per-platform polish (display upscale options, gamepad mappings); PC-XT/fabgl runtime is
  **Phase 3** (currently a stub — selecting PLATFORM_PCXT on desktop is inert).
- **Notes / minor divergences:** text is drawn with the proportional FreeSans9pt7b for all sizes
  (device uses a 6x8 built-in for dense rows — cosmetic only). The 480x272 fill-screen/centering path
  is collapsed to a straight 320x240 framebuffer (CYD-like). ESP.restart() = process exit, so the
  splash platform-picker exits rather than rebooting — use `EMU_PLATFORM=<name>` to boot a platform
  directly (also the deterministic choice for gdb).
