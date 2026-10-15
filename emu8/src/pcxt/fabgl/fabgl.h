// fabgl.h - SHIM for the emu8 PC-XT (8086) port.
//
// The vendored fabgl sources (i8086, PIC8259, PIT8253, bios, ...) do
// #include "fabgl.h". A quote-include resolves to THIS file first (it lives in
// the same directory), so we satisfy the few library macros they actually need
// WITHOUT pulling in the whole FabGL library (VGA driver, FreeRTOS, ESP-IDF).
//
// The heavyweight pieces FabGL's PCEmulator example used (PS/2 keyboard stack,
// VGA scanout GraphicsAdapter, MCP23S17, serial UARTs, FreeRTOS task) are
// replaced by small purpose-built shims (i8042.h, MC146818.h, graphicsadapter.h,
// machine.h) so the same code links into both the desktop host harness (g++)
// and the ESP32-S3 device build.
//
// Licensing note: the i8086 CPU core (i8086.cpp) is MIT (8086tiny / Julian Olds).
// The fabgl device files (PIC8259, PIT8253, bios, ...) are GPLv3 - keep their
// original headers intact. See plan.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// i8086.cpp guards a diagnostic pragma behind these. Define them to a valid
// constant expression; value chosen so the guarded pragma is simply skipped.
#ifndef FABGL_ESP_IDF_VERSION_VAL
  #define FABGL_ESP_IDF_VERSION_VAL(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#endif
#ifndef FABGL_ESP_IDF_VERSION
  #define FABGL_ESP_IDF_VERSION 0
#endif

// On the ESP32 the i8086 core sprinkled a cache-priming workaround between
// PSRAM accesses. Off-device (and on the S3 with working PSRAM cache) it is a
// no-op. The device build can redefine this before including the core if needed.
#ifndef PSRAM_WORKAROUND2
  #define PSRAM_WORKAROUND2
#endif

// --- PIT real-time source -------------------------------------------------
// PIT8253.cpp drives its counters from a free-running ~5 MHz tick so IRQ0 fires
// at real wall-clock rate (18.2 Hz) regardless of emulation speed. FabGL used
// the ESP32 FRC1 hardware timer; we provide a portable 5 MHz monotonic source
// (defined in pcxt_time.cpp on the device via micros(), and in the host harness).
#define FRC_TIMER_PRESCALER_16  16
#define FRC1TimerMax            0xFFFFFFFFu

#ifdef __cplusplus
extern "C" {
#endif
void     FRC1Timer_init(int prescaler);
uint32_t FRC1Timer(void);   // free-running counter at PIT_FRC1_FREQUENCY (5 MHz)
#ifdef __cplusplus
}
#endif

// --- save states ----------------------------------------------------------
// Each chip lists its state ONCE in a snapshot(PcSnap &) member that both saves and loads (same
// field order both ways), so the two can never drift apart. The emu8 glue
// (src/pcxt/pcxt_snapshot.cpp) points `move`/`mark` at its snapshot file; nothing here depends on it.
// Fields go through fixed-width types so every build (ESP32, RP2350, desktop x64) shares one format.
struct PcSnap {
  void (*move)(void * ctx, void * p, size_t n);   // write (saving) or read (loading) n bytes at p
  void (*mark)(void * ctx, const char * tag);     // 4-char section marker (verified on load)
  void * ctx;
  bool   loading;

  void bytes(void * p, size_t n)                  { move(ctx, p, n); }
  void tag(const char * t)                        { mark(ctx, t); }
  template <class T> void val(T & v)              { bytes(&v, sizeof(v)); }
  void flag(bool & b)                             { uint8_t v = b ? 1 : 0; val(v); if (loading) b = v != 0; }
  void i32(int & v)                               { int32_t x = v; val(x); if (loading) v = x; }
  template <class E> void e32(E & e)              { int32_t x = (int32_t)e; val(x); if (loading) e = (E)x; }
};
