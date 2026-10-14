// esp_task_wdt.h - PicoCalc shim. Only Arduino-ESP32 core 3.x takes this path
// (`#if ESP_ARDUINO_VERSION_MAJOR >= 3`, which is 0 here), so nothing in it is actually
// reached on this board -- but the header must resolve, and the no-op WDT calls it
// declares live in pico_shim.h.
#pragma once
#include "pico_shim.h"

typedef struct {
  uint32_t timeout_ms;
  uint32_t idle_core_mask;
  bool     trigger_panic;
} esp_task_wdt_config_t;

static inline int esp_task_wdt_reconfigure(const esp_task_wdt_config_t *) { return 0; }
