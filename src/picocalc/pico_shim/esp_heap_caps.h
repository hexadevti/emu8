// esp_heap_caps.h - PicoCalc shim. The NES / PC-XT sources include this by name for
// heap_caps_malloc() and the MALLOC_CAP_* flags; board.h has already pulled them in via
// pico_shim.h. See the "Heap capabilities" block there for what they mean on one flat SRAM heap.
#pragma once
#include "pico_shim.h"
