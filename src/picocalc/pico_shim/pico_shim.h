// pico_shim.h - Arduino-ESP32 -> earlephilhower arduino-pico compatibility layer (PicoCalc).
//
// emu8 was written against Arduino-ESP32, and the shared/core sources call a handful of
// ESP-IDF-only APIs directly: the IRAM/RTC section attributes, heap_caps_*/ps_malloc, the
// reset-reason enum, the core-pinned FreeRTOS task helpers and the task watchdog. None of
// those exist on the RP2350 core, but every one has a one-line equivalent (or is simply
// meaningless there), so this header supplies them and NOT ONE core file has to change.
//
// This is the RP2350 twin of src/desktop/arduino_shim/Arduino.h, which does the same job for
// the SDL desktop build. The difference: arduino-pico is a real Arduino core, so Arduino.h,
// SPI.h, FS.h, SD.h and EEPROM.h are the genuine articles -- only the ESP-specific extras are
// shimmed here. board.h pulls this in for every translation unit; the sibling esp_*.h /
// driver/*.h files exist so the sources that #include those names by hand still resolve
// (the build task puts this directory on the include path with -I).
#pragma once
#ifndef EMU8_PICO_SHIM_H   // belt and braces: board.h includes this by relative path while
#define EMU8_PICO_SHIM_H   // the sibling esp_*.h shims include it as <pico_shim.h> off -I,
                           // and #pragma once does not always match the two on Windows.

#if defined(BOARD_PICOCALC)

#include <Arduino.h>      // the real arduino-pico core: Serial, SPI, millis(), the rp2040 object, ...
#include <EEPROM.h>       // must precede the `EEPROM` macro below (see the EEPROM block)
#include <FreeRTOS.h>     // FQBN option os=freertos -- the shared code is built around FreeRTOS SMP
#include <task.h>
#include <semphr.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_attr.h"     // IRAM_ATTR / DRAM_ATTR / RTC_NOINIT_ATTR (kept separate: C-safe)

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------
// Heap capabilities.
//
// The ESP32 has several physically distinct heaps (internal DRAM, external PSRAM, DMA-capable,
// 8-bit-addressable) and the cores pick between them with heap_caps_malloc(n, caps). The RP2350
// has ONE uniform SRAM heap, so every request lands in the same place -- except a request that
// explicitly asks for PSRAM, which must fail here (this board's 8MB PSRAM is on plain GPIOs and
// is not memory-mapped; see BOARD_HAS_PSRAM in board.h). Failing honestly matters: the callers
// that ask for SPIRAM all check for null and fall back.
// ---------------------------------------------------------------------------------------
#define MALLOC_CAP_EXEC       (1 << 0)
#define MALLOC_CAP_32BIT      (1 << 1)
#define MALLOC_CAP_8BIT       (1 << 2)
#define MALLOC_CAP_DMA        (1 << 3)
#define MALLOC_CAP_SPIRAM     (1 << 10)
#define MALLOC_CAP_INTERNAL   (1 << 11)
#define MALLOC_CAP_DEFAULT    (1 << 12)

// ps_malloc() is a REAL symbol, not a macro: src/tiny386/tiny386_core.cpp forward-declares it
// as `extern "C" void *ps_malloc(size_t)`, which a macro would break. Defined in pico_shim.cpp.
void  *ps_malloc(size_t n);
void  *ps_calloc(size_t n, size_t size);
void  *ps_realloc(void *p, size_t n);

void   *heap_caps_malloc(size_t n, uint32_t caps);
void   *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void   *heap_caps_realloc(void *p, size_t n, uint32_t caps);
void    heap_caps_free(void *p);
size_t  heap_caps_get_free_size(uint32_t caps);
size_t  heap_caps_get_largest_free_block(uint32_t caps);

// ---------------------------------------------------------------------------------------
// Reset reason. videoSetup() shows the boot splash on a HARDWARE reset and skips it on a
// software restart, so only the ESP_RST_SW case has to be distinguished accurately.
// ---------------------------------------------------------------------------------------
typedef enum {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON,
  ESP_RST_EXT,
  ESP_RST_SW,
  ESP_RST_PANIC,
  ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT,
  ESP_RST_WDT,
  ESP_RST_DEEPSLEEP,
  ESP_RST_BROWNOUT,
  ESP_RST_SDIO,
} esp_reset_reason_t;

esp_reset_reason_t esp_reset_reason(void);

// ---------------------------------------------------------------------------------------
// Task watchdog. Nothing subscribes the RP2350 to a watchdog unless we start one, so the
// "stop the WDT from rebooting us during multi-second SD I/O" calls are simply no-ops.
// ---------------------------------------------------------------------------------------
static inline void disableCore0WDT(void) {}
static inline void disableCore1WDT(void) {}
static inline void disableLoopWDT(void)  {}
static inline void enableCore0WDT(void)  {}
static inline void enableLoopWDT(void)   {}
static inline void esp_task_wdt_reset(void) {}

#define ESP_OK 0

// _BV() is an AVR-era Arduino macro that Arduino-ESP32 still ships and arduino-pico does not.
// src/shared/keyboardPs2.cpp uses it to assemble PS/2 scancode bits (that file compiles here but
// no-ops itself, since this board's KEYBOARD_DATA_PIN/KEYBOARD_IRQ_PIN are -1).
#ifndef _BV
#define _BV(bit) (1 << (bit))
#endif

#ifdef __cplusplus
}   // extern "C"
#endif

// ---------------------------------------------------------------------------------------
// Core-pinned task creation.
//
// TWO corrections are folded into these macros so no caller has to know about them:
//
//  1. THE CORE NUMBERS ARE INVERTED. Arduino-ESP32 runs loop() on core 1, so this codebase
//     pins the render loop to core 0 and the USB/CPU work to core 1. arduino-pico hard-pins
//     setup()/loop() to core 0 -- the opposite. Mapping caller-core N to hardware core (1-N)
//     therefore preserves the INTENT ("the other core from loop()") everywhere.
//
//  2. THE STACK UNIT DIFFERS. Arduino-ESP32's xTaskCreate* takes a stack size in BYTES;
//     vanilla FreeRTOS takes it in WORDS (4 bytes here). The dynamic wrapper divides by 4 so
//     `xTaskCreatePinnedToCore(..., 4096, ...)` still means 4KB. The STATIC wrapper must NOT
//     divide: there the depth has to match the caller's StackType_t[] length in elements, so
//     the array and the count are adjusted together at the call site instead (see the
//     BOARD_PICOCALC branch around renderTaskStack in src/shared/video.cpp).
// ---------------------------------------------------------------------------------------
#define PICO_CORE_MASK(core)  ((UBaseType_t)(1u << (1 - (core))))

#define xTaskCreatePinnedToCore(fn, name, stackBytes, param, prio, handle, core)              \
        xTaskCreateAffinitySet((fn), (name), ((stackBytes) / sizeof(StackType_t)), (param),   \
                               (prio), PICO_CORE_MASK(core), (handle))

#define xTaskCreateStaticPinnedToCore(fn, name, stackWords, param, prio, stack, tcb, core)    \
        xTaskCreateStaticAffinitySet((fn), (name), (stackWords), (param), (prio), (stack),    \
                                     (tcb), PICO_CORE_MASK(core))

// The PLAIN xTaskCreate() call sites (the four Apple II disk/HD loader tasks) were written for
// Arduino-ESP32 too, where the stack argument is likewise in BYTES. Vanilla FreeRTOS reads it as
// WORDS, so an unconverted `4096` quietly reserves 16KB -- 64KB across the four, an eighth of
// this board's SRAM, taken from the same heap the Apple II memory map needs. Convert it the same
// way, so the call sites keep meaning what they say on every board.
//
// The inner call is parenthesised so the macro does not expand into itself; <task.h> is included
// above, so the real declaration is already parsed before this macro exists.
#define xTaskCreate(fn, name, stackBytes, param, prio, handle)                                 \
        (xTaskCreate)((fn), (name), ((stackBytes) / sizeof(StackType_t)), (param), (prio),     \
                      (handle))

// ---------------------------------------------------------------------------------------
// std::mutex stand-in.
//
// arm-none-eabi libstdc++ is built without gthreads, so <mutex> compiles to an (almost) empty
// header and std::mutex simply does not exist. page_lock -- the Apple II page-flip lock shared by
// softswitches.cpp and the render task -- is the ONLY std threading primitive in the whole tree,
// and it only ever calls lock()/unlock(), so a FreeRTOS mutex behind those two methods is a drop-in.
// Deliberately NOT a complete std::mutex: no try_lock(), no native_handle(). Like std::mutex it is
// non-copyable and non-recursive (a second take from the same task blocks).
// ---------------------------------------------------------------------------------------
#ifdef __cplusplus
class PicoMutex {
public:
  PicoMutex()  { _h = xSemaphoreCreateMutex(); }
  ~PicoMutex() { if (_h) vSemaphoreDelete(_h); }
  PicoMutex(const PicoMutex &) = delete;
  PicoMutex &operator=(const PicoMutex &) = delete;
  void lock()   { if (_h) xSemaphoreTake(_h, portMAX_DELAY); }
  void unlock() { if (_h) xSemaphoreGive(_h); }
private:
  SemaphoreHandle_t _h;
};
#endif

// ---------------------------------------------------------------------------------------
// The `ESP` object.
//
// Arduino-ESP32 exposes a global `EspClass ESP`; arduino-pico exposes a global `RP2040 rp2040`
// instead, with the same information under different names and no `ESP` at all. Exactly four
// methods are used across the whole tree (getCycleCount / getFreeHeap / getCpuFreqMHz /
// restart), and each maps one-to-one, so a four-line forwarder lets every core keep saying
// ESP.something().
//
// getCycleCount() is only ever used to measure elapsed cycles (the MHz meter in cpu.cpp and
// c64_cpu.cpp subtract two readings), so the fact that the RP2350 counter has a different
// epoch and rolls over on its own schedule does not matter -- only the difference is read.
// ---------------------------------------------------------------------------------------
#ifdef __cplusplus
class PicoEspClass {
public:
  uint32_t getCycleCount() { return rp2040.getCycleCount(); }
  uint32_t getFreeHeap()   { return (uint32_t)rp2040.getFreeHeap(); }
  uint32_t getCpuFreqMHz() { return (uint32_t)(F_CPU / 1000000UL); }
  void     restart()       { rp2040.restart(); }
};
extern PicoEspClass ESP;    // the single instance lives in pico_shim.cpp

// ---------------------------------------------------------------------------------------
// EEPROM helper accessors.
//
// Both cores back EEPROM with a RAM buffer flushed to flash by commit(), and both spell
// begin/read/write/commit identically. Arduino-ESP32 additionally offers typed accessors;
// src/shared/eprom.cpp uses four of them (readBool/writeBool/readChar/writeChar), and
// arduino-pico has none. All four are single bytes, so they are one line each on top of
// the real read()/write().
//
// Subclass + macro rather than patching eprom.cpp: the library declares its own global as
// `extern EEPROMClass EEPROM`, which cannot grow members, so `EEPROM` is redirected to our
// instance instead. <EEPROM.h> is included ABOVE so that extern declaration is already past
// before the macro exists. The library global stays linked but is never begin()-ed, and the
// macro is harmless next to the EEPROM_SIZE / *EEPROMaddress names in emu.h (different tokens).
// ---------------------------------------------------------------------------------------
class PicoEEPROMClass : public EEPROMClass {
public:
  bool   readBool(int address)                { return read(address) != 0; }
  int8_t readChar(int address)                { return (int8_t)read(address); }
  size_t writeBool(int address, bool value)   { write(address, value ? 1 : 0); return 1; }
  size_t writeChar(int address, int8_t value) { write(address, (uint8_t)value); return 1; }
};
extern PicoEEPROMClass picoEEPROM;   // the single instance lives in pico_shim.cpp
#define EEPROM picoEEPROM
#endif

#endif // BOARD_PICOCALC
#endif // EMU8_PICO_SHIM_H
