#include "../../emu.h"
#include "nes.h"
#include <dirent.h>   // raw POSIX directory enumeration (fast browser scan; see loadC64FilesSync)

// iNES (.nes) cartridge loader. Header (16 bytes):
//   [0..3] "NES\x1A"   [4] PRG size /16K   [5] CHR size /8K
//   [6] flags6: bit0 mirroring (0=horizontal,1=vertical), bit2 trainer, bit3 four-screen,
//               bits4-7 mapper low nibble
//   [7] flags7: bits4-7 mapper high nibble
// Then optional 512-byte trainer, PRG ROM, CHR ROM. The WHOLE PRG/CHR is loaded into RAM and
// the mapper layer (nes_mapper.cpp) banks it. Supported: 0 NROM, 1 MMC1, 2 UxROM, 3 CNROM.
// Big ROMs that don't fit the heap are refused with a log (no PSRAM; same wall as the C64 carts).

namespace nes {

#define NES_MAX_MAPPER 4          // highest mapper number nes_mapper.cpp implements (0/1/2/3/4)
#define NES_HEAP_MARGIN 10240     // keep this much heap free after loading the cart

static bool endsWithCI(const std::string &name, const char *ext) {
  size_t n = strlen(ext);
  if (name.size() < n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower(name[name.size() - n + i]) != tolower(ext[i])) return false;
  return true;
}

// ---- PRG bank streaming (for ROMs whose full PRG won't fit RAM; CHR stays fully resident) ----
// A 4 x 8K cache (one slot per PRG window) is fed from the .nes on SD as the mapper switches
// banks. Works because PRG bank switches are infrequent (per scene/level), unlike CHR (per
// scanline) which therefore MUST stay in RAM. The .nes handle is kept open across switches.
static bool     prgStreamed    = false;
static uint8_t *prgCache       = nullptr;      // streaming bank cache: prgSlots x 8K (contiguous)
static int       prgSlots      = 0;            // number of 8K cache slots (largest that fits)
static int16_t  *prgSlotBank   = nullptr;      // malloc'd [prgSlots]: 8K bank held in each slot
static uint16_t *prgSlotUse    = nullptr;      // malloc'd [prgSlots]: LRU timestamps
static int16_t   prgMapBank[4] = {-1,-1,-1,-1}; // bank each PRG window maps now (eviction-protected)
static uint16_t prgUseClock    = 0;
static File     prgFile;                       // persistent .nes handle for streaming reads
static uint32_t prgDataOffset  = 0;            // file offset of PRG data (16 [+512 trainer])
static uint32_t cartHeapBytes  = 0;            // heap the loaded cart owns (so the guard can plan)

// Free any previously-loaded cartridge buffers (so re-loading doesn't leak).
static void freeCart() {
  if (prgRom)   { free(prgRom);   prgRom = nullptr; }
  if (chrData)  { free(chrData);  chrData = nullptr; }
  if (prgRam)   { free(prgRam);   prgRam = nullptr; }
  if (prgCache)    { free(prgCache);    prgCache = nullptr; }
  if (prgSlotBank) { free(prgSlotBank); prgSlotBank = nullptr; }
  if (prgSlotUse)  { free(prgSlotUse);  prgSlotUse = nullptr; }
  if (prgStreamed) { prgFile.close(); prgStreamed = false; }
  prgSlots = 0; prgUseClock = 0;
  for (int i = 0; i < 4; i++) prgMapBank[i] = -1;
  prgRomSize = 0; chrSize = 0; chrIsRam = false; cartHeapBytes = 0;
  for (int i = 0; i < 4; i++) prgMap[i] = nullptr;
  for (int i = 0; i < 8; i++) chrMap[i] = nullptr;
}

// Resolve an 8K PRG bank to a RAM pointer: a contiguous prgRom slice (resident, small ROMs), or
// the multi-slot streaming cache. The cache keeps several recently-used banks resident (LRU), so a
// game whose working set fits the cache rarely hits SD — crucial for UxROM, which swaps the $8000
// bank constantly. Eviction never touches a bank another window currently maps (those stay valid).
uint8_t *prgBankResolve(int window, int bank8k) {
  if (!prgStreamed) return prgRom + (uint32_t)bank8k * 8192;   // resident contiguous
  window &= 3;
  for (int s = 0; s < prgSlots; s++)                            // cache hit?
    if (prgSlotBank[s] == bank8k) {
      prgSlotUse[s] = ++prgUseClock; prgMapBank[window] = (int16_t)bank8k;
      return prgCache + (uint32_t)s * 8192;
    }
  int victim = -1; uint16_t best = 0xFFFF;                      // miss: evict LRU unprotected slot
  for (int s = 0; s < prgSlots; s++) {
    int16_t sb = prgSlotBank[s];
    bool prot = false;
    for (int w = 0; w < 4; w++) if (w != window && prgMapBank[w] == sb && sb >= 0) { prot = true; break; }
    if (prot) continue;
    if (prgSlotUse[s] <= best) { best = prgSlotUse[s]; victim = s; }
  }
  if (victim < 0) victim = 0;                                   // (only if prgSlots<=4; degrades ok)
  prgFile.seek(prgDataOffset + (uint32_t)bank8k * 8192);
  prgFile.read(prgCache + (uint32_t)victim * 8192, 8192);
  prgSlotBank[victim] = (int16_t)bank8k;
  prgSlotUse[victim]  = ++prgUseClock;
  prgMapBank[window]  = (int16_t)bank8k;
  return prgCache + (uint32_t)victim * 8192;
}

// Append a short "<rom>: <reason>" line to the on-screen startup warning (kept narrow to fit).
static void loadWarnAdd(const char *path, const char *reason) {
  const char *name = (*path == '/') ? path + 1 : path;
  size_t cur = strlen(loadWarn);
  if (cur > 200) return;
  char nm[28]; strncpy(nm, name, 26); nm[26] = 0;
  char line[64];
  snprintf(line, sizeof(line), "%s: %s\n", nm, reason);
  strncat(loadWarn, line, sizeof(loadWarn) - 1 - cur);
}

bool nesLoadROM(const char *path) {
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "NES: cannot open %s", path); printLog(buf); return false; }

  uint8_t hdr[16];
  if (f.read(hdr, 16) != 16 ||
      hdr[0] != 'N' || hdr[1] != 'E' || hdr[2] != 'S' || hdr[3] != 0x1A) {
    f.close(); sprintf(buf, "NES: %s not an iNES file", path); printLog(buf); return false;
  }

  int prgBanks = hdr[4];                       // x16K
  int chrBanks = hdr[5];                       // x8K
  uint8_t flags6 = hdr[6], flags7 = hdr[7];
  int mapper = (flags6 >> 4) | (flags7 & 0xF0);
  bool hasTrainer = flags6 & 0x04;

  if (mapper > NES_MAX_MAPPER) {
    f.close();
    sprintf(buf, "NES: %s uses mapper %d (supported: 0/1/2/3/4)", path, mapper);
    printLog(buf);
    char r[40]; snprintf(r, sizeof(r), "mapper %d not supported", mapper);
    loadWarnAdd(path, r);
    return false;
  }

  uint32_t prgSize = (uint32_t)prgBanks * 16384;
  uint32_t chrAlloc = chrBanks ? (uint32_t)chrBanks * 8192 : 8192;   // CHR-RAM = 8K when 0 banks
  if (prgSize == 0) {
    f.close(); sprintf(buf, "NES: %s has 0 PRG banks", path); printLog(buf); return false;
  }
  if (mapper == 0 && prgSize != 16384 && prgSize != 32768) {
    f.close(); sprintf(buf, "NES: bad NROM PRG size %u", (unsigned)prgSize); printLog(buf); return false;
  }

  uint32_t dataOffset = hasTrainer ? (16 + 512) : 16;

  // Heap guard (no PSRAM). CHR must be fully resident (per-scanline access can't stream); PRG is
  // held resident (whole ROM in RAM -> no run-time SD reads), or streamed via a 4x8K cache only
  // when the whole PRG won't fit. `avail` adds back the heap the CURRENT cart owns, since loading
  // frees it first — without that, switching games from the menu under-counts the free heap and
  // wrongly streams a ROM that actually fits.
  uint32_t fullNeed   = prgSize + chrAlloc + 8192;
  uint32_t streamNeed = 4u * 8192 + chrAlloc + 8192;   // 32K PRG cache + CHR + PRG-RAM
  uint32_t avail = ESP.getFreeHeap() + cartHeapBytes;
  bool useStream;
  if      (fullNeed   + NES_HEAP_MARGIN <= avail) useStream = false;
  else if (streamNeed + NES_HEAP_MARGIN <= avail) useStream = true;
  else {
    f.close();
    sprintf(buf, "NES: %s too big (full %uK / stream %uK, avail %uK)", path,
            (unsigned)(fullNeed / 1024), (unsigned)(streamNeed / 1024), (unsigned)(avail / 1024));
    printLog(buf);
    char r[48]; snprintf(r, sizeof(r), "too big (CHR %uK won't fit)", (unsigned)(chrAlloc / 1024));
    loadWarnAdd(path, r);
    return false;
  }

  freeCart();
  mirrorMode = (flags6 & 0x01) ? MIRROR_VERTICAL : MIRROR_HORIZONTAL;
  mapperNum  = (uint8_t)mapper;
  prgRomSize = prgSize;                          // full size — bank-count math (both modes)

  // CHR — always fully resident in RAM.
  chrData = (uint8_t *)malloc(chrAlloc);
  if (!chrData) { f.close(); printLog("NES: CHR malloc failed"); freeCart(); return false; }
  chrSize = chrAlloc;
  if (chrBanks == 0) {                          // CHR-RAM
    memset(chrData, 0, chrAlloc);
    chrIsRam = true;
  } else {
    f.seek(dataOffset + prgSize);
    if (f.read(chrData, chrAlloc) != (int)chrAlloc) { f.close(); printLog("NES: CHR read short"); freeCart(); return false; }
    chrIsRam = false;
  }

  // PRG — resident as one contiguous block when one exists (small ROMs); otherwise streamed from
  // SD through the multi-slot LRU cache. (The fragmented heap has no 128K+ block, and not even
  // enough 8K pieces, so 128K games can't be fully resident — they stream.)
  if (!useStream) {
    prgRom = (uint8_t *)malloc(prgSize);
    if (prgRom) {
      f.seek(dataOffset);
      if (f.read(prgRom, prgSize) != (int)prgSize) { f.close(); printLog("NES: PRG read short"); freeCart(); return false; }
    } else {
      sprintf(buf, "NES: PRG %uK has no contiguous block -> streaming", (unsigned)(prgSize / 1024));
      printLog(buf);
      useStream = true;
    }
  }
  if (useStream) {
    int maxSlots = (int)(prgSize / 8192);        // never need more slots than the ROM has banks
    if (maxSlots > 16) maxSlots = 16;
    for (int n = maxSlots; n >= 4; n--) {        // grab the largest cache that fits (more = less SD)
      prgCache = (uint8_t *)malloc((size_t)n * 8192);
      if (prgCache) { prgSlots = n; break; }
    }
    if (!prgCache) { f.close(); printLog("NES: PRG cache malloc failed"); freeCart(); return false; }
    prgSlotBank = (int16_t *)malloc((size_t)prgSlots * sizeof(int16_t));
    prgSlotUse  = (uint16_t *)malloc((size_t)prgSlots * sizeof(uint16_t));
    if (!prgSlotBank || !prgSlotUse) { f.close(); printLog("NES: PRG cache meta malloc failed"); freeCart(); return false; }
    for (int i = 0; i < prgSlots; i++) { prgSlotBank[i] = -1; prgSlotUse[i] = 0; }
    for (int i = 0; i < 4; i++) prgMapBank[i] = -1;
    prgUseClock = 0;
    prgFile = FSTYPE.open(path, FILE_READ);      // persistent handle for streaming reads
    if (!prgFile) { f.close(); printLog("NES: PRG stream open failed"); freeCart(); return false; }
    prgDataOffset = dataOffset;
    prgStreamed = true;
    sprintf(buf, "NES: PRG %uK streamed via %d x 8K LRU cache (%dK)",
            (unsigned)(prgSize / 1024), prgSlots, prgSlots * 8);
    printLog(buf);
  }

  // Optional 8K PRG-RAM at $6000 (battery or work RAM). Allocate unconditionally.
  prgRam = (uint8_t *)malloc(8192);
  if (prgRam) memset(prgRam, 0, 8192);

  mapperInit();                                 // set up the initial bank windows (streaming loads them)
  ::selectedNesFileName = path;                 // mark this ROM as the loaded one (settings browser)
  cartHeapBytes = (useStream ? (uint32_t)prgSlots * 8192 : prgSize) + chrAlloc + 8192;  // next guard

  f.close();
  sprintf(buf, "NES: loaded %s, mapper %d, PRG %uK%s CHR %s%uK, %s mirror", path, mapper,
          (unsigned)(prgSize / 1024), useStream ? " (streamed)" : "",
          chrIsRam ? "RAM " : "", (unsigned)(chrAlloc / 1024),
          mirrorMode == MIRROR_VERTICAL ? "vert" : "horiz");
  printLog(buf);
  return true;
}

// Scan the SD root for *.nes files into the global nesFiles list (full paths).
#define NES_MAX_FILES 200
void loadNesFilesSync() {
  nesFiles.clear();
  nesFiles.reserve(NES_MAX_FILES);
  DIR *dp = opendir(SD_VFS_ROOT);
  if (dp) {
    struct dirent *de;
    int scanned = 0;
    while ((de = readdir(dp)) != nullptr) {
      if (de->d_type == DT_DIR) continue;
      std::string nm = de->d_name;
      if (endsWithCI(nm, ".nes")) nesFiles.push_back(std::string("/") + nm);
      if ((++scanned & 0x3f) == 0) ::uiDirScanProgress((int)nesFiles.size());
      if ((int)nesFiles.size() >= NES_MAX_FILES) break;
    }
    closedir(dp);
  }
  sprintf(buf, "NES: %d .nes file(s) on SD root", (int)nesFiles.size());
  printLog(buf);
}

// Load the first .nes on the SD root that actually loads — skip ones refused for an
// unsupported mapper or because they don't fit the heap, so one big/odd ROM doesn't block
// a perfectly good one sitting next to it on the card.
// Peek a .nes header just for its mapper number (-1 if not a valid iNES file).
static int nesPeekMapper(const char *path) {
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) return -1;
  uint8_t h[8];
  int n = f.read(h, 8);
  f.close();
  if (n != 8 || h[0] != 'N' || h[1] != 'E' || h[2] != 'S' || h[3] != 0x1A) return -1;
  return (h[6] >> 4) | (h[7] & 0xF0);
}

bool nesLoadFirstRom() {
  loadWarn[0] = 0;                              // fresh warning text each boot
  if (nesFiles.empty()) loadNesFilesSync();
  if (nesFiles.empty()) {
    printLog("NES: no .nes on SD root");
    strcpy(loadWarn, "No .nes files found on the SD card\n");
    return false;
  }
  // Boot back into the last-loaded ROM (persisted in EEPROM by the settings page), if it still loads.
  if (::selectedNesFileName.length() > 1) {
    sprintf(buf, "NES: autoloading last ROM %s", ::selectedNesFileName.c_str());
    printLog(buf);
    if (nesLoadROM(::selectedNesFileName.c_str())) return true;
    printLog("NES: autoload failed -> scanning SD");
  }
  // Pass 1: prefer a non-NROM ROM, so a freshly-added MMC1/UxROM/CNROM/MMC3 game loads in place of
  // a plain NROM one. Pass 2: NROM / fallback.
  for (size_t i = 0; i < nesFiles.size(); i++)
    if (nesPeekMapper(nesFiles[i].c_str()) >= 1 && nesLoadROM(nesFiles[i].c_str())) return true;
  for (size_t i = 0; i < nesFiles.size(); i++)
    if (nesPeekMapper(nesFiles[i].c_str()) < 1 && nesLoadROM(nesFiles[i].c_str())) return true;
  printLog("NES: no loadable .nes on SD root");
  strncat(loadWarn, "-> no ROM could be loaded\n", sizeof(loadWarn) - 1 - strlen(loadWarn));
  return false;
}

} // namespace nes
