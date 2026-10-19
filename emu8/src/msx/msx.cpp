// This MSX translation unit is compiled out entirely on boards that clear
// BOARD_HAS_MSX_CORE (currently none -- see board.h; it is on for every board).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro;
// the guard below then empties the file, keeping the Z80 core's static RAM out of the image.
#include "../../emu.h"
#if BOARD_HAS_MSX_CORE

// msx.cpp - device-side glue for the MSX1 platform: allocation, BIOS/C-BIOS load, the core-0 render
// push, the core-1 run loop, input injection, and the settings (file browser / load) hooks. This is
// the only MSX file that pulls in emu.h (Arduino/board), keeping the rest of src/msx/ host-portable.
//
// Wired into the platform dispatch by:
//   emu8.ino setup()/loop(), src/shared/video.cpp renderLoop(), and src/shared/optionsui.cpp.

#include "../../emu.h"
#include "msx.h"
#include "msx_cbios.h"
#include "msx_cart.h"
#include "msx_disk.h"
#include "msx_diskrom.h"
#include "../shared/filebrowser.h"   // shared SD image browser (subdirectories + sorting)
#include "../shared/dirty_bands.h"     // PicoCalc: push only the bands that changed
#include "../shared/speedmeter.h"     // live Z80 MHz for the settings title

// ---- per-frame RGB565 conversion band (malloc'd on the MSX path only, like nesScratch) ----------
static uint16_t* msxScratch = nullptr;
static uint8_t*  g_cartBuf  = nullptr;   // device cartridge image (PSRAM); freed on reload
static uint8_t*  g_diskBuf  = nullptr;   // mounted .dsk image (PSRAM); freed on remount
static const uint8_t* g_diskRomBuf = nullptr;  // /roms/hb3600_disk.rom (16K; flash on the PicoCalc), loaded once
static File      g_diskFile;             // persistent r+ handle for SD write-back of dirty sectors
static bool      g_diskFileOpen = false;
static bool      g_diskWritable = false; // g_diskFile opened r+ (else read-only: streamed writes fail = WP)
static bool      g_diskNeedsFlush = false;   // streamed writes since the last g_diskFile.flush()
static String    g_diskPath;             // the mounted .dsk (settings UNMOUNT button), "" = none
static String    g_cartPath;             // the loaded .rom/.mx1/.mx2 cartridge (settings UNMOUNT button), "" = none

// Streamed .dsk (no RAM copy): the FDC reads/writes each 512-byte sector straight from the SD file.
// Called on core 1 from inside the Z80 FDC access; the bus lock serializes it against core 0.
static bool msxDiskStreamIO(uint32_t off, uint8_t* b, bool write) {
  if (!g_diskFileOpen || (write && !g_diskWritable)) return false;
  busTake();
  bool ok = g_diskFile.seek(off);
  if (ok) ok = write ? (g_diskFile.write(b, 512) == 512) : (g_diskFile.read(b, 512) == 512);
  busGive();
  if (ok && write) g_diskNeedsFlush = true;      // committed once per frame by msxDiskFlush
  return ok;
}

// Drain up to maxSectors dirty sectors of the mounted .dsk back to the SD file (called from msxLoop,
// core 1; the bus lock serializes the SD access against the touch reads on core 0).
static void msxDiskFlush(int maxSectors) {
  if (g_diskFileOpen && g_diskNeedsFlush) {        // streamed image: sectors are already in the file
    g_diskNeedsFlush = false;
    busTake(); g_diskFile.flush(); busGive();
  }
  if (!g_diskFileOpen || !msx::diskHasDirty()) return;
  busTake();
  for (int i = 0; i < maxSectors; i++) {
    const uint8_t* data; uint32_t off;
    int sec = msx::diskTakeDirtySector(&data, &off);
    if (sec < 0) break;
    g_diskFile.seek(off);
    g_diskFile.write(data, 512);
  }
  g_diskFile.flush();
  busGive();
}
static void msxDiskClose() {              // flush everything left + close the write-back handle
  if (!g_diskFileOpen) return;
  while (msx::diskHasDirty()) msxDiskFlush(64);
  busTake(); g_diskFile.close(); busGive();
  g_diskFileOpen = false; g_diskWritable = false; g_diskNeedsFlush = false;
}
volatile bool msxResetReq = false;   // set by the desktop debugger (debug_bridge.cpp) for an in-process reset
static const int M_W = 256, M_H = 192, M_OX = (320 - 256) / 2;

static uint8_t* msxAllocFast(size_t n) {                 // internal SRAM first (CPU hot path), PSRAM fallback
  uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t*)ps_malloc(n);
  return p;
}
#if BOARD_HAS_MSX2
// MSX2's big buffers (128K VRAM, 256K mapper) take internal SRAM only while 64K of it stays free for
// the rest of the system (display, audio, SD, Wi-Fi); otherwise PSRAM.
static uint8_t* msx2Alloc(size_t n) {
  uint8_t* p = nullptr;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= n + 0x10000)
    p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t*)ps_malloc(n);
  return p;
}
#endif
static bool msxEndsCI(const std::string& s, const char* suf) {
  size_t n = strlen(suf); if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i])) return false;
  return true;
}
static bool isBiosName(const std::string& nm) {
  return msxEndsCI(nm, "msxbios.rom") || msxEndsCI(nm, "/msx.rom") || nm == "MSX.ROM" || nm == "msx.rom"
      || msxEndsCI(nm, "cbios_main_msx1.rom") || msxEndsCI(nm, "cbios_main_msx2.rom")
      || msxEndsCI(nm, "cbios_sub.rom") || msxEndsCI(nm, "cbios_logo_msx2.rom") || msxEndsCI(nm, "msx2ext.rom");
}

#if BOARD_HAS_MSX2
#define MSX_BIOS_MAX 0xC000      // MSX2: main 32K + SUB-ROM (or another 16K at $8000 of slot 0)
#else
#define MSX_BIOS_MAX 0x8000
#endif

#if BOARD_HAS_MSX2
// ---- MSX2 SUB-ROM -------------------------------------------------------------------------------
// An MSX2 BIOS (version byte 0x2D >= 1) needs its 16K SUB-ROM (slot 3-0). Taken from the BIOS file
// itself when that is 48K with "CD" at 32K (a main + sub dump), else from a file next to the BIOS:
// the BIOS name with "main" -> "sub" (nms8245_main.rom -> nms8245_sub.rom), "basic-bios2" -> "msx2sub"
// (openMSX names), "msx2" -> "msx2ext" (blueMSX's MSX2.ROM + MSX2EXT.ROM), cbios_sub.rom for C-BIOS,
// <stem>_sub.rom / <stem>sub.rom, then msx2sub.rom / msx2ext.rom. C-BIOS also gets its logo ROM
// (cbios_logo_msx2.rom) at $8000 of slot 0 when the BIOS file is the bare 32K main ROM.
static std::string msx2Replace(const std::string& s, const char* from, const char* to) {
  std::string lo = s;
  for (auto& c : lo) c = (char)tolower((unsigned char)c);
  size_t i = lo.find(from);
  if (i == std::string::npos) return "";
  return s.substr(0, i) + to + s.substr(i + strlen(from));
}
static bool msx2ReadFile(const std::string& path, uint8_t* dst, int len) {
  if (path.empty()) return false;
  File f = FSTYPE.open(path.c_str(), FILE_READ);
  if (!f) return false;
  bool ok = (int)f.size() == len && f.read(dst, len) == len;
  f.close();
  return ok;
}
static bool msx2LoadSubRom(const char* biosPath, uint8_t* bios, int* len) {
  if (*len > 0x8000 && bios[0x8000] == 'C' && bios[0x8001] == 'D') {
    msx::subRom = bios + 0x8000; *len = 0x8000;
    printLog("MSX2: SUB-ROM from the BIOS file");
    return true;
  }
  const std::string path = biosPath;
  const size_t slash = path.rfind('/');
  const std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = name.rfind('.');
  const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
  const std::string ext = dot == std::string::npos ? ".rom" : name.substr(dot);
  std::string lo = name;
  for (auto& c : lo) c = (char)tolower((unsigned char)c);
  const bool cbios = lo.find("cbios") != std::string::npos;

  if (cbios && *len == 0x8000 && msx2ReadFile(dir + "cbios_logo_msx2.rom", bios + 0x8000, 0x4000)) {
    *len = 0xC000;
    printLog("MSX2: C-BIOS logo ROM at slot 0 $8000");
  }
  std::string cand[] = {
    msx2Replace(name, "main", "sub").empty() ? "" : dir + msx2Replace(name, "main", "sub"),
    msx2Replace(name, "basic-bios2", "msx2sub").empty() ? "" : dir + msx2Replace(name, "basic-bios2", "msx2sub"),
    msx2Replace(name, "msx2", "msx2ext").empty() ? "" : dir + msx2Replace(name, "msx2", "msx2ext"),
    cbios ? dir + "cbios_sub.rom" : "",
    dir + stem + "_sub" + ext, dir + stem + "sub" + ext,
    dir + "msx2sub.rom", dir + "msx2ext.rom", dir + "MSX2EXT.ROM",
  };
  uint8_t* sub = msxAllocFast(0x4000);
  if (!sub) return false;
  for (const std::string& c : cand) {
    if (c.empty() || c == path) continue;
    if (msx2ReadFile(c, sub, 0x4000) && sub[0] == 'C' && sub[1] == 'D') {
      msx::subRom = sub;
      sprintf(buf, "MSX2: SUB-ROM %.80s", c.c_str()); printLog(buf);
      return true;
    }
  }
  free(sub);
  return false;
}
#endif

// ---- SD BIOS ------------------------------------------------------------------------------------
static const char* g_biosErr = "";        // why the BIOS did not load, for the NO BIOS screen
static char g_biosPath[ROMSEL_SLOT_LEN];  // the file that was tried
static uint8_t g_biosId[2];               // the BIOS ID bytes $2B/$2C as loaded (msxBiosRegionFix)
static bool g_biosIdSaved = false;
static bool loadBiosFromSD() {
  // Exactly one file: the BIOS picked on the settings ROMS page, else the default
  // /roms/hotbit12.rom (Sharp Epcom HotBit 1.2: MSX1 BIOS + MSX BASIC). Nothing else is tried.
  // Booted as "MSX2" from the system menu, the MSX2 BIOS slot instead (default /roms/cbios_main_msx2.rom).
  char picked[ROMSEL_SLOT_LEN];
  snprintf(picked, sizeof(picked), "%s", romPath(Msx2Selected ? ROMSEL_MSX2_BIOS : ROMSEL_MSX_BIOS));
  strcpy(g_biosPath, picked);
  const char* nm = picked;
  msx::biosLen = 0;
  if (!*nm) { g_biosErr = "BIOS set to NONE"; printLog("MSX: NO BIOS - slot set to none"); return false; }
  File f = FSTYPE.open(nm, FILE_READ);
  if (!f) { g_biosErr = "cannot open file"; sprintf(buf, "MSX: NO BIOS - cannot open %.80s", nm); printLog(buf); return false; }
  int len = f.size();
  if (len < 0x4000 || len > MSX_BIOS_MAX) {
    f.close(); g_biosErr = MSX_BIOS_MAX > 0x8000 ? "wrong size (need 16-48K)" : "wrong size (need 16-32K)";
    sprintf(buf, "MSX: NO BIOS - %.80s is %d bytes, need 16-%dK", nm, len, MSX_BIOS_MAX / 1024); printLog(buf); return false;
  }
#if BOARD_ROM_IN_FLASH
  // PicoCalc: no PSRAM, so the BIOS is copied into spare flash and read through XIP (a no-op
  // compare on every boot after the first). See src/picocalc/romflash_picocalc.cpp. Staged through
  // msxScratch like the cart and disk ROM: after the 80K of RAM + VRAM the heap may not spare 4K.
  const uint8_t* fb = romFlashLoad(ROMFLASH_BIOS, f, len, (uint8_t*)msxScratch); f.close();
  if (!fb) { g_biosErr = "copy to flash failed"; sprintf(buf, "MSX: NO BIOS - %.80s flash load failed", nm); printLog(buf); return false; }
  msx::bios = fb; msx::biosLen = len;
  msx::biosIsCbios = strstr(nm, "cbios") || strstr(nm, "CBIOS");
  sprintf(buf, "MSX: BIOS %s (%dK, flash)", nm, len / 1024); printLog(buf);
  return true;
#else
  uint8_t* b = msxAllocFast(MSX_BIOS_MAX);
  if (!b) { f.close(); g_biosErr = "out of memory"; printLog("MSX: NO BIOS - alloc failed"); return false; }
  int got = f.read(b, len); f.close();
  if (got != len) { free(b); g_biosErr = "SD read short"; sprintf(buf, "MSX: NO BIOS - %.80s read short", nm); printLog(buf); return false; }
#if BOARD_HAS_MSX2
  msx::isMsx2 = len >= 0x8000 && b[0x2D] >= 1;     // MSX version byte: 0 MSX1, 1 MSX2, 2 MSX2+
  msx::subRom = nullptr;
  if (msx::isMsx2 && !msx2LoadSubRom(nm, b, &len)) {
    free(b); msx::isMsx2 = false; g_biosErr = "MSX2 BIOS: SUB-ROM not found";
    sprintf(buf, "MSX: NO BIOS - %.80s is MSX2 but no SUB-ROM was found next to it", nm); printLog(buf); return false;
  }
#endif
  msx::bios = b; msx::biosLen = len;
  msx::biosIsCbios = strstr(nm, "cbios") || strstr(nm, "CBIOS");
  if (len >= 0x30) { g_biosId[0] = b[0x2B]; g_biosId[1] = b[0x2C]; g_biosIdSaved = true; }
  sprintf(buf, "MSX: BIOS %s (%dK)", nm, len / 1024); printLog(buf);
  return true;
#endif
}

// Scan SD root for *.rom / *.dsk into msxFiles (excluding the reserved BIOS names).
#define MSX_MAX_FILES FB_MAX_ENTRIES
// SD ROM/disk browser (.rom/.mx1/.mx2/.dsk + subdirectories); see src/shared/filebrowser.h.
// The BIOS images stay hidden -- they are loaded by name, not picked.
static bool msxAccept(const std::string &n) {
  if (isBiosName(n)) return false;
  return msxEndsCI(n, ".rom") || msxEndsCI(n, ".mx1") || msxEndsCI(n, ".mx2") || msxEndsCI(n, ".dsk");
}
static FileBrowser msxBrowser = { "MSX", &msxFiles, msxAccept, nullptr, MSX_MAX_FILES, "" };

void loadMsxFilesSync()      { fbScan(msxBrowser); }
void msxBrowseEnter(const char *path) { fbEnter(msxBrowser, path); }
void msxBrowseUp()           { fbUp(msxBrowser); }

// ============================ platform entry points =============================================
static void msxDiskRomUpdate();   // below, with the other disk code
void msxSetup() {
  printLog("MSX1 Setup... (Z80 + TMS9918 VDP + AY-3-8910 PSG + 8255 PPI)");

#if defined(BOARD_PICOCALC)
  // No PSRAM, so the heap has to hold the 80K of RAM + VRAM below. The ~6.8K of side buffers go in
  // sharedBigBuf past the 49152-byte framebuffer instead: that tail is only used while an Apple II
  // runs (its IIe banks and speaker stack), and a platform switch reboots.
  static_assert(sizeof(sharedBigBuf) >= 256 * 192 + 256 * 8 * 2 + 2 * 0x546,
                "MSX framebuffer + scratch band + menu buffers must fit in sharedBigBuf");
  msxScratch = (uint16_t*)(sharedBigBuf + 256 * 192);           // 4096 bytes, 16-bit aligned; no render
                                                                 // use here: flash staging for carts/disk ROM
  menuScreen = sharedBigBuf + 256 * 192 + 256 * 8 * 2;
  menuColor  = menuScreen + 0x546;
#else
  // shared text UI buffers (settings window) - allocate like the C64/NES/Atari paths
  menuScreen = (unsigned char*)malloc(0x546);
  menuColor  = (unsigned char*)malloc(0x546);
  msxScratch = (uint16_t*)malloc(256 * 8 * sizeof(uint16_t));
#endif

#if BOARD_HAS_MSX2
  // The BIOS decides MSX1 or MSX2, so it is loaded before the machine memory is sized.
  loadBiosFromSD();
  if (msx::isMsx2) {
    printLog("MSX2 mode: V9938 VDP (128K VRAM) + 256K memory mapper + RP5C01 clock");
    for (int segs = 16; segs >= 8 && !msx::mapRam; segs /= 2) {
      msx::mapRam = msx2Alloc((size_t)segs * 0x4000);
      if (msx::mapRam) msx::mapSegs = segs;
    }
    msx::vram = msx2Alloc(msx::V99_VRAM_SIZE);
    msx::framebuffer = (uint8_t*)ps_malloc(msx::FB2_W * msx::FB2_H);
    if (!msx::framebuffer) msx::framebuffer = msx2Alloc(msx::FB2_W * msx::FB2_H);
    if (!msx::mapRam || !msx::vram || !msx::framebuffer) {
      sprintf(buf, "MSX2: out of memory for RAM/VRAM (heap=%u)", (unsigned)ESP.getFreeHeap());
      printLog(buf);
      g_biosErr = "out of memory for MSX2 RAM/VRAM";
      msx::biosLen = 0;
      return;
    }
    memset(msx::mapRam, 0, (size_t)msx::mapSegs * 0x4000);
    memset(msx::vram, 0, msx::V99_VRAM_SIZE);
    memset(msx::framebuffer, 0, msx::FB2_W * msx::FB2_H);
    msx::ram = msx::mapRam;                      // the first 64K, for code that peeks "slot 3 RAM"
    msx::machineWire();
    msxDiskRomUpdate();
    msx::machineReset();
    if (selectedMsxFileName.length() > 1 && selectedMsxFileName != "/")
      msxLoadSelected(selectedMsxFileName.c_str());
    sprintf(buf, "MSX2 ready (%dK mapper). internal free=%u, heap=%u", msx::mapSegs * 16,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)ESP.getFreeHeap());
    printLog(buf);
    return;
  }
#endif
  msx::ram  = msxAllocFast(0x10000);            // 64 KB work RAM (slot 3) - keep internal for speed
#if defined(BOARD_PICOCALC)
  static_assert(msx::VRAM_SIZE <= PLATFORM_POOL_SIZE, "MSX VRAM must fit platformPool");
  msx::vram = platformPool;                     // 16 KB VDP RAM: static, the heap only holds the 64K RAM
#else
  msx::vram = msxAllocFast(msx::VRAM_SIZE);     // 16 KB VDP RAM - read every frame
#endif
  msx::framebuffer = sharedBigBuf;              // 256*192 = 49152 <= sizeof(sharedBigBuf)
  if (msx::ram)  memset(msx::ram, 0, 0x10000);
  if (msx::vram) memset(msx::vram, 0, msx::VRAM_SIZE);
  if (!msx::ram || !msx::vram) {
    sprintf(buf, "MSX: out of memory for RAM/VRAM (heap=%u)", (unsigned)ESP.getFreeHeap());
    printLog(buf);
    g_biosErr = "out of memory for RAM/VRAM";   // shown on the NO BIOS screen
    msx::biosLen = 0;                            // msxLoop idles on biosLen == 0 instead of crashing
    return;
  }

#if !BOARD_HAS_MSX2
  loadBiosFromSD();                              // SD BIOS, else embedded C-BIOS, else biosLen=0 (warning)
#endif

  msx::machineWire();
  msxDiskRomUpdate();                            // Disk BASIC in slot 2 unless DISK ROM is OFF
  msx::machineReset();

  // auto-load the saved cartridge (.rom/.mx1/.mx2) or mount the saved disk (.dsk) on boot
  if (selectedMsxFileName.length() > 1 && selectedMsxFileName != "/")
    msxLoadSelected(selectedMsxFileName.c_str());

  sprintf(buf, "MSX ready. internal free=%u, heap=%u",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)ESP.getFreeHeap());
  printLog(buf);
}

// NTSC MSX1 runs the Z80 at 3.579545 MHz: 59736 T-states per 59.92 Hz frame = 16687 us/frame. In
// NORMAL mode we pace each frame to that wall-clock so the emulator matches real hardware; in FAST
// mode (msxFast) we run uncapped (as fast as the S3 manages). A one-time boot benchmark reports the
// host's effective Z80 speed so we know how much headroom there is over the real 3.58 MHz.
void msxLoop() {
  if (msx::biosLen > 0) {                              // uncapped benchmark (~0.8 s of emulated time)
    uint32_t c0 = msx::cpu.cycles; uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 800) msx::runFrame();
    uint32_t dt = millis() - t0; uint32_t dc = msx::cpu.cycles - c0;
    msxMeasuredMhz = (dt > 0) ? (float)((double)dc / ((double)dt * 1000.0)) : 0.0f;
    sprintf(buf, "MSX: uncapped Z80 = %.2f MHz (real MSX1 = 3.58)", msxMeasuredMhz);
    printLog(buf);
  }

  uint32_t FRAME_US = 16687;
  uint32_t nextUs = micros();
  SpeedMeter meter; meter.restart(msx::cpu.cycles);
  for (;;) {
    if (OptionsWindow) { vTaskDelay(pdMS_TO_TICKS(20)); nextUs = micros(); meter.restart(msx::cpu.cycles); continue; }  // paused in settings
    if (msxResetReq)   { msxResetReq = false; msx::machineReset(); nextUs = micros(); meter.restart(msx::cpu.cycles); }
    if (msx::biosLen == 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }                 // no BIOS -> nothing to run

    msx::runFrame();
#if BOARD_HAS_MSX2
    if (msx::isMsx2) FRAME_US = msx::frameMicros();  // 60 or 50 Hz, per the V9938's R9
#endif
    meter.tick(msx::cpu.cycles, msxMeasuredMhz);
    msxDiskFlush(4);                                   // drain a few dirty disk sectors to SD per frame

    if (msxFast) {
#if defined(BOARD_DESKTOP)
      taskYIELD();                                     // desktop: full host speed (render/input on other threads)
#else
      vTaskDelay(1);                                   // uncapped: just yield to core 0 / WDT
#endif
      nextUs = micros();
    } else {                                           // pace to real 3.58 MHz (absolute target = no drift)
      nextUs += FRAME_US;
      int32_t wait = (int32_t)(nextUs - micros());
      if (wait > 0) {
        uint32_t ms = (uint32_t)wait / 1000;
        vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));        // sleep the ms bulk (yields); sub-ms self-corrects
      } else if (wait < -100000) {
        nextUs = micros();                             // far behind (interp slower than real / resumed): resync
      } else {
        vTaskDelay(1);                                 // slightly behind: yield, keep WDT fed
      }
    }
  }
}

// Convert the 256x192 indexed framebuffer to RGB565 and push it (8-line bands), like atariRenderFrame.
// Runs on the core-0 render task (owns the TFT). vdpRender() fills the framebuffer. When the on-screen
// keyboard is open the picture is vertically SCALED into the rows above it (oskRasterTop/Height) so the
// emulated screen stays fully visible above the keyboard instead of being covered (matches Apple/C64).
#if BOARD_HAS_MSX2
// ---- MSX2 display -------------------------------------------------------------------------------
// The V9938 framebuffer is 512 bytes per line, each line 256 or 512 px (v99LineFlags) of palette
// indices or GRB332. It is resampled to the output width with a box filter (512 -> 320 keeps the
// 80-column text readable; 256 -> 320 for SCREEN: ENLARGE / STRETCH) and nearest-line vertically.
// ORIG: narrow frames 1:1 centred, frames with a wide line 320 wide; ENLARGE 320x240; STRETCH the raw
// 320x320 panel. On the PicoCalc only the 8-line source bands that changed are sent.
struct HTap { uint16_t i; uint8_t n; uint8_t w[3]; };
static HTap g_tap256[320], g_tap512[320];
static int  g_tapW = -1;
static void msx2BuildTaps(HTap* t, int srcW, int ow) {
  for (int x = 0; x < ow; x++) {
    const int a = x * srcW, b = (x + 1) * srcW;          // in 1/ow source pixels
    HTap& h = t[x];
    h.i = (uint16_t)(a / ow); h.n = 0;
    int sum = 0;
    for (int j = h.i; j * ow < b && h.n < 3; j++) {
      const int lo = a > j * ow ? a : j * ow, hi = b < (j + 1) * ow ? b : (j + 1) * ow;
      int w = (hi - lo) * 256 / srcW;
      h.w[h.n++] = (uint8_t)(w > 255 ? 255 : w); sum += w;
    }
    if (h.n == 1) h.w[0] = 255;
    else h.w[h.n - 1] = (uint8_t)(h.w[h.n - 1] + (256 - sum));
    if (h.i + h.n > srcW) h.n = (uint8_t)(srcW - h.i);
  }
}
static inline uint16_t msx2Mix(const uint16_t* c, const HTap& t) {
  if (t.n == 1) return c[t.i];
  uint32_t r = 0, g = 0, b = 0;
  for (int k = 0; k < t.n; k++) {
    const uint16_t v = c[t.i + k]; const uint32_t w = t.w[k];
    r += (v >> 11) * w; g += ((v >> 5) & 63) * w; b += (v & 31) * w;
  }
  return (uint16_t)(((r >> 8) << 11) | ((g >> 8) << 5) | (b >> 8));
}
// One output row: source line sy resampled to ow pixels.
static void msx2Row(int sy, int ow, uint16_t* out) {
  static uint16_t src[512];
  const uint8_t fl = msx::v99LineFlags[sy];
  const int sw = (fl & msx::V99_LINE_WIDE) ? 512 : 256;
  const uint8_t* p = msx::framebuffer + sy * msx::FB2_W;
  if (fl & msx::V99_LINE_G7) for (int x = 0; x < sw; x++) src[x] = msx::V99_PAL332[p[x]];
  else                       for (int x = 0; x < sw; x++) src[x] = msx::v99DispPal[p[x] & 15];
  const HTap* t = (sw == 512) ? g_tap512 : g_tap256;
  for (int x = 0; x < ow; x++) out[x] = msx2Mix(src, t[x]);
}

static void msx2RenderFrame() {
  if (!msx::frameReady) return;
  if (!msx::framebuffer) { msx::frameReady = false; return; }
  const int H = msx::v99FrameH;
  bool wide = false;
  for (int y = 0; y < H && !wide; y++) wide = msx::v99LineFlags[y] & msx::V99_LINE_WIDE;
  const bool osk = oskActive();
#if UI_PICOCALC
  extern uint8_t screenFill;
  const uint8_t fill = osk ? (uint8_t)SCREEN_ORIG : screenFillNow();
#else
  const uint8_t fill = SCREEN_ORIG;
#endif
  int ox, oy, ow, oh;
  if (fill == SCREEN_STRETCH)      { ox = 0; oy = 0; ow = 320; oh = PANEL_NATIVE_H; }
  else if (fill)                   { ox = 0; oy = 0; ow = 320; oh = 240; }
  else {
    ow = wide ? 320 : 256; ox = (320 - ow) / 2;
    if (osk) { oy = oskRasterTop(); oh = oskRasterHeight(); }
    else     { oh = H; oy = (240 - H) / 2; }
  }
  if (ow != g_tapW) { msx2BuildTaps(g_tap256, 256, ow); msx2BuildTaps(g_tap512, 512, ow); g_tapW = ow; }
  static uint16_t row[320];

#if UI_PICOCALC
  static int lastGeo = -1;
  static uint8_t lastFill = SCREEN_ORIG;
  const int geo = ox | (oy << 9) | (ow << 18) | (oh << 9 << 18);
  if (lastFill == SCREEN_STRETCH && fill != SCREEN_STRETCH) tft.fillPanelBlack();
  const bool repaint = clearScr || geo != lastGeo || fill != lastFill;
  if (repaint && !fill) {                              // black borders around the picture
    tft.fillRect(0, 0, 320, oy, TFT_BLACK);
    if (!osk) tft.fillRect(0, oy + oh, 320, 240 - (oy + oh), TFT_BLACK);
    tft.fillRect(0, oy, ox, oh, TFT_BLACK);
    tft.fillRect(ox + ow, oy, 320 - (ox + ow), oh, TFT_BLACK);
  }
  clearScr = false; lastGeo = geo; lastFill = fill;

  uint32_t ph = dband::SEED;
  for (int i = 0; i < 16; i++) ph = dband::mix(ph, msx::v99DispPal[i]);
  dband::begin(dband::mix(dband::mix(0x4D535832u, ph), (uint32_t)geo ^ ((uint32_t)fill << 30)), repaint);
  const int bands = (H + 7) / 8;
  bool dirty[dband::MAX_BANDS];
  for (int b = 0; b < bands; b++) {
    const int y0 = b * 8, n = (y0 + 8 <= H ? 8 : H - y0);
    uint32_t h = dband::hash(dband::SEED, msx::framebuffer + y0 * msx::FB2_W, (uint32_t)n * msx::FB2_W);
    for (int y = y0; y < y0 + n; y++) h = dband::mix(h, msx::v99LineFlags[y]);
    dirty[b] = dband::changed(b, h);
  }
  int sent = 0;
  for (int b = 0; b < bands; ) {
    if (!dirty[b]) { b++; continue; }
    int e = b;
    while (e < bands && dirty[e]) e++;
    sent += e - b;
    const int r0 = dband::stretchRow(b * 8, H, oh), r1 = dband::stretchRow(e * 8 < H ? e * 8 : H, H, oh);
    if (r1 > r0) {
      if (fill == SCREEN_STRETCH) tft.setPanelAddrWindow(ox, r0, ow, r1 - r0);
      else                        tft.setAddrWindow(ox, oy + r0, ow, r1 - r0);
      tft.startWrite();
      int lastSy = -1;
      for (int r = r0; r < r1; r++) {
        int sy = r * H / oh; if (sy > H - 1) sy = H - 1;
        if (sy != lastSy) { msx2Row(sy, ow, row); lastSy = sy; }
        for (int x = 0; x < ow; x++) tft.writeColor(row[x], 1);
      }
      tft.endWrite();
    }
    b = e;
  }
  dband::frameDone("MSX2", sent);
#else
  tft.fillRect(0, 0, 320, oy, TFT_BLACK);
  if (!osk) tft.fillRect(0, oy + oh, 320, 240 - (oy + oh), TFT_BLACK);
  tft.fillRect(0, oy, ox, oh, TFT_BLACK);
  tft.fillRect(ox + ow, oy, 320 - (ox + ow), oh, TFT_BLACK);
  tft.setSwapBytes(true);
  for (int r = 0; r < oh; r++) {
    int sy = r * H / oh; if (sy > H - 1) sy = H - 1;
    msx2Row(sy, ow, row);
    tft.pushImage(ox, oy + r, ow, 1, row);
  }
  tft.setSwapBytes(false);
  fpsFrameShown();
#endif
  msx::frameReady = false;
}
#endif

void msxRenderFrame() {
#if BOARD_HAS_MSX2
  if (msx::isMsx2) { msx2RenderFrame(); return; }
#endif
  // The VDP framebuffer is rendered on the CPU core (msx::runFrame). Display it only when a fresh
  // frame is ready, and clear the flag when done so core 1 can render the next one -- this
  // handshake guarantees the two cores never touch the framebuffer simultaneously (no tearing).
  if (!msx::frameReady) return;                 // no new frame; the panel keeps the last one
  if (!msxScratch || !msx::framebuffer) { msx::frameReady = false; return; }
  const uint16_t* pal = msx::MSX_PALETTE;
  const int outTop = oskRasterTop();      // 0 when the keyboard is open, else 24 (centered)
  const int outH   = oskRasterHeight();   // keyboard-top (112) when open, else 192 (1:1)
  const int belowY = outTop + outH;
#if UI_PICOCALC
  // PicoCalc: SPI is the bottleneck (50MHz, ~16ms for the 256x192 picture alone), so the black
  // borders -- another ~28K pixels -- are painted only when something may have drawn over them
  // (a menu closed -> clearScr) or the raster moved, not every frame.
  // SCREEN: ENLARGE (Settings) scales the picture 5:4 on both axes to the whole 320x240 logical
  // screen, STRETCH 5:4 x 5:3 to the whole raw 320x320 panel, so there are no borders to paint --
  // but leaving either must repaint them over the old picture (and leaving STRETCH the bars).
  extern uint8_t screenFill;
  static int  lastTop = -1, lastH = -1;
  static bool lastOsk = false;
  static uint8_t lastFill = SCREEN_ORIG;
  const bool osk  = oskActive();
  const uint8_t fill = osk ? (uint8_t)SCREEN_ORIG : screenFillNow();
  if (lastFill == SCREEN_STRETCH && fill != SCREEN_STRETCH) tft.fillPanelBlack();   // its bars
  const bool borders = !fill && (clearScr || outTop != lastTop || outH != lastH || osk != lastOsk || lastFill);
  const bool repaint = borders || clearScr;          // anything may have drawn over the picture
  clearScr = false;
  lastTop = outTop; lastH = outH; lastOsk = osk; lastFill = fill;
#else
  const bool borders = true;
  const bool osk = oskActive();
#endif
  if (borders) {
    tft.fillRect(0, 0, 320, outTop, TFT_BLACK);                       // top border (none when OSK open)
    // Below the picture: when the keyboard is open it OWNS that region (and only repaints when dirty),
    // so we must NOT clear it every frame or we'd erase the keyboard to black. Clear it only when closed.
    if (!osk) tft.fillRect(0, belowY, 320, 240 - belowY, TFT_BLACK);
    tft.fillRect(0, outTop, M_OX, outH, TFT_BLACK);                   // left border
    tft.fillRect(M_OX + M_W, outTop, 320 - (M_OX + M_W), outH, TFT_BLACK); // right border
  }
#if UI_PICOCALC
  // ENLARGE / STRETCH or 1:1: only the 8-line bands that changed since the last push go out (dirty_bands.h).
  if (fill || outH == M_H) {
    const int sent = dband::pushIndexed256(msx::framebuffer, pal, 0x0F, M_OX, outTop, fill, false,
                                           0x4D535831u, repaint);
    dband::frameDone("MSX", sent);
    msx::frameReady = false;
    return;
  }
  // On-screen keyboard open (picture scaled): the whole picture in one address window, pixels
  // streamed through writeColor()'s ping-pong DMA staging.
  tft.setAddrWindow(M_OX, outTop, M_W, outH);
  tft.startWrite();
  for (int oy = 0; oy < outH; oy++) {
    int sy = oy * M_H / outH;                          // nearest-neighbor vertical scale (1:1 when outH==192)
    if (sy > M_H - 1) sy = M_H - 1;
    const uint8_t* src = msx::framebuffer + sy * M_W;
    for (int x = 0; x < M_W; x++) tft.writeColor(pal[src[x] & 0x0F], 1);
  }
  tft.endWrite();
#else
  tft.setSwapBytes(true);
  for (int oy = 0; oy < outH; ) {
    int n = 0;
    while (oy + n < outH && n < 8) {
      int sy = (oy + n) * M_H / outH;                  // nearest-neighbor vertical scale (1:1 when outH==192)
      if (sy > M_H - 1) sy = M_H - 1;
      const uint8_t* src = msx::framebuffer + sy * M_W;
      uint16_t* dst = msxScratch + n * M_W;
      for (int x = 0; x < M_W; x++) dst[x] = pal[src[x] & 0x0F];
      n++;
    }
    tft.pushImage(M_OX, outTop + oy, M_W, n, msxScratch);
    oy += n;
  }
  tft.setSwapBytes(false);
#endif
  fpsFrameShown();
  msx::frameReady = false;     // done reading the framebuffer; core 1 may render the next frame
}

// msxPsgSetup() lives in msx_audio.cpp (it owns the I2S driver), like atariAudioSetup/sidSetup.

// ---- input (from joystick.cpp / touchkeyboard.cpp / usbkeyboard.cpp) ----
void msxKeyMatrix(uint8_t row, uint8_t col, bool down) { msx::kbSetKey(row, col, down); }
void msxSetInput(uint8_t joyMask) { msx::setJoystick(joyMask); }

// ---- cartridge compatibility fixes (RAM BIOS/cart only; the PicoCalc's flash copies stay as read) ----
// Region: a Japanese cart that reads the BIOS ID bytes ($2B/$2C: char set, date format, keyboard)
// and resets on a non-Japanese machine (Metal Gear) gets a BIOS reporting Japan. Detected by the
// file-name tag, else by an LD HL/A/rr,($002B/$002C) in the image; any other cart gets the
// original bytes back. BASIC loader: a ROM with only a BASIC program (INIT 0, TEXT set) whose program
// is DEFUSR=&Hxxxx + USR (Super Lode Runner) cannot start under C-BIOS (no BASIC), so its INIT is
// pointed at the USR address.
static bool msxHasTag(const std::string& p, const char* const* tags) {
  std::string lo = p;
  for (auto& c : lo) c = (char)tolower((unsigned char)c);
  for (; *tags; tags++) if (lo.find(*tags) != std::string::npos) return true;
  return false;
}
static bool msxReadsBiosId(const uint8_t* d, int len) {
  for (int i = 0; i + 3 < len; i++) {
    const uint8_t lo = d[i + 1], hi = d[i + 2];
    if (hi != 0 || (lo != 0x2B && lo != 0x2C)) continue;
    const uint8_t op = d[i];
    if (op == 0x2A || op == 0x3A) return true;                                     // LD HL/A,(nn)
    if (i > 0 && (d[i - 1] == 0xED && (op == 0x4B || op == 0x5B || op == 0x7B))) return true;   // LD rr,(nn)
    if (i > 0 && ((d[i - 1] == 0xDD || d[i - 1] == 0xFD) && op == 0x2A)) return true;           // LD IX/IY,(nn)
  }
  return false;
}
static void msxBiosRegionFix(const std::string& path, const uint8_t* cart, int len) {
  if (!g_biosIdSaved || !msx::bios || msx::biosLen < 0x30) return;
  static const char* const jpTags[]    = { "(j)", "(japan)", nullptr };
  static const char* const otherTags[] = { "(e)", "(eu", "(uk)", "(u)", "(usa)", "(b)", "(bra", "(s)", "(spa",
                                           "(k)", "(kor", "(a)", "(ar", "(f)", "(fr", "(g)", "(ger", "(nl", nullptr };
  const bool jp = cart && (msxHasTag(path, jpTags) || (!msxHasTag(path, otherTags) && msxReadsBiosId(cart, len)));
  uint8_t* b = (uint8_t*)msx::bios;
  const uint8_t want0 = jp ? (uint8_t)(g_biosId[0] & 0x80) : g_biosId[0];   // keep the 50/60 Hz bit
  const uint8_t want1 = jp ? (uint8_t)(g_biosId[1] & 0xF0) : g_biosId[1];   // keep the BASIC version
  if (b[0x2B] == want0 && b[0x2C] == want1) return;
  b[0x2B] = want0; b[0x2C] = want1;
  sprintf(buf, "MSX: BIOS region -> %s", jp ? "Japan (cart checks it)" : "original"); printLog(buf);
}
__attribute__((unused)) static void msxBasicStubFix(uint8_t* cart, int len) {
  if (!msx::biosIsCbios || len < 0x4000 || cart[0] != 'A' || cart[1] != 'B') return;
  const int init = cart[2] | cart[3] << 8, text = cart[8] | cart[9] << 8;
  if (init || text < 0x4000 || text >= 0xC000) return;
  const int base = text & 0xC000, off = text - base;           // the header sits at the start of that page
  if (off + 2 >= len) return;
  int usr = -1, calls = 0;
  for (int i = off; i < len && i < off + 0x400; i++) {         // a loader stub is a few short lines
    const uint8_t t = cart[i];
    if (t == 0xDD) calls++;                                    // USR token
    if (t == 0x97 && usr < 0) {                                // DEF ... USR[n] = &Hxxxx / integer
      int j = i + 1;
      while (j < len && cart[j] == ' ') j++;
      if (j >= len || cart[j] != 0xDD) continue;
      j++;
      if (j < len && cart[j] >= 0x11 && cart[j] <= 0x1A) j++;  // USR0..USR9
      while (j < len && cart[j] == ' ') j++;
      if (j + 3 >= len || cart[j] != 0xEF) continue;           // '='
      j++;
      if (cart[j] == 0x0C || cart[j] == 0x1C) usr = cart[j + 1] | cart[j + 2] << 8;
    }
  }
  if (usr < 0x4000 || usr >= 0xC000 || calls < 2) return;      // DEFUSR + a USR() call
  for (int bank = 0; bank + 16 <= len; bank += 0x4000)         // mapper carts repeat the header per bank
    if (cart[bank] == 'A' && cart[bank + 1] == 'B' && !cart[bank + 2] && !cart[bank + 3]
        && (cart[bank + 8] | cart[bank + 9] << 8) == text) {
      cart[bank + 2] = usr & 0xFF; cart[bank + 3] = usr >> 8;
    }
  sprintf(buf, "MSX: BASIC loader stub -> INIT $%04X (C-BIOS has no BASIC)", usr); printLog(buf);
}

// ---- settings hooks ----
// Load a .rom/.mx1/.mx2 cartridge into slot 1 (and remove any disk).
// live: settings Ctrl-Enter "mount only" -- the cart goes into slot 1 of the running machine and
// the disk and disk ROM stay as they are (Disk BASIC hooks point into that ROM); the BIOS starts
// the cartridge at the next reset, as when one is plugged into a real MSX that is switched on.
static bool msxLoadCart(const char* path, bool live = false) {
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "MSX: cannot open %s", path); printLog(buf); return false; }
  int len = f.size();
  if (len <= 0 || len > 0x100000) { f.close(); printLog("MSX: cart size out of range"); return false; }
#if BOARD_ROM_IN_FLASH
  // PicoCalc: the image goes to the flash cartridge window (only changed sectors are rewritten).
  // Pull the old cart out first -- its bytes are about to be overwritten in place.
  msxCartEject(1);
  // With the MSX running and the file browser open the heap is down to ~4K, too little for
  // romFlashLoad's own 4K staging buffer; msxScratch (4K of sharedBigBuf) is idle on this board.
  const uint8_t* cb = romFlashLoad(ROMFLASH_CART, f, len, (uint8_t*)msxScratch); f.close();
  if (!cb) { printLog("MSX: cart does not fit in flash"); return false; }
#else
  if (g_cartBuf) { free(g_cartBuf); g_cartBuf = nullptr; }
  uint8_t* cb = (uint8_t*)ps_malloc(len);
  if (!cb) { f.close(); printLog("MSX: cart malloc failed"); return false; }
  int got = f.read(cb, len); f.close();
  if (got != len) { free(cb); printLog("MSX: cart read short"); return false; }
  g_cartBuf = cb;
  msxBasicStubFix(cb, len);
  msxBiosRegionFix(path, cb, len);
#endif
  msxCartLoadImage(1, cb, len);
  g_cartPath = path;
  if (!live) {
    msxDiskClose();                                // flush + close any disk write-back handle
    msx::diskEject(); g_diskPath = "";             // disk off when a cart is inserted
    msxDiskRomUpdate();                            // ...and its ROM, unless DISK ROM is ON
    if (g_diskBuf) { free(g_diskBuf); g_diskBuf = nullptr; }
  }
  sprintf(buf, "MSX: cart %s (%dK) %s", path, len / 1024, live ? "inserted (starts at next reset)" : "loaded");
  printLog(buf);
  return true;
}

// Load the MSX disk-interface ROM from the SD card on first use (cached for the session). The disk
// ROM used to be embedded; it now lives at /roms/hb3600_disk.rom (File-Hunter's name). Returns
// false (disk unmountable) if it is missing or the wrong size.
static bool msxEnsureDiskRom() {
  if (g_diskRomBuf) return true;
  const char* path = romPath(ROMSEL_MSX_DISK);     // settings ROMS page pick, else /roms/hb3600_disk.rom
  if (!*path) { printLog("MSX: no disk ROM set - cannot mount disk"); return false; }
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "MSX: %.80s missing - cannot mount disk", path); printLog(buf); return false; }
  if ((int)f.size() != 0x4000) { f.close(); printLog("MSX: disk ROM wrong size (want 16K)"); return false; }
#if BOARD_ROM_IN_FLASH
  // PicoCalc: no PSRAM, so the disk ROM gets its own 16K flash window next to the BIOS (a cartridge
  // can stay loaded beside it). Read through XIP like the BIOS.
  const uint8_t* fb = romFlashLoad(ROMFLASH_DISKROM, f, 0x4000, (uint8_t*)msxScratch); f.close();   // see msxLoadCart
  if (!fb) { printLog("MSX: disk ROM flash load failed"); return false; }
  g_diskRomBuf = fb;
  return true;
#endif
  uint8_t* b = (uint8_t*)ps_malloc(0x4000);
  if (!b) { f.close(); printLog("MSX: diskrom alloc failed"); return false; }
  int got = f.read(b, 0x4000); f.close();
  if (got != 0x4000) { free(b); return false; }
  g_diskRomBuf = b;
  return true;
}

// Mount a .dsk image and install the disk ROM in slot 2 (and remove any cart). The image is read into
// PSRAM when there is room; otherwise (always on the PicoCalc: a 720K disk is ~6x its free heap) it
// is streamed -- each sector the FDC touches is read from / written to the SD file directly.
// keepCart: a live disk swap (settings MOUNT) leaves the running cartridge alone.
static bool msxMountDiskImage(const char* path, bool keepCart = false) {
  if (!msxEnsureDiskRom()) return false;            // need the disk ROM (from SD) before mounting
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "MSX: cannot open %s", path); printLog(buf); return false; }
  int len = f.size();
  if (len <= 0 || len > 2 * 1024 * 1024) { f.close(); printLog("MSX: disk size out of range"); return false; }
  msx::diskEject();                                 // drop the old image before freeing / closing it
  msxDiskClose();                                   // flush + close any previous write-back handle
  if (g_diskBuf) { free(g_diskBuf); g_diskBuf = nullptr; }
  uint8_t* db = nullptr;
#if !BOARD_ROM_IN_FLASH
  db = (uint8_t*)ps_malloc(len);
  if (db) {
    int rd = 0;
    while (rd < len) { int n = f.read(db + rd, (len - rd > 8192) ? 8192 : (len - rd)); if (n <= 0) break; rd += n; }
    if (rd != len) { free(db); f.close(); printLog("MSX: disk read short"); return false; }
  }
#endif
  f.close();
  g_diskFile = FSTYPE.open(path, "r+");             // random-access read/write (no truncate) for write-back
  g_diskWritable = (bool)g_diskFile;
  if (!g_diskWritable && !db) g_diskFile = FSTYPE.open(path, FILE_READ);   // streamed, read-only
  g_diskFileOpen = (bool)g_diskFile;
  if (!db && !g_diskFileOpen) { sprintf(buf, "MSX: cannot open %s", path); printLog(buf); return false; }
  g_diskBuf = db;
  if (!keepCart) {
    msxCartEject(1);                                // cart off when a disk is booted
    if (g_cartBuf) { free(g_cartBuf); g_cartBuf = nullptr; }
    g_cartPath = "";
    msxBiosRegionFix("", nullptr, 0);               // original BIOS ID bytes
  }
  if (msxDiskRom != MSX_DISKROM_OFF)
    msx::diskSetRom(g_diskRomBuf, 0x4000);          // install the disk ROM (from /roms) in slot 2
  if (db) msx::diskSetImage(db, len);
  else    msx::diskSetStream(len, msxDiskStreamIO);
  g_diskPath = path;
  sprintf(buf, "MSX: disk %s (%dK) mounted%s%s", path, len / 1024, db ? "" : ", streamed from SD",
          g_diskWritable ? "" : " (read-only)");
  printLog(buf);
  return true;
}

// Settings MOUNT / MOUNT & RUN for a .dsk. MOUNT swaps the disk in the running machine (a multi-disk
// game asking for disk 2; a cart keeps running) -- unless the disk ROM was not in slot 2 yet, in which
// case the BIOS never initialised Disk BASIC and only a reset makes the drive usable. MOUNT & RUN
// boots the disk (cart out, reset), same as LOAD & RUN.
bool msxMountDisk(const char* path, bool run) {
  if (run) return msxLoadSelected(path);
  const bool romWasIn = msx::diskRomInstalled();
  if (!msxMountDiskImage(path, true)) return false;
  selectedMsxFileName = path;
  if (!romWasIn && msx::diskRomInstalled()) { printLog("MSX: disk ROM was not active - resetting"); msxResetReq = true; }
  return true;
}

// Settings UNMOUNT: flush and eject the disk, no reset. The disk ROM stays in slot 2 until the next
// reset (the running Disk BASIC has hooks into it); the drive just reports no disk.
void msxUnmountDisk() {
  msx::diskEject();
  msxDiskClose();
  if (g_diskBuf) { free(g_diskBuf); g_diskBuf = nullptr; }
  sprintf(buf, "MSX: disk %s unmounted", g_diskPath.c_str()); printLog(buf);
  if (selectedMsxFileName == g_diskPath) selectedMsxFileName = "";   // do not auto-mount it on boot
  g_diskPath = "";
}

// Settings UNMOUNT for the loaded cartridge: pull it out of slot 1 and reset (the running program
// lived in it), back to BASIC -- or Disk BASIC if the disk ROM is in.
void msxUnloadCart() {
  msxCartEject(1);
  if (g_cartBuf) { free(g_cartBuf); g_cartBuf = nullptr; }
  msxBiosRegionFix("", nullptr, 0);                // original BIOS ID bytes
  sprintf(buf, "MSX: cart %s unmounted", g_cartPath.c_str()); printLog(buf);
  if (selectedMsxFileName == g_cartPath) selectedMsxFileName = "";   // do not auto-load it on boot
  g_cartPath = "";
  msxDiskRomUpdate();                              // back to Disk BASIC (AUTO)
  msxResetReq = true;
}

bool msxCartLoaded(const char* path) {
  return g_cartPath.length() && path && g_cartPath == path;
}

bool msxDiskMounted(const char* path) {
  return msx::diskPresent() && g_diskPath.length() && path && g_diskPath == path;
}

// Put the disk ROM in slot 2 or take it out, per the DISK ROM setting (emu.h) and what is loaded.
// The BIOS only sees the change at the next reset.
// OFF wins even over a mounted disk (the drive is then unreachable, as on an MSX without one).
static void msxDiskRomUpdate() {
  const bool want = msxDiskRom == MSX_DISKROM_ON
                 || (msxDiskRom == MSX_DISKROM_AUTO && (msx::diskPresent() || !g_cartPath.length()));
  if (want && msxEnsureDiskRom()) msx::diskSetRom(g_diskRomBuf, 0x4000);
  else if (!want)                 msx::diskSetRom(nullptr, 0);
  sprintf(buf, "MSX: DISK ROM %s -> %s", msxDiskRom == MSX_DISKROM_ON ? "ON" : msxDiskRom == MSX_DISKROM_OFF ? "OFF" : "AUTO",
          msx::diskRomInstalled() ? "in slot 2" : "out");
  printLog(buf);
}

// Settings DISK ROM toggle. The machine resets so the BIOS rescans the slots.
void msxApplyDiskRom() {
  msxDiskRomUpdate();
  msxResetReq = true;
}

bool msxLoadSelected(const char* path) {
  std::string p = path ? path : "";
  bool ok;
  if (msxEndsCI(p, ".dsk")) ok = msxMountDiskImage(path);
  else if (msxEndsCI(p, ".rom") || msxEndsCI(p, ".mx1") || msxEndsCI(p, ".mx2")) ok = msxLoadCart(path);
  else { printLog("MSX: unsupported file (use .rom/.mx1/.mx2 cart or .dsk disk)"); return false; }
  if (ok) { selectedMsxFileName = path; msxResetReq = true; }
  return ok;
}

// Settings Ctrl-Enter on a .rom/.mx1/.mx2: insert it without a reset (see msxLoadCart).
bool msxMountCart(const char* path) {
  if (!msxLoadCart(path, true)) return false;
  selectedMsxFileName = path;
  return true;
}

void msxScanFiles() { loadMsxFilesSync(); }

// ---- startup overlay: no-BIOS error, or a one-time "C-BIOS = no Disk BASIC" note ----
bool msxRenderLoadWarning() {
  if (msx::biosLen == 0) {                       // hard error: cannot boot
    static bool drawn = false;
    if (OptionsWindow) { drawn = false; return false; }   // SETTINGS > ROMS can pick a BIOS; redraw on close
    if (!drawn) {
      tft.fillScreen(TFT_BLACK);
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK); tft.drawString("MSX: NO BIOS FOUND", 8, 8, 2);
      tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.drawString("Put hotbit12.rom (32K) in /roms", 8, 40, 1);
      tft.drawString("on the SD card,", 8, 56, 1);
      tft.drawString("or pick one in SETTINGS (Ctrl-F1) > ROMS.", 8, 72, 1);
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.drawString(g_biosPath, 8, 96, 1);                     // what was tried, and why it failed
      tft.drawString(*g_biosErr ? g_biosErr : "not loaded", 8, 112, 1);
      tft.setTextDatum(MC_DATUM);
      drawn = true;
    }
    return true;                                 // hold this screen (nothing to run) until SETTINGS opens
  }
  return false;   // embedded/SD BIOS present -> boot straight into it (no overlay)
}

#endif  // BOARD_HAS_MSX_CORE
