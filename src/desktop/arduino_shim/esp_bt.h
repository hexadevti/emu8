// esp_bt.h — desktop shim. c64.cpp releases the unused BT controller DRAM on the device; on the PC
// there's nothing to release, so these are no-ops returning ESP_OK.
#pragma once

#include <cstdint>

typedef enum {
  ESP_BT_MODE_IDLE = 0,
  ESP_BT_MODE_BLE,
  ESP_BT_MODE_CLASSIC_BT,
  ESP_BT_MODE_BTDM,
} esp_bt_mode_t;

inline int esp_bt_controller_mem_release(esp_bt_mode_t) { return 0; /* ESP_OK */ }
inline int esp_bt_mem_release(esp_bt_mode_t) { return 0; /* ESP_OK */ }
