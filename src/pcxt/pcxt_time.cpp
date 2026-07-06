// pcxt_time.cpp - device implementation of the PIT real-time source (5 MHz).
//
// PIT8253.cpp calls FRC1Timer() to know how much real time has elapsed so IRQ0
// (the 18.2 Hz system timer) fires at wall-clock rate independent of how fast the
// 8086 interpreter runs. On the ESP32-S3 we derive a 5 MHz tick from the 1 MHz
// esp_timer microsecond clock. The host harness provides its own definition, so
// this translation unit is device-only.

#ifndef PCXT_HOST_BOOT

#include <stdint.h>
#include "../../board.h"

// Microsecond clock. On the ESP32 this is the ESP-IDF C-linkage `esp_timer_get_time`; on the desktop
// SDL build it's the arduino_shim's C++-linkage version (hal.cpp). Match the linkage of each so the
// reference resolves (the IDF symbol is C, the shim symbol is C++/mangled).
#if defined(BOARD_DESKTOP)
int64_t esp_timer_get_time();
#else
extern "C" int64_t esp_timer_get_time(void);
#endif

extern "C" void FRC1Timer_init(int /*prescaler*/) { }

extern "C" uint32_t FRC1Timer(void) {
  return (uint32_t)((uint64_t)esp_timer_get_time() * 5ULL);  // 5 ticks/us = 5 MHz
}

#endif // !PCXT_HOST_BOOT
