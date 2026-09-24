// pico_shim.cpp - out-of-line half of the ESP-IDF compatibility layer (see pico_shim/pico_shim.h).
//
// Only the pieces that must be real symbols rather than inline code live here: ps_malloc (the
// tiny386 core forward-declares it as an extern "C" function, so it cannot be a macro), the
// heap_caps_* family, and esp_reset_reason(). Guarded so arduino-cli, which compiles every .cpp
// under src/ for every target, produces an empty object file on the other boards.
#include "../../board.h"

#if defined(BOARD_PICOCALC)

#include "pico_shim/pico_shim.h"

extern "C" {

// --- allocation -------------------------------------------------------------------------------
// One flat SRAM heap, so the "which memory?" argument only decides whether we refuse. A request
// that names SPIRAM without accepting internal RAM as a fallback CANNOT be served on this board
// (BOARD_HAS_PSRAM 0 -- the PicoCalc's 8MB PSRAM hangs off plain GPIOs and is not memory-mapped),
// and every such call site checks the result for null, so returning null is the honest answer.
static inline bool wantsUnavailablePsram(uint32_t caps) {
  return (caps & MALLOC_CAP_SPIRAM) && !(caps & MALLOC_CAP_INTERNAL) && !BOARD_HAS_PSRAM;
}

void *ps_malloc(size_t n)                 { return malloc(n); }
void *ps_calloc(size_t n, size_t size)    { return calloc(n, size); }
void *ps_realloc(void *p, size_t n)       { return realloc(p, n); }

void *heap_caps_malloc(size_t n, uint32_t caps) {
  return wantsUnavailablePsram(caps) ? nullptr : malloc(n);
}
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
  return wantsUnavailablePsram(caps) ? nullptr : calloc(n, size);
}
void *heap_caps_realloc(void *p, size_t n, uint32_t caps) {
  return wantsUnavailablePsram(caps) ? nullptr : realloc(p, n);
}
void heap_caps_free(void *p) { free(p); }

size_t heap_caps_get_free_size(uint32_t caps) {
  return wantsUnavailablePsram(caps) ? 0 : rp2040.getFreeHeap();
}

// The pico-sdk heap exposes no largest-contiguous-block query. The callers use this only to log a
// fragmentation figure, so report the total free size; it is an upper bound, never a false
// negative, and no allocation decision is made from it.
size_t heap_caps_get_largest_free_block(uint32_t caps) {
  return heap_caps_get_free_size(caps);
}

// --- reset reason -----------------------------------------------------------------------------
// The one consumer (videoSetup) asks a single question: "did WE reboot the board, or did the user
// power-cycle / press RST?" -- a hardware reset re-shows the SELECT SYSTEM splash, a software one
// boots straight into the chosen platform.
//
// ESP.restart() reboots the RP2350 through the watchdog, so both SOFT_RESET and WDT_RESET mean
// "we did it". Treating WDT_RESET as software is safe here precisely because this firmware never
// arms a hardware watchdog (see the no-op disableCore0WDT/... in pico_shim.h) -- nothing else can
// produce it. NOTE: arduino-pico's own source marks getResetReason() as untested on RP2350; if the
// splash logic misbehaves, this function is the single place to correct it.
esp_reset_reason_t esp_reset_reason(void) {
  switch (rp2040.getResetReason()) {
    case RP2040::SOFT_RESET:  return ESP_RST_SW;
    case RP2040::WDT_RESET:   return ESP_RST_SW;
    case RP2040::PWRON_RESET: return ESP_RST_POWERON;
    case RP2040::RUN_PIN_RESET: return ESP_RST_EXT;
    default:                  return ESP_RST_UNKNOWN;
  }
}

}   // extern "C"

// The one instance of the ESP-compatibility forwarder declared in pico_shim.h. It holds no
// state -- every method forwards straight to arduino-pico's rp2040 object.
PicoEspClass ESP;

// The EEPROM object every caller actually reaches, via the `#define EEPROM picoEEPROM` in
// pico_shim.h. The library's own EEPROMClass global is left untouched and unused.
#undef EEPROM
PicoEEPROMClass picoEEPROM;

// Bring-up checkpoints (see pico_shim.h). Written by the render task on core 1, read by the
// 6502 meter on core 0.

#endif // BOARD_PICOCALC
