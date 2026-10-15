// This SMS translation unit is compiled out entirely on boards that clear
// BOARD_HAS_SMS_CORE (currently none -- see board.h; it is on for every board).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro;
// the guard below then empties the file, keeping the Z80 core's static RAM out of the image.
#include "../../emu.h"
#if BOARD_HAS_SMS_CORE

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
#include "../z80/z80_snapshot.h"
#include <string.h>
#include <stdlib.h>

namespace sms {

static uint8_t  bankReg[3] = {0, 1, 2};   // $FFFD/$FFFE/$FFFF -> 16 KB ROM bank for slots 0/1/2
static uint8_t  ramCtrl    = 0;           // $FFFC: bit3 = cart-RAM at 0x8000, bit2 = which RAM bank
static int      numBanks   = 1;           // ROM size / 16 KB (rounded up)
static int      bankMask   = 0;           // numBanks-1 (used when bankPow2)
static bool     bankPow2   = true;        // false => non-power-of-two bank count, use modulo
// Up to 32 KB of on-cart battery RAM (Phantasy Star etc.). Heap, and only once a game first maps it
// ($FFFC bit3): most carts never do, and as a static it was 32 KB the PicoCalc's RP2040 could not
// spare (board.h). Kept for the rest of the session once allocated; if the heap cannot supply it,
// the window reads open bus and drops writes rather than stopping the game.
static const int CART_RAM_SIZE = 0x8000;
static uint8_t* cartRam = nullptr;

static uint32_t slotBase[3] = {0, 0x4000, 0x8000};   // bankReg[] resolved to ROM offsets (per write)

// The same mapping as cartRead, resolved to one pointer per 8 KB of CPU space for the Z80 bus in
// sms_machine.cpp. Kept current by updatePages() on every mapper write and image change; nullptr
// means "ask cartRead" (the 1 KB fixed vector area while slot 0 is paged, cart RAM not allocated,
// or a bank past the end of the image).
const uint8_t* cartRdPage[6];

static void updatePages() {
  for (int k = 0; k < 6; k++) {
    const int slot = k >> 1;
    const uint32_t half = (uint32_t)(k & 1) * 0x2000;
    const uint8_t* p = nullptr;
    if (slot == 2 && (ramCtrl & 0x08)) {
      if (cartRam) p = cartRam + ((ramCtrl & 0x04) ? 0x4000 : 0) + half;
    } else if (k != 0 || slotBase[0] == 0) {
      uint32_t off = slotBase[slot] + half;
      if (rom && off + 0x2000 <= (uint32_t)romLen) p = rom + off;
    }
    cartRdPage[k] = p;
  }
}

static inline uint32_t bankBase(int slot) {
  int b = bankReg[slot];
  b = bankPow2 ? (b & bankMask) : (b % numBanks);
  return (uint32_t)b * 0x4000;
}
static void updateSlots() { for (int s = 0; s < 3; s++) slotBase[s] = bankBase(s); updatePages(); }

void cartSetImage(const uint8_t* data, int len) {
  rom = (uint8_t*)data; romLen = len;
  numBanks = (len + 0x3FFF) / 0x4000; if (numBanks < 1) numBanks = 1;
  bankPow2 = (numBanks & (numBanks - 1)) == 0;
  bankMask = numBanks - 1;
  updateSlots();
}

void cartReset() {
  bankReg[0] = 0; bankReg[1] = 1; bankReg[2] = 2;   // de-facto power-on banks (games set them anyway)
  ramCtrl = 0;
  if (cartRam) memset(cartRam, 0, CART_RAM_SIZE);
  updateSlots();
}

// ---- save state (sms_snapshot.cpp) -------------------------------------------------------------
// The ROM is not saved (flash / PSRAM); the cart RAM is streamed by sms_snapshot.cpp through
// cartRamData(). cartRamEnsure() is the same lazy allocation a $FFFC write does, for loading a
// snapshot that has cart RAM into a session that has not mapped it yet. The "MAPR" record holds the
// three bank registers and $FFFC; the window offsets and page pointers are rebuilt from them.
uint8_t* cartRamData() { return cartRam; }
int      cartRamSize() { return CART_RAM_SIZE; }
bool cartRamEnsure() {
  if (!cartRam && (cartRam = (uint8_t*)malloc(CART_RAM_SIZE))) memset(cartRam, 0, CART_RAM_SIZE);
  return cartRam != nullptr;
}
void cartSnapshot(SnapIO &io) {
  io.tag("MAPR");
  io.arr(bankReg);
  io.val(ramCtrl);
  if (io.loading) updateSlots();
}

uint8_t cartRead(uint16_t a) {
  if (a >= 0x8000 && a < 0xC000 && (ramCtrl & 0x08)) {        // cart RAM mapped over slot 2
    if (!cartRam) return 0xFF;
    int rb = (ramCtrl & 0x04) ? 1 : 0;
    return cartRam[rb * 0x4000 + (a - 0x8000)];
  }
  uint32_t off;
  if (a < 0x0400)       off = a;                              // first 1 KB fixed to bank 0
  else if (a < 0x4000)  off = slotBase[0] + a;                // slot 0
  else if (a < 0x8000)  off = slotBase[1] + (a - 0x4000);     // slot 1
  else                  off = slotBase[2] + (a - 0x8000);     // slot 2
  return (rom && off < (uint32_t)romLen) ? rom[off] : 0xFF;
}

void cartWrite(uint16_t a, uint8_t v) {
  switch (a) {                                                // Sega mapper registers (top of RAM)
    case 0xFFFC:
      ramCtrl = v;
      if ((v & 0x08) && !cartRam && (cartRam = (uint8_t*)malloc(CART_RAM_SIZE)))
        memset(cartRam, 0, CART_RAM_SIZE);
      updatePages();
      return;
    case 0xFFFD: bankReg[0] = v; slotBase[0] = bankBase(0); updatePages(); return;
    case 0xFFFE: bankReg[1] = v; slotBase[1] = bankBase(1); updatePages(); return;
    case 0xFFFF: bankReg[2] = v; slotBase[2] = bankBase(2); updatePages(); return;
  }
  if (a >= 0x8000 && a < 0xC000 && (ramCtrl & 0x08) && cartRam) {   // write to mapped cart RAM
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

#endif  // BOARD_HAS_SMS_CORE
