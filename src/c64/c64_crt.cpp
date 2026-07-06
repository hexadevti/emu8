#include "../../emu.h"
#include "c64.h"

// C64 cartridge (.crt) loader with bank switching.
//
// Generic 8K/16K/Ultimax carts plus the common BANK-SWITCHING game carts (Ocean, Magic
// Desk, System 3 / C64GS, Fun Play...). Bank-switching carts are usually 128-512K - far too
// big for RAM - so we index every CHIP packet's file offset at load time and STREAM the
// selected 8K bank off the SD card into cartROML/cartROMH whenever the cart writes its
// banking register at $DE00. Only the current bank lives in RAM (8K + 8K).
//
// EasyFlash and other flash/RAM carts are not supported (no $DE02 control / flash emulation).

static uint16_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Index of each ROM block in the .crt file (so we can stream banks without holding them).
struct CartChip { uint32_t fileOff; uint16_t bank; uint16_t loadAddr; uint16_t size; };
#define CART_MAX_CHIPS 128   // 128 banks x 8K = up to 1MB cart (plenty); keeps BSS small
static CartChip cartChips[CART_MAX_CHIPS];
static int      cartChipCount = 0;
static String   cartPath;
static int      cartType = 0;       // .crt hardware type (0=generic, 5=Ocean, 19=Magic Desk...)
static int      cartCurBank = -1;
static File     cartFile;           // kept OPEN for fast bank streaming (re-opening per bank
                                    // took ~190ms each, stalling the CPU and crashing loaders)

#if defined(BOARD_DESKTOP)
// Desktop debug: the currently-mapped 8K bank (-1 = no cart) and the cart's total bank count, for the
// cartridge ROM-access map in the "Disk read" panel (src/desktop/ui_imgui.cpp).
int c64CartCurBank() { return c64::cartActive ? cartCurBank : -1; }
int c64CartBankCount() {
  if (!c64::cartActive) return 0;
  int mx = 0; for (int i = 0; i < cartChipCount; i++) if (cartChips[i].bank > mx) mx = cartChips[i].bank;
  return mx + 1;
}
#endif

void c64CartUnmount() {
  c64::cartActive = false;
  c64::cartExrom = c64::cartGame = true;   // both lines inactive (no cart)
  if (cartFile) cartFile.close();
  cartChipCount = 0;
  cartCurBank = -1;
  cartPath = "";
}

// Stream every CHIP packet belonging to `bank` from the (already-open) SD image into the ROM
// windows. The file handle stays open across bank switches so this is a quick seek+read.
static void loadCartBank(int bank) {
  if (bank == cartCurBank) return;          // already resident
  if (!cartFile) cartFile = FSTYPE.open(cartPath.c_str(), FILE_READ);
  if (!cartFile) { sprintf(buf, "cart: bank %d SD-OPEN FAILED", bank); printLog(buf); return; }
  for (int i = 0; i < cartChipCount; i++) {
    if (cartChips[i].bank != bank) continue;
    uint16_t la = cartChips[i].loadAddr, sz = cartChips[i].size;
    cartFile.seek(cartChips[i].fileOff);
    if (la >= 0x8000 && la <= 0x9fff) {       // ROML window ($8000)
      uint16_t n = sz > 0x2000 ? 0x2000 : sz;
      cartFile.read(c64::cartROML + (la - 0x8000), n);
      if (sz > 0x2000) cartFile.read(c64::cartROMH, sz - 0x2000);  // 16K chip spills into ROMH
    } else if (la >= 0xa000) {                // ROMH window ($A000 or $E000)
      uint16_t n = sz > 0x2000 ? 0x2000 : sz;
      cartFile.read(c64::cartROMH + (la & 0x1fff), n);
    }
  }
  cartCurBank = bank;
}

static uint8_t efRam[256];   // EasyFlash 256-byte RAM at $DF00-$DFFF

// EasyFlash helpers (cartType 32). EF has 64 banks of ROML($8000)+ROMH($A000), selects the
// bank via $DE00 and the EXROM/GAME mapping via $DE02, and exposes 256 bytes of RAM at $DF00.
// Boots in Ultimax (see c64LoadCRT). Read-only: flash WRITES (cart saving / EAPI programming)
// are NOT emulated, so in-cart saves won't persist, but games run.
bool c64CartIsEF() { return c64::cartActive && cartType == 32; }
unsigned char c64CartRamRead(uint16_t addr) { return efRam[addr & 0xff]; }
void c64CartRamWrite(uint16_t addr, uint8_t val) { efRam[addr & 0xff] = val; }

// Cartridge I/O-1 register write ($DE00-$DEFF). Called from write8 when a cart is mounted.
void c64CartBankWrite(uint16_t addr, uint8_t val) {
  if (!c64::cartActive) return;
  switch (cartType) {
    case 32:                                  // EasyFlash: A1=0 -> bank reg, A1=1 -> control reg
      if ((addr & 2) == 0) loadCartBank(val & 0x3f);         // $DE00/$DE01 bank register (0-63)
      else {                                                  // $DE02/$DE03 control register
        c64::cartExrom = !(val & 0x02);                      // bit1=1 -> /EXROM active (low)
        c64::cartGame  = (val & 0x04) ? !(val & 0x01) : false; // bit2(MODE): GAME from bit0, else low
      }
      break;
    case 19:                                  // Magic Desk: bit7 disables, bits0-5 = bank
      if (val & 0x80) { c64::cartExrom = true; }            // unmap ROML -> RAM at $8000
      else { c64::cartExrom = false; loadCartBank(val & 0x3f); }
      break;
    case 15:                                  // System 3 / C64GS: bank = low bits of address
      loadCartBank(addr & 0x3f);
      break;
    case 5:                                   // Ocean
    default:                                   // best-effort for other write-banked carts
      loadCartBank(val & 0x3f);
      break;
  }
}

bool c64LoadCRT(const char *path)
{
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "crt: cannot open %s", path); printLog(buf); return false; }

  uint8_t hdr[64];
  if (f.read(hdr, 64) != 64 || memcmp(hdr, "C64 CARTRIDGE   ", 16) != 0) {
    f.close(); printLog("crt: bad header"); return false;
  }
  uint32_t hdrLen = be32(hdr + 16);
  if (hdrLen < 64) hdrLen = 64;
  uint16_t hwType = be16(hdr + 22);
  uint8_t  exrom  = hdr[24];
  uint8_t  game   = hdr[25];

  if (!c64::cartROML) c64::cartROML = (uint8_t *)malloc(0x2000);
  if (!c64::cartROMH) c64::cartROMH = (uint8_t *)malloc(0x2000);
  if (!c64::cartROML || !c64::cartROMH) { f.close(); printLog("crt: out of memory"); return false; }
  memset(c64::cartROML, 0, 0x2000);
  memset(c64::cartROMH, 0, 0x2000);

  // Index the CHIP packets (record file offsets; don't load the data yet).
  if (cartFile) cartFile.close();        // drop any previous cart's open handle
  cartPath = path;
  cartType = hwType;
  cartChipCount = 0;
  cartCurBank = -1;
  f.seek(hdrLen);
  uint8_t ch[16];
  while (cartChipCount < CART_MAX_CHIPS) {
    uint32_t chipStart = f.position();
    if (f.read(ch, 16) != 16 || memcmp(ch, "CHIP", 4) != 0) break;
    uint32_t pktLen  = be32(ch + 4);
    if (pktLen < 16) pktLen = 16;
    cartChips[cartChipCount].fileOff  = chipStart + 16;
    cartChips[cartChipCount].bank     = be16(ch + 10);
    cartChips[cartChipCount].loadAddr = be16(ch + 12);
    cartChips[cartChipCount].size     = be16(ch + 14);
    cartChipCount++;
    f.seek(chipStart + pktLen);               // advance to the next packet
  }
  f.close();

  if (hwType == 32) {                          // EasyFlash boots in ULTIMAX mode: that's the
    c64::cartExrom = true;                      // $DE02=0 reset state (EXROM high, GAME low), so
    c64::cartGame  = false;                     // the CPU resets through the bank-0 ROMH vector
    memset(efRam, 0, sizeof(efRam));            // at $E000-$FFFF where the EF boot loader lives;
  } else {                                       // it then remaps banks/mode via $DE00/$DE02.
    c64::cartExrom = (exrom != 0);             // line HIGH (inactive) when byte != 0
    c64::cartGame  = (game  != 0);
  }
  c64::cartActive = true;
  loadCartBank(0);                             // map the reset bank
  c64::c64ResetReq = true;                     // reset so the KERNAL launches the cart

  const char *kind = (c64::cartExrom && !c64::cartGame) ? "Ultimax"
                   : (!c64::cartExrom && !c64::cartGame) ? "16K" : "8K";
  sprintf(buf, "crt: %s hwType=%u, %d bank-chips -> reset", kind, hwType, cartChipCount);
  printLog(buf);
  return true;
}
