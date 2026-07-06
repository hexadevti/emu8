// sms_cart.cpp - Sega Master System cartridge mapping (the standard "Sega" mapper).
//
// CPU address space 0x0000-0xBFFF is three 16 KB ROM windows plus an optional 16 KB cart-RAM window:
//   0x0000-0x03FF : ALWAYS ROM bank 0 (the interrupt vectors are never paged out)
//   0x0400-0x3FFF : ROM slot 0  (bank register $FFFD)
//   0x4000-0x7FFF : ROM slot 1  (bank register $FFFE)
//   0x8000-0xBFFF : ROM slot 2  (bank register $FFFF) OR on-cart RAM when $FFFC bit3 is set
// The four mapper registers live at 0xFFFC-0xFFFF (they are also normal work-RAM bytes - the write
// goes to both, handled in sms_machine.cpp). The ROM image stays fully resident; banking just repoints
// the per-16 KB window offsets, like the NES/MSX mapper windows.
//
// Codemasters and other mappers are a later addition; this file is structured so cartWrite() can grow
// a mapper-type switch. For now plain (<=48 KB linear) and Sega-banked ROMs both work.

#include "sms.h"
#include "sms_cart.h"
#include <string.h>

namespace sms {

static uint8_t  bankReg[3] = {0, 1, 2};   // $FFFD/$FFFE/$FFFF -> 16 KB ROM bank for slots 0/1/2
static uint8_t  ramCtrl    = 0;           // $FFFC: bit3 = cart-RAM at 0x8000, bit2 = which RAM bank
static int      numBanks   = 1;           // ROM size / 16 KB (rounded up)
static int      bankMask   = 0;           // numBanks-1 when power of two, else 0 (use modulo)
static uint8_t  cartRam[0x8000];          // up to 32 KB on-cart battery RAM (Phantasy Star etc.)

void cartSetImage(const uint8_t* data, int len) {
  rom = (uint8_t*)data; romLen = len;
  numBanks = (len + 0x3FFF) / 0x4000; if (numBanks < 1) numBanks = 1;
  bankMask = ((numBanks & (numBanks - 1)) == 0) ? (numBanks - 1) : 0;   // 0 => non-power-of-two, use %
}

void cartReset() {
  bankReg[0] = 0; bankReg[1] = 1; bankReg[2] = 2;   // de-facto power-on banks (games set them anyway)
  ramCtrl = 0;
  memset(cartRam, 0, sizeof(cartRam));
}

static inline uint32_t bankBase(int slot) {
  int b = bankReg[slot];
  b = bankMask ? (b & bankMask) : (b % numBanks);
  return (uint32_t)b * 0x4000;
}

uint8_t cartRead(uint16_t a) {
  if (a >= 0x8000 && a < 0xC000 && (ramCtrl & 0x08)) {        // cart RAM mapped over slot 2
    int rb = (ramCtrl & 0x04) ? 1 : 0;
    return cartRam[rb * 0x4000 + (a - 0x8000)];
  }
  uint32_t off;
  if (a < 0x0400)       off = a;                              // first 1 KB fixed to bank 0
  else if (a < 0x4000)  off = bankBase(0) + a;                // slot 0
  else if (a < 0x8000)  off = bankBase(1) + (a - 0x4000);     // slot 1
  else                  off = bankBase(2) + (a - 0x8000);     // slot 2
  return (rom && off < (uint32_t)romLen) ? rom[off] : 0xFF;
}

void cartWrite(uint16_t a, uint8_t v) {
  switch (a) {                                                // Sega mapper registers (top of RAM)
    case 0xFFFC: ramCtrl    = v; return;
    case 0xFFFD: bankReg[0] = v; return;
    case 0xFFFE: bankReg[1] = v; return;
    case 0xFFFF: bankReg[2] = v; return;
  }
  if (a >= 0x8000 && a < 0xC000 && (ramCtrl & 0x08)) {        // write to mapped cart RAM
    int rb = (ramCtrl & 0x04) ? 1 : 0;
    cartRam[rb * 0x4000 + (a - 0x8000)] = v;
  }
  // writes to ROM area without cart RAM: ignored (ROM)
}

} // namespace sms

void smsCartLoadImage(const uint8_t* data, int len) {
  sms::cartSetImage(data, len);
  sms::cartReset();
}
