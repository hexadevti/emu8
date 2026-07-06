// msx_cart.cpp - MSX cartridge slot read/write with the common bank-switched mappers:
//   PLAIN   - 16K/32K linear ROM at $4000 (no banking)
//   KONAMI  - "Konami without SCC": 8K banks, regs at $6000/$8000/$A000 ($4000 fixed bank 0)
//   SCC     - "Konami with SCC":    8K banks, regs at $5000/$7000/$9000/$B000 (SCC sound stubbed)
//   ASCII8  - 8K banks, regs at $6000/$6800/$7000/$7800
//   ASCII16 - 16K banks, regs at $6000/$7000
//
// The ROM image stays fully resident (PSRAM on device); banking just repoints the per-8K window
// indices, like the NES mapper windows. Detection is the classic fMSX/blueMSX write-address
// heuristic; the active mapper is logged. SCC waveform sound is M3.5 (here it just reads as 0xFF).

#include "msx.h"
#include "msx_cart.h"
#include <string.h>

namespace msx {

enum { MAP_PLAIN = 0, MAP_KONAMI, MAP_SCC, MAP_ASCII8, MAP_ASCII16 };

struct Cart {
  const uint8_t* data = nullptr;
  int      size = 0;
  int      mapper = MAP_PLAIN;
  int      nBanks8k = 0;
  uint16_t bank8k[4] = {0, 1, 2, 3};   // 8K bank index for windows $4000/$6000/$8000/$A000
  bool     sccEnabled = false;
};
static Cart g_cart[3];                 // slots 1 and 2

bool cartPresent(int slot) { return (slot >= 1 && slot <= 2) && g_cart[slot].data != nullptr; }

uint8_t cartRead(int slot, uint16_t a) {
  if (slot < 1 || slot > 2) return 0xFF;
  Cart& c = g_cart[slot];
  if (!c.data || a < 0x4000 || a >= 0xC000) return 0xFF;
  if (c.mapper == MAP_SCC && c.sccEnabled && a >= 0x9800 && a < 0xA000) return 0xFF;  // SCC area (M3.5)
  int w = (a - 0x4000) >> 13;                                   // 0..3
  uint32_t off = (uint32_t)c.bank8k[w] * 0x2000 + (a & 0x1FFF);
  return (off < (uint32_t)c.size) ? c.data[off] : 0xFF;
}

void cartWrite(int slot, uint16_t a, uint8_t v) {
  if (slot < 1 || slot > 2) return;
  Cart& c = g_cart[slot];
  if (!c.data || c.nBanks8k <= 0) return;
  int mask = c.nBanks8k - 1;                                    // ROMs are power-of-two sized
  bool pow2 = (c.nBanks8k & mask) == 0;
  auto setBank = [&](int w, int b) { c.bank8k[w] = (uint16_t)(pow2 ? (b & mask) : (b % c.nBanks8k)); };
  switch (c.mapper) {
    case MAP_KONAMI:
      if      ((a & 0xE000) == 0x6000) setBank(1, v);
      else if ((a & 0xE000) == 0x8000) setBank(2, v);
      else if ((a & 0xE000) == 0xA000) setBank(3, v);
      break;
    case MAP_SCC:
      if      ((a & 0xF800) == 0x5000) setBank(0, v);
      else if ((a & 0xF800) == 0x7000) setBank(1, v);
      else if ((a & 0xF800) == 0x9000) { setBank(2, v); c.sccEnabled = ((v & 0x3F) == 0x3F); }
      else if ((a & 0xF800) == 0xB000) setBank(3, v);
      break;
    case MAP_ASCII8:
      if (a >= 0x6000 && a < 0x8000) setBank((a >> 11) & 3, v);  // 0x6000/0x6800/0x7000/0x7800
      break;
    case MAP_ASCII16:
      if      (a >= 0x6000 && a < 0x6800) { setBank(0, 2 * v); setBank(1, 2 * v + 1); }   // $4000-$7FFF
      else if (a >= 0x7000 && a < 0x7800) { setBank(2, 2 * v); setBank(3, 2 * v + 1); }   // $8000-$BFFF
      break;
    default: break;                                             // PLAIN: no banking
  }
}

// fMSX/blueMSX-style write-address heuristic.
static int detectMapper(const uint8_t* d, int size) {
  if (size <= 0x8000) return MAP_PLAIN;                         // 8/16/32K linear
  int konami = 0, scc = 0, ascii8 = 0, ascii16 = 0, a8only = 0;
  for (int i = 0; i + 2 < size; i++) {
    if (d[i] != 0x32) continue;                                 // LD (nn),A
    uint16_t addr = (uint16_t)(d[i + 1] | (d[i + 2] << 8));
    switch (addr) {
      case 0x6000: konami++; ascii8++; ascii16++; break;
      case 0x8000: case 0xA000: konami++; break;
      case 0x5000: case 0x9000: case 0xB000: scc++; break;
      case 0x7000: scc++; ascii8++; ascii16++; break;
      case 0x6800: case 0x7800: ascii8++; a8only++; break;      // registers UNIQUE to ASCII8
      case 0x77FF: ascii16++; break;
    }
  }
  int best = MAP_KONAMI, bv = konami;                            // default for unknown >32K
  if (scc     > bv) { bv = scc;     best = MAP_SCC; }
  if (ascii8  > bv) { bv = ascii8;  best = MAP_ASCII8; }
  if (ascii16 > bv) { bv = ascii16; best = MAP_ASCII16; }
  // ASCII16 ROMs touch only 0x6000/0x7000 -> they tie ASCII8 and lose above. A genuine ASCII8 ROM
  // must write its unique bank registers 0x6800/0x7800; without those it's ASCII16.
  if (best == MAP_ASCII8 && a8only == 0) best = MAP_ASCII16;
  return best;
}

int cartMapper(int slot) { return (slot >= 1 && slot <= 2) ? g_cart[slot].mapper : -1; }

} // namespace msx

void msxCartLoadImage(int slot, const uint8_t* data, int len) {
  if (slot < 1 || slot > 2) return;
  msx::Cart& c = msx::g_cart[slot];
  c.data = data; c.size = len;
  c.nBanks8k = (len + 0x1FFF) / 0x2000;
  c.mapper = msx::detectMapper(data, len);
  c.sccEnabled = false;
  // sensible initial banks per mapper (cart init code overrides via writes)
  for (int i = 0; i < 4; i++) c.bank8k[i] = (uint16_t)i;
  if (c.mapper == msx::MAP_KONAMI) c.bank8k[0] = 0;             // $4000 window fixed to bank 0
}
void msxCartEject(int slot) {
  if (slot < 1 || slot > 2) return;
  msx::g_cart[slot] = msx::Cart();
}
