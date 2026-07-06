// iigs_mem.cpp - Apple IIGS 24-bit banked memory (M2 skeleton). See iigs_mem.h.

#include "iigs_mem.h"
#include <Arduino.h>
#include "esp_heap_caps.h"
#include <string.h>

uint8_t* iigsBankPtr[256];

static uint8_t* sramBanks  = nullptr;   // banks $00,$01 (2 x 64KB) in internal SRAM
static uint8_t* psExpansion = nullptr;  // banks $02.. in PSRAM

// M2 config: enough to validate the bankPtr mechanism + SRAM/PSRAM split + cross-bank execution.
// (Full layout - $E0/$E1 video/shadow + the rest of expansion - lands in M3 alongside SHR/shadow.)
#define IIGS_SRAM_BANKS 2          // $00, $01
#define IIGS_PS_FIRST   0x02
#define IIGS_PS_BANKS   0x1E       // $02..$1F (~1.9 MB) - room for cross-bank tests + ROM staging

static inline bool isIObank(uint8_t b) { return b == 0x00 || b == 0x01 || b == 0xE0 || b == 0xE1; }

bool iigsMemInit() {
  static bool done = false;
  if (done) return true;
  for (int b = 0; b < 256; b++) iigsBankPtr[b] = nullptr;
  sramBanks   = (uint8_t*)heap_caps_malloc((size_t)IIGS_SRAM_BANKS * 0x10000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  psExpansion = (uint8_t*)ps_malloc((size_t)IIGS_PS_BANKS * 0x10000);
  if (!sramBanks || !psExpansion) return false;
  memset(sramBanks, 0, (size_t)IIGS_SRAM_BANKS * 0x10000);
  memset(psExpansion, 0, (size_t)IIGS_PS_BANKS * 0x10000);
  iigsBankPtr[0x00] = sramBanks + 0 * 0x10000;
  iigsBankPtr[0x01] = sramBanks + 1 * 0x10000;
  for (int b = 0; b < IIGS_PS_BANKS; b++) iigsBankPtr[IIGS_PS_FIRST + b] = psExpansion + (size_t)b * 0x10000;
  done = true;
  return true;
}

uint8_t iigsRead24(uint32_t a) {
  uint8_t bank = (uint8_t)(a >> 16); uint16_t off = (uint16_t)a;
  if ((off & 0xF000) == 0xC000 && isIObank(bank)) return 0;   // I/O window stub (softswitches -> M3)
  uint8_t* p = iigsBankPtr[bank];
  return p ? p[off] : 0;
}
void iigsWrite24(uint32_t a, uint8_t v) {
  uint8_t bank = (uint8_t)(a >> 16); uint16_t off = (uint16_t)a;
  if ((off & 0xF000) == 0xC000 && isIObank(bank)) return;     // I/O window stub
  uint8_t* p = iigsBankPtr[bank];
  if (p) p[off] = v;
}
