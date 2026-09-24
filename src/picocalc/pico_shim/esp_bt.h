// esp_bt.h - PicoCalc shim. c64FreeBtMem() releases the ESP32's unused Bluetooth controller DRAM;
// the RP2350 has no radio, so this is guarded out by BOARD_HAS_BLE=0. Present for completeness.
#pragma once
#include "pico_shim.h"

typedef enum { ESP_BT_MODE_IDLE = 0, ESP_BT_MODE_BLE, ESP_BT_MODE_CLASSIC_BT, ESP_BT_MODE_BTDM } esp_bt_mode_t;

static inline int esp_bt_controller_mem_release(esp_bt_mode_t) { return 0; }
static inline int esp_bt_mem_release(esp_bt_mode_t) { return 0; }
