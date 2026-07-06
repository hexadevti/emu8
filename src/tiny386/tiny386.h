// tiny386.h — public entry points for the PLATFORM_TINY386 emulator (Intel i386 PC + VGA).
// Wraps the vendored tiny386 C99 core (hchunhui/tiny386, BSD-3) with the emu8 glue in
// tiny386.cpp. Mirrors the PC-XT platform surface (see proto.h / src/pcxt/pcxt.h) so the
// shared dispatch (emu8.ino), render loop (src/shared/video.cpp) and settings UI treat it
// like any other platform. The heavy machine is allocated in PSRAM; ROMs are embedded.
#pragma once
#include <stdint.h>

void tiny386Setup();                  // alloc guest RAM/VGA (PSRAM) + framebuffer; build the PC; load BIOS; reset
void tiny386Loop();                   // run the i386 in chunks (from loop()); the PIT/RTC drive IRQs in real time
bool tiny386RenderFrame();            // copy the VGA framebuffer to the panel; false when unchanged (skip flush)
void tiny386ForceRedraw();            // force a full repaint after a menu/screen clear
void tiny386SetInput(uint8_t joyMask); // gamepad -> arrow/enter scancodes (M4)
void tiny386KeyDown(uint8_t hidUsage, bool shift, bool ctrl, bool alt); // USB key -> PS/2 make code
void tiny386KeyUp(uint8_t hidUsage);  // USB key -> PS/2 break code
void tiny386MouseInput(int dx, int dy, uint8_t buttons);  // USB mouse -> PS/2 mouse (relative)
void tiny386HardReset();              // request a soft reboot of the emulated PC
bool tiny386LoadSelected(const char *path);  // (deprecated) — use tiny386MountA/C
bool tiny386MountA(const char *sel);         // settings: A: floppy live mount/eject (no reboot)
bool tiny386MountC(const char *sel);         // settings: C: hard disk re-attach + soft-reboot the PC
void tiny386ScanFiles();              // settings: rescan SD root for disk images
void loadTiny386FilesSync();          // populate the options disk browser at boot
bool tiny386RenderLoadWarning();      // startup overlay (false: SeaBIOS shows its own POST)
