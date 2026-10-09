#pragma once
// speedmeter.h - live emulated-CPU speed for the settings title bar ("Z80  3.58 MHz").
//
// The frame-paced cores (MSX / SMS / ZX / Coleco / PCXT) used to fill their xxxMeasuredMhz once,
// from an uncapped boot benchmark, so the title showed the FAST speed forever: toggling SPEED to
// NORMAL changed the real speed but never the label. This keeps a rolling ~1 s window instead.
// tick() is called once per frame (not per instruction) so it costs nothing on the hot path;
// restart() after a pause / the settings window / a machine reset (which zeroes cpu.cycles) so idle
// time and counter jumps never land in a window.
#include <stdint.h>

struct SpeedMeter {
  uint32_t c0 = 0, t0 = 0;
  void restart(uint32_t count) { c0 = count; t0 = millis(); }
  // count: running cycle (or instruction) counter; scale: MHz per (count/us) -- 1 for cycles.
  void tick(uint32_t count, float &outMhz, float scale = 1.0f) {
    uint32_t dt = millis() - t0;
    if (dt < 1000) return;
    uint32_t n = count - c0;                         // unsigned: a 32-bit wrap still subtracts right
    if (n / dt > 1000000u) { restart(count); return; } // >1 GHz: counter was zeroed under us (reset)
    outMhz = (float)n * scale / ((float)dt * 1000.0f);
    restart(count);
  }
};
