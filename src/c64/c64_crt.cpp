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
//
// PicoCalc (BOARD_ROM_IN_FLASH): the RP2040 heap left beside the 64K guest RAM cannot be counted
// on for the 16K of bank buffers, so the whole .crt is copied into the spare-flash cartridge window
// (src/picocalc/romflash_picocalc.cpp) instead, and cartROML/cartROMH just POINT into it. A bank
// switch is then two pointer stores -- no SD access from the CPU core mid-game either. The emulated
// machine never writes cart ROM, so the const-cast onto XIP flash is safe.

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
#if BOARD_ROM_IN_FLASH
static const uint8_t *cartFlash = nullptr;         // the .crt image in XIP flash
static const uint8_t  cartBlank[0x2000] = { 0 };   // a window the current bank leaves unmapped
#else
static File     cartFile;           // kept OPEN for fast bank streaming (re-opening per bank
                                    // took ~190ms each, stalling the CPU and crashing loaders)
#endif

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
#if BOARD_ROM_IN_FLASH
  cartFlash = nullptr;
#else
  if (cartFile) cartFile.close();
#endif
  cartChipCount = 0;
  cartCurBank = -1;
  cartPath = "";
}

// Stream every CHIP packet belonging to `bank` from the (already-open) SD image into the ROM
// windows. The file handle stays open across bank switches so this is a quick seek+read.
static void loadCartBank(int bank) {
  if (bank == cartCurBank) return;          // already resident
#if BOARD_ROM_IN_FLASH
  // Same mapping as the streaming path below, as pointers into the flash image. A chip that does
  // not start on its window's first byte (a 4K Ultimax chip at $F000) is pointed back by that
  // offset; whatever flash sits in front of it reads as open bus would (XIP is read-only).
  if (!cartFlash) return;
  for (int i = 0; i < cartChipCount; i++) {
    if (cartChips[i].bank != bank) continue;
    uint16_t la = cartChips[i].loadAddr, sz = cartChips[i].size;
    const uint8_t *p = cartFlash + cartChips[i].fileOff;
    if (la >= 0x8000 && la <= 0x9fff) {
      c64::cartROML = (uint8_t *)(p - (la - 0x8000));
      if (sz > 0x2000) c64::cartROMH = (uint8_t *)(p + 0x2000);   // 16K chip spills into ROMH
    } else if (la >= 0xa000) {
      c64::cartROMH = (uint8_t *)(p - (la & 0x1fff));
    }
  }
#else
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
#endif
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

// The hardware types c64CartBankWrite() actually emulates. Anything else has banking or I/O this
// loader does not know, so the browser greys it out rather than letting it crash on start.
static bool crtTypeSupported(uint16_t t) { return t == 0 || t == 5 || t == 15 || t == 19 || t == 32; }

static uint16_t crtLastErr = C64F_OK;
uint16_t c64CrtLastError() { return crtLastErr; }

// Header check shared by the browser (c64ProbeFiles) and c64LoadCRT: everything that can be said
// about a .crt without loading it. Leaves f positioned anywhere. Returns a C64F_* code, with the
// hardware type in the high byte for C64F_CRT_TYPE.
uint16_t c64CrtProbe(File &f)
{
  uint8_t hdr[64];
  if (f.read(hdr, 64) != 64 || memcmp(hdr, "C64 CARTRIDGE   ", 16) != 0) return C64F_CRT_HDR;
  uint16_t hwType = be16(hdr + 22);
  if (!crtTypeSupported(hwType)) return C64F_CRT_TYPE | ((hwType > 255 ? 255 : hwType) << 8);
  uint32_t hdrLen = be32(hdr + 16);
  if (hdrLen < 64) hdrLen = 64;
  uint8_t ch[4];
  if (!f.seek(hdrLen) || f.read(ch, 4) != 4 || memcmp(ch, "CHIP", 4) != 0) return C64F_CRT_EMPTY;
#if BOARD_ROM_IN_FLASH
  if ((uint32_t)f.size() > romFlashCapacity(ROMFLASH_CART)) return C64F_CRT_BIG;
#endif
  return C64F_OK;
}

static bool c64LoadCRTLocked(const char *path);

// Called from the settings UI (render task) and from the READY trap / boot autoload (CPU core), so
// the whole load holds gBusLock, and a failed heap allocation (the File open) is an error instead
// of an uncaught bad_alloc that stops the board.
bool c64LoadCRT(const char *path)
{
  const bool hadCart = c64::cartActive;
  bool ok = false;
  busTake();
  try { ok = c64LoadCRTLocked(path); }
  catch (const std::bad_alloc &) { crtLastErr = C64F_NOMEM; printLog("crt: out of heap"); }
  busGive();
  if (ok) {
    c64AutoloadPending = false;   // a .prg/.d64 still queued for READY must not load over the cart
  } else if (hadCart && !c64::cartActive) {
    // The old cart was unmounted before the new one failed: the CPU would resume inside cart code
    // that is gone. Reset into BASIC instead.
    c64::c64ResetReq = true;
    printLog("crt: previous cartridge removed -> reset to BASIC");
  }
  return ok;
}

static bool c64LoadCRTLocked(const char *path)
{
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { crtLastErr = C64F_OPEN; snprintf(buf, sizeof(buf), "crt: cannot open %.100s", path); printLog(buf); return false; }

  crtLastErr = c64CrtProbe(f);
  if (crtLastErr != C64F_OK) {
    f.close();
    snprintf(buf, sizeof(buf), "crt: %.80s: %s", path, c64FileProblemText(crtLastErr));
    printLog(buf);
    return false;
  }
  uint8_t hdr[64];
  f.seek(0);
  f.read(hdr, 64);
  uint32_t hdrLen = be32(hdr + 16);
  if (hdrLen < 64) hdrLen = 64;
  uint16_t hwType = be16(hdr + 22);
  uint8_t  exrom  = hdr[24];
  uint8_t  game   = hdr[25];

#if BOARD_ROM_IN_FLASH
  // Pull the old cart out first: its bytes in the flash window are about to be overwritten.
  c64CartUnmount();
  const uint32_t fileLen = f.size();
#else
  if (!c64::cartROML) c64::cartROML = (uint8_t *)malloc(0x2000);
  if (!c64::cartROMH) c64::cartROMH = (uint8_t *)malloc(0x2000);
  if (!c64::cartROML || !c64::cartROMH) { f.close(); crtLastErr = C64F_NOMEM; printLog("crt: out of memory"); return false; }
  memset(c64::cartROML, 0, 0x2000);
  memset(c64::cartROMH, 0, 0x2000);
  if (cartFile) cartFile.close();        // drop any previous cart's open handle
#endif

  // Index the CHIP packets (record file offsets; don't load the data yet).
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
#if BOARD_ROM_IN_FLASH
    // Mapped by pointer, so a truncated last chip must not reach past the image into other flash.
    if (chipStart + 16 + cartChips[cartChipCount].size > fileLen) break;
#endif
    cartChipCount++;
    f.seek(chipStart + pktLen);               // advance to the next packet
  }
#if BOARD_ROM_IN_FLASH
  f.seek(0);
  // Staging: the running C64 leaves under 1K of heap, so borrow 4K of its framebuffer. The
  // settings window has the panel while this runs, and the cart's reset redraws every line anyway.
  cartFlash = romFlashLoad(ROMFLASH_CART, f, fileLen, sharedBigBuf);   // only changed sectors are rewritten
  f.close();
  if (!cartFlash) { cartChipCount = 0; crtLastErr = C64F_FLASH; printLog("crt: copy to flash failed"); return false; }
  c64::cartROML = (uint8_t *)cartBlank;        // until bank 0 maps its chips over them
  c64::cartROMH = (uint8_t *)cartBlank;
#else
  f.close();
#endif
  if (cartChipCount == 0) { c64CartUnmount(); crtLastErr = C64F_CRT_EMPTY; printLog("crt: no ROM chips"); return false; }

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
