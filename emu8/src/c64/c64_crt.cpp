#include "../../emu.h"
#include "c64.h"
#include "../shared/snapshot.h"

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
static int16_t  cartRomlChip = -1;  // the chip ROML / ROMH were last filled from (-1: none), for
static int16_t  cartRomhChip = -1;  // snapshots: a bank that maps one window leaves the other as was
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
  cartRomlChip = cartRomhChip = -1;
  cartPath = "";
}

bool c64CartIsPath(const char *path) {
  return c64::cartActive && path && cartPath.length() && strcmp(cartPath.c_str(), path) == 0;
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
      cartRomlChip = i;
      if (sz > 0x2000) { c64::cartROMH = (uint8_t *)(p + 0x2000); cartRomhChip = i; }   // 16K chip spills into ROMH
    } else if (la >= 0xa000) {
      c64::cartROMH = (uint8_t *)(p - (la & 0x1fff));
      cartRomhChip = i;
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
      cartRomlChip = i;
      if (sz > 0x2000) { cartFile.read(c64::cartROMH, sz - 0x2000); cartRomhChip = i; }  // 16K chip spills into ROMH
    } else if (la >= 0xa000) {                // ROMH window ($A000 or $E000)
      uint16_t n = sz > 0x2000 ? 0x2000 : sz;
      cartFile.read(c64::cartROMH + (la & 0x1fff), n);
      cartRomhChip = i;
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
  cartRomlChip = cartRomhChip = -1;
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

// ---- Snapshots (c64_snapshot.cpp) ----
// A snapshot names the cartridge it was made with and holds its banking state, not its ROM: the
// same .crt has to be mounted before loading it. Identity: the hardware type, the CHIP table
// (offsets / banks / load addresses / sizes) and samples of the ROM bytes -- of every chip where
// the whole image is in flash (PicoCalc), of the mapped bank's chips everywhere. A sample another
// build cannot take is 0 and is not compared.
struct CartSnapId {
  uint8_t  active;
  uint16_t type, chips;
  uint32_t table;          // FNV-1a of the CHIP table
  uint32_t image;          // FNV-1a of each chip's first 64 bytes (flash builds; 0 = not taken)
  int16_t  bank;           // the mapped bank
  uint32_t bankSample;     // FNV-1a of that bank's chips' first 64 bytes, read through the windows
};

static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n) {
  while (n--) h = (h ^ *p++) * 16777619u;
  return h;
}
static uint32_t fnvU32(uint32_t h, uint32_t v) {
  const uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
  return fnv(h, b, 4);
}

// Where chip i's first byte sits in the ROML / ROMH window (nullptr: not in one); `room` is what
// is left of the window from there.
static const uint8_t *cartChipInWindow(int i, uint16_t &room) {
  const uint16_t la = cartChips[i].loadAddr;
  uint16_t off;
  const uint8_t *w;
  if (la >= 0x8000 && la <= 0x9fff) { off = la - 0x8000; w = c64::cartROML; }
  else if (la >= 0xa000)            { off = la & 0x1fff;  w = c64::cartROMH; }
  else return nullptr;
  room = 0x2000 - off;
  return w ? w + off : nullptr;
}

static CartSnapId cartSnapId() {
  CartSnapId id = {};
  id.bank = -1;
  if (!c64::cartActive) return id;
  id.active = 1;
  id.type = (uint16_t)cartType;
  id.chips = (uint16_t)cartChipCount;
  id.table = 2166136261u;
  for (int i = 0; i < cartChipCount; i++) {
    const CartChip &c = cartChips[i];
    id.table = fnvU32(id.table, c.fileOff);
    id.table = fnvU32(id.table, ((uint32_t)c.bank << 16) | c.loadAddr);
    id.table = fnvU32(id.table, c.size);
  }
#if BOARD_ROM_IN_FLASH
  if (cartFlash) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < cartChipCount; i++)
      h = fnv(h, cartFlash + cartChips[i].fileOff, cartChips[i].size < 64 ? cartChips[i].size : 64);
    id.image = h ? h : 1;
  }
#endif
  id.bank = (int16_t)cartCurBank;
  uint32_t h = 2166136261u;
  for (int i = 0; i < cartChipCount; i++) {
    if (cartChips[i].bank != cartCurBank) continue;
    uint16_t room = 0;
    const uint8_t *p = cartChipInWindow(i, room);
    if (!p) continue;
    uint16_t n = cartChips[i].size < 64 ? cartChips[i].size : 64;
    h = fnv(h, p, n < room ? n : room);
  }
  id.bankSample = h ? h : 1;
  return id;
}

// First thing in the C64 stream. Loading: reads the saved identity into locals and rejects a
// snapshot made with another cartridge (or with / without one) before anything is changed.
bool c64CartSnapCheck(SnapIO &io) {
  const CartSnapId cur = cartSnapId();
  CartSnapId s = cur;
  io.val(s.active); io.val(s.type); io.val(s.chips); io.val(s.table);
  io.val(s.image); io.val(s.bank); io.val(s.bankSample);
  if (!io.ok) return false;
  if (!io.loading) return true;
  if (s.active != cur.active) {
    io.reject(s.active ? "Mount the snapshot's cartridge first" : "Remove the cartridge first");
    return false;
  }
  if (!s.active) return true;
  if (s.type != cur.type || s.chips != cur.chips || s.table != cur.table ||
      (s.image && cur.image && s.image != cur.image) ||
      (s.bank == cur.bank && s.bankSample != cur.bankSample)) {
    io.reject("Snapshot is for another cartridge");
    return false;
  }
  return true;
}

// Point (flash) or fill (RAM buffers) one window from chip i, as loadCartBank does. False when the
// RAM-buffer build has no open image to read it from.
static bool cartSnapMap(int i, bool high) {
  if (i < 0 || i >= cartChipCount) {
#if BOARD_ROM_IN_FLASH
    (high ? c64::cartROMH : c64::cartROML) = (uint8_t *)cartBlank;
#endif
    (high ? cartRomhChip : cartRomlChip) = -1;
    return true;
  }
  const uint16_t la = cartChips[i].loadAddr, sz = cartChips[i].size;
  const bool lowChip = la >= 0x8000 && la <= 0x9fff;
#if BOARD_ROM_IN_FLASH
  (void)sz;
  if (!cartFlash) return false;
  const uint8_t *p = cartFlash + cartChips[i].fileOff;
  if (!high)        c64::cartROML = (uint8_t *)(p - (la - 0x8000));
  else if (lowChip) c64::cartROMH = (uint8_t *)(p + 0x2000);
  else              c64::cartROMH = (uint8_t *)(p - (la & 0x1fff));
#else
  // Only reached for a snapshot taken on a flash build (no windows in it): the caller holds the SD
  // bus, and this is the open image the CPU core streams its banks from.
  if (!cartFile || !c64::cartROML || !c64::cartROMH) return false;
  uint32_t from = cartChips[i].fileOff;
  uint8_t *const base = high ? c64::cartROMH : c64::cartROML;
  uint8_t *to;
  uint32_t n;
  if (!high)        { to = base + (la - 0x8000); n = sz; }
  else if (lowChip) { to = base; from += 0x2000; n = sz > 0x2000 ? sz - 0x2000 : 0; }
  else              { to = base + (la & 0x1fff); n = sz; }
  if (n > (uint32_t)(base + 0x2000 - to)) n = (uint32_t)(base + 0x2000 - to);
  if (!cartFile.seek(from) || (uint32_t)cartFile.read(to, n) != n) return false;
#endif
  (high ? cartRomhChip : cartRomlChip) = (int16_t)i;
  return true;
}

// The cartridge's live state: EXROM/GAME, the mapped bank and the chip each window was filled from,
// the EasyFlash RAM, and -- on builds that copy banks into RAM -- the two 8K windows themselves.
void c64CartSnapState(SnapIO &io) {
  if (!c64::cartActive) return;               // c64CartSnapCheck: the same on both sides
  bool exrom = c64::cartExrom, game = c64::cartGame;
  int16_t bank = (int16_t)cartCurBank, roml = cartRomlChip, romh = cartRomhChip;
  io.flag(exrom); io.flag(game);
  io.val(bank); io.val(roml); io.val(romh);
  io.arr(efRam);
  uint8_t windows = BOARD_ROM_IN_FLASH ? 0 : 1;
  io.val(windows);
  if (windows) {
#if BOARD_ROM_IN_FLASH
    if (io.loading) {                         // a RAM-buffer build's windows: not needed here
      uint8_t skip[64];
      for (int n = 0; n < 0x4000 && io.ok; n += sizeof(skip)) io.bytes(skip, sizeof(skip));
    }
#else
    io.bytes(c64::cartROML, 0x2000);
    io.bytes(c64::cartROMH, 0x2000);
#endif
  }
  if (!io.loading || !io.ok) return;
  c64::cartExrom = exrom;
  c64::cartGame = game;
  cartCurBank = bank;
  if (windows && !BOARD_ROM_IN_FLASH) {       // the bytes came with the snapshot
    cartRomlChip = roml;
    cartRomhChip = romh;
  } else if (!cartSnapMap(roml, false) || !cartSnapMap(romh, true)) {
    io.fail("Cartridge image not available");
  }
}
