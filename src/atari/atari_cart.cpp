#include "../../emu.h"
#include "atari.h"
#include "../shared/filebrowser.h"   // shared SD image browser (subdirectories + sorting)

// Atari 2600 cartridge loader. ROMs are raw images (no header) identified purely by size:
//   2K / 4K  -> not bank-switched (2K mirrors into the 4K window)
//   8K       -> F8  (2 banks, hotspots $1FF8/$1FF9)
//   16K      -> F6  (4 banks, hotspots $1FF6-$1FF9)
//   32K      -> F4  (8 banks, hotspots $1FF4-$1FFB)
// Optional Superchip (+128 bytes RAM, write $1000-$107F / read $1080-$10FF) is auto-detected on
// 8K+ carts when the first 256 bytes are a constant fill (the usual zeroed RAM area in the image).
// The whole ROM (<=32K) is held in RAM — no SD streaming (unlike the NES, 2600 carts are tiny).

namespace atari {

enum CartType { CART_RAW, CART_F8, CART_F6, CART_F4 };
static CartType cartType = CART_RAW;
static int      cartBank = 0;
static bool     hasSC    = false;       // Superchip 128B RAM present
static uint8_t *scRam    = nullptr;     // 128 bytes, malloc'd on first SC cart (not static BSS)

static uint32_t cartRawMask = 0x0FFF;   // address mask for non-banked carts (0x7FF for 2K)

static bool endsWithCI(const std::string &name, const char *ext) {
  size_t n = strlen(ext);
  if (name.size() < n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower(name[name.size() - n + i]) != tolower(ext[i])) return false;
  return true;
}

// Append a short "<rom>: <reason>" line to the on-screen startup warning.
static void loadWarnAdd(const char *path, const char *reason) {
  if (!loadWarn) return;
  const char *name = (*path == '/') ? path + 1 : path;
  size_t cur = strlen(loadWarn);
  if (cur > 200) return;
  char nm[28]; strncpy(nm, name, 26); nm[26] = 0;
  char line[64];
  snprintf(line, sizeof(line), "%s: %s\n", nm, reason);
  strncat(loadWarn, line, 255 - cur);
}

static void freeCart() {
  if (cartRom) { free(cartRom); cartRom = nullptr; }
  cartSize = 0; cartType = CART_RAW; cartBank = 0; cartRawMask = 0x0FFF; hasSC = false;
}

// A cartridge whose first 256 bytes are a constant fill almost always reserves that region for
// Superchip RAM (the image ships it zeroed). Only consulted for 8K+ carts to avoid false positives.
static bool detectSC() {
  uint8_t b = cartRom[0];
  for (int i = 1; i < 256; i++) if (cartRom[i] != b) return false;
  return true;
}

static inline void checkHotspot(uint16_t a) {
  switch (cartType) {
    case CART_F8: if (a == 0xFF8) cartBank = 0; else if (a == 0xFF9) cartBank = 1; break;
    case CART_F6: if (a >= 0xFF6 && a <= 0xFF9) cartBank = a - 0xFF6; break;
    case CART_F4: if (a >= 0xFF4 && a <= 0xFFB) cartBank = a - 0xFF4; break;
    default: break;
  }
}

uint8_t cartRead(uint16_t addr) {
  uint16_t a = addr & 0x0FFF;
  if (hasSC && a >= 0x80 && a < 0x100) return scRam[a & 0x7F];   // Superchip read port $1080-$10FF
  checkHotspot(a);                                              // bank switch on any access
  if (cartType == CART_RAW) return cartRom[a & cartRawMask];
  return cartRom[(uint32_t)cartBank * 4096 + a];
}

void cartWrite(uint16_t addr, uint8_t val) {
  uint16_t a = addr & 0x0FFF;
  if (hasSC && a < 0x80) { scRam[a] = val; return; }            // Superchip write port $1000-$107F
  checkHotspot(a);
}

bool atariLoadROM(const char *path) {
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "Atari: cannot open %s", path); printLog(buf); return false; }

  uint32_t sz = f.size();
  if (sz != 2048 && sz != 4096 && sz != 8192 && sz != 16384 && sz != 32768) {
    f.close();
    sprintf(buf, "Atari: %s size %u unsupported (need 2/4/8/16/32K)", path, (unsigned)sz);
    printLog(buf);
    char r[40]; snprintf(r, sizeof(r), "size %uK unsupported", (unsigned)(sz / 1024));
    loadWarnAdd(path, r);
    return false;
  }

  freeCart();
  cartRom = (uint8_t *)malloc(sz);
  if (!cartRom) { f.close(); printLog("Atari: ROM malloc failed"); return false; }
  if (f.read(cartRom, sz) != (int)sz) { f.close(); printLog("Atari: ROM read short"); freeCart(); return false; }
  f.close();

  cartSize = sz;
  cartBank = 0;
  switch (sz) {
    case 2048:  cartType = CART_RAW; cartRawMask = 0x07FF; break;
    case 4096:  cartType = CART_RAW; cartRawMask = 0x0FFF; break;
    case 8192:  cartType = CART_F8;  break;
    case 16384: cartType = CART_F6;  break;
    case 32768: cartType = CART_F4;  break;
  }
  hasSC = (sz >= 8192) && detectSC();
  if (hasSC) {
    if (!scRam) scRam = (uint8_t *)malloc(128);
    if (scRam) memset(scRam, 0, 128); else hasSC = false;   // no RAM -> fall back to plain banking
  }

  ::selectedAtariFileName = path;
  const char *tn = (cartType == CART_RAW) ? (sz == 2048 ? "2K" : "4K")
                 : (cartType == CART_F8) ? "F8/8K" : (cartType == CART_F6) ? "F6/16K" : "F4/32K";
  sprintf(buf, "Atari: loaded %s (%s%s)", path, tn, hasSC ? " +SC" : "");
  printLog(buf);
  return true;
}

// Scan the SD root for *.a26 / *.bin files into the global atariFiles list (full paths).
#define ATARI_MAX_FILES 200
// SD ROM browser (.a26/.bin + subdirectories); see src/shared/filebrowser.h.
static bool atariAccept(const std::string &n) {
  return endsWithCI(n, ".a26") || endsWithCI(n, ".bin");
}
static FileBrowser atariBrowser = { "Atari", &atariFiles, atariAccept, nullptr, ATARI_MAX_FILES, "/" };

void loadAtariFilesSync()      { fbScan(atariBrowser); }
void browseEnter(const char *path) { fbEnter(atariBrowser, path); }
void browseUp()                { fbUp(atariBrowser); }

bool atariLoadFirstRom() {
  if (loadWarn) loadWarn[0] = 0;
  if (atariFiles.empty()) loadAtariFilesSync();
  if (atariFiles.empty()) {
    printLog("Atari: no .a26/.bin on SD root");
    if (loadWarn) strcpy(loadWarn, "No .a26/.bin ROMs found on the SD card\n");
    return false;
  }
  // Boot back into the last-loaded ROM (persisted in EEPROM by the settings page), if it still loads.
  if (::selectedAtariFileName.length() > 1) {
    sprintf(buf, "Atari: autoloading last ROM %s", ::selectedAtariFileName.c_str());
    printLog(buf);
    if (atariLoadROM(::selectedAtariFileName.c_str())) return true;
    printLog("Atari: autoload failed -> scanning SD");
  }
  for (size_t i = 0; i < atariFiles.size(); i++)
    if (atariLoadROM(atariFiles[i].c_str())) return true;
  printLog("Atari: no loadable ROM on SD root");
  if (loadWarn) strncat(loadWarn, "-> no ROM could be loaded\n", 255 - strlen(loadWarn));
  return false;
}

} // namespace atari
