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

// ---- per-frame RGB565 conversion band (malloc'd on the MSX path only, like nesScratch) ----------
static uint16_t* msxScratch = nullptr;
static uint8_t*  g_cartBuf  = nullptr;   // device cartridge image (PSRAM); freed on reload
static uint8_t*  g_diskBuf  = nullptr;   // mounted .dsk image (PSRAM); freed on remount
static uint8_t*  g_diskRomBuf = nullptr;  // /roms/msx/diskrom.rom (16K), loaded once on first mount
static File      g_diskFile;             // persistent r+ handle for SD write-back of dirty sectors
static bool      g_diskFileOpen = false;

// Drain up to maxSectors dirty sectors of the mounted .dsk back to the SD file (called from msxLoop,
// core 1; the bus lock serializes the SD access against the touch reads on core 0).
static void msxDiskFlush(int maxSectors) {
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
  g_diskFile.close();
  g_diskFileOpen = false;
}
volatile bool msxResetReq = false;   // set by the desktop debugger (debug_bridge.cpp) for an in-process reset
static const int M_W = 256, M_H = 192, M_OX = (320 - 256) / 2;

static uint8_t* msxAllocFast(size_t n) {                 // internal SRAM first (CPU hot path), PSRAM fallback
  uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t*)ps_malloc(n);
  return p;
}
static bool msxEndsCI(const std::string& s, const char* suf) {
  size_t n = strlen(suf); if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i])) return false;
  return true;
}
static bool isBiosName(const std::string& nm) {
  return msxEndsCI(nm, "msxbios.rom") || msxEndsCI(nm, "/msx.rom") || nm == "MSX.ROM" || nm == "msx.rom"
      || msxEndsCI(nm, "cbios_main_msx1.rom");
}

// ---- SD BIOS discovery + C-BIOS fallback -------------------------------------------------------
static bool loadBiosFromSD() {
  // /roms/msx/ first (the C-BIOS dumped from the old embedded array now lives there), then the
  // legacy SD-root names for a user-supplied BIOS. There is no embedded fallback any more - the
  // ROMs live on the card (see msx_cbios.cpp / the roms-to-sd refactor).
  const char* names[] = { "/roms/msx/cbios.rom", "/roms/msx/msxbios.rom",
                          "/MSXBIOS.ROM", "/msxbios.rom", "/MSX.ROM", "/msx.rom", "/CBIOS_MAIN_MSX1.ROM" };
  for (const char* nm : names) {
    File f = FSTYPE.open(nm, FILE_READ);
    if (!f) continue;
    int len = f.size();
    if (len >= 0x4000 && len <= 0x8000) {
#if BOARD_ROM_IN_FLASH
      // PicoCalc: no PSRAM, so the BIOS is copied into spare flash and read through XIP (a no-op
      // compare on every boot after the first). See src/picocalc/romflash_picocalc.cpp.
      const uint8_t* fb = romFlashLoad(ROMFLASH_BIOS, f, len); f.close();
      if (fb) { msx::bios = fb; msx::biosLen = len;
                msx::biosIsCbios = strstr(nm, "cbios") || strstr(nm, "CBIOS");
                sprintf(buf, "MSX: BIOS %s (%dK, flash)", nm, len / 1024); printLog(buf); return true; }
      continue;
#endif
      uint8_t* b = msxAllocFast(0x8000);
      if (b) {
        int got = f.read(b, len); f.close();
        if (got == len) { msx::bios = b; msx::biosLen = len;
                          msx::biosIsCbios = strstr(nm, "cbios") || strstr(nm, "CBIOS");
                          sprintf(buf, "MSX: BIOS %s (%dK)", nm, len / 1024); printLog(buf); return true; }
        free(b);
      } else f.close();
    } else f.close();
  }
  msx::biosLen = 0;
  printLog("MSX: NO BIOS - put cbios.rom (32K) in /roms/msx on the SD card");
  return false;
}

// Scan SD root for *.rom / *.dsk into msxFiles (excluding the reserved BIOS names).
#define MSX_MAX_FILES 200
// SD ROM/disk browser (.rom/.mx1/.dsk + subdirectories); see src/shared/filebrowser.h.
// The BIOS images stay hidden -- they are loaded by name, not picked.
static bool msxAccept(const std::string &n) {
  if (isBiosName(n)) return false;
  return msxEndsCI(n, ".rom") || msxEndsCI(n, ".mx1") || msxEndsCI(n, ".dsk");
}
static FileBrowser msxBrowser = { "MSX", &msxFiles, msxAccept, nullptr, MSX_MAX_FILES, "/" };

void loadMsxFilesSync()      { fbScan(msxBrowser); }
void msxBrowseEnter(const char *path) { fbEnter(msxBrowser, path); }
void msxBrowseUp()           { fbUp(msxBrowser); }

// ============================ platform entry points =============================================
void msxSetup() {
  printLog("MSX1 Setup... (Z80 + TMS9918 VDP + AY-3-8910 PSG + 8255 PPI)");

#if defined(BOARD_PICOCALC)
  // No PSRAM, so the heap has to hold the 80K of RAM + VRAM below. The ~6.8K of side buffers go in
  // sharedBigBuf past the 49152-byte framebuffer instead: that tail is only used while an Apple II
  // runs (its IIe banks and speaker stack), and a platform switch reboots.
  static_assert(sizeof(sharedBigBuf) >= 256 * 192 + 256 * 8 * 2 + 2 * 0x546,
                "MSX framebuffer + scratch band + menu buffers must fit in sharedBigBuf");
  msxScratch = (uint16_t*)(sharedBigBuf + 256 * 192);           // 4096 bytes, 16-bit aligned
  menuScreen = sharedBigBuf + 256 * 192 + 256 * 8 * 2;
  menuColor  = menuScreen + 0x546;
#else
  // shared text UI buffers (settings window) - allocate like the C64/NES/Atari paths
  menuScreen = (unsigned char*)malloc(0x546);
  menuColor  = (unsigned char*)malloc(0x546);
  msxScratch = (uint16_t*)malloc(256 * 8 * sizeof(uint16_t));
#endif

  msx::ram  = msxAllocFast(0x10000);            // 64 KB work RAM (slot 3) - keep internal for speed
  msx::vram = msxAllocFast(msx::VRAM_SIZE);     // 16 KB VDP RAM - read every frame
  msx::framebuffer = sharedBigBuf;              // 256*192 = 49152 <= sizeof(sharedBigBuf)
  if (msx::ram)  memset(msx::ram, 0, 0x10000);
  if (msx::vram) memset(msx::vram, 0, msx::VRAM_SIZE);
  if (!msx::ram || !msx::vram) {
    sprintf(buf, "MSX: out of memory for RAM/VRAM (heap=%u)", (unsigned)ESP.getFreeHeap());
    printLog(buf);
    msx::biosLen = 0;                            // msxLoop idles on biosLen == 0 instead of crashing
    return;
  }

  loadBiosFromSD();                              // SD BIOS, else embedded C-BIOS, else biosLen=0 (warning)

  msx::machineWire();
  msx::machineReset();

  // auto-load the saved cartridge (.rom/.mx1) or mount the saved disk (.dsk) on boot
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
    uint64_t c0 = msx::cpu.cycles; uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 800) msx::runFrame();
    uint32_t dt = millis() - t0; uint64_t dc = msx::cpu.cycles - c0;
    msxMeasuredMhz = (dt > 0) ? (float)((double)dc / ((double)dt * 1000.0)) : 0.0f;
    sprintf(buf, "MSX: uncapped Z80 = %.2f MHz (real MSX1 = 3.58)", msxMeasuredMhz);
    printLog(buf);
  }

  const uint32_t FRAME_US = 16687;
  uint32_t nextUs = micros();
  for (;;) {
    if (OptionsWindow) { vTaskDelay(pdMS_TO_TICKS(20)); nextUs = micros(); continue; }  // paused in settings
    if (msxResetReq)   { msxResetReq = false; msx::machineReset(); nextUs = micros(); }
    if (msx::biosLen == 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }                 // no BIOS -> nothing to run

    msx::runFrame();
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
void msxRenderFrame() {
  // The VDP framebuffer is rendered on the CPU core (msx::runFrame). Display it only when a fresh
  // frame is ready, and clear the flag when done so core 1 can render the next one -- this
  // handshake guarantees the two cores never touch the framebuffer simultaneously (no tearing).
  if (!msx::frameReady) return;                 // no new frame; the panel keeps the last one
  if (!msxScratch || !msx::framebuffer) { msx::frameReady = false; return; }
  const uint16_t* pal = msx::MSX_PALETTE;
  const int outTop = oskRasterTop();      // 0 when the keyboard is open, else 24 (centered)
  const int outH   = oskRasterHeight();   // keyboard-top (112) when open, else 192 (1:1)
  const int belowY = outTop + outH;
#if defined(BOARD_PICOCALC)
  // PicoCalc: SPI is the bottleneck (50MHz, ~16ms for the 256x192 picture alone), so the black
  // borders -- another ~28K pixels -- are painted only when something may have drawn over them
  // (a menu closed -> clearScr) or the raster moved, not every frame.
  // SCREEN: FILL (Settings) scales the picture 5:4 on both axes to the whole 320x240 logical screen,
  // so there are no borders to paint -- but leaving FILL must repaint them over the old picture.
  extern bool screenFill;
  static int  lastTop = -1, lastH = -1;
  static bool lastOsk = false, lastFill = false;
  const bool osk  = oskActive();
  const bool fill = screenFill && !osk;
  const bool borders = !fill && (clearScr || outTop != lastTop || outH != lastH || osk != lastOsk || lastFill);
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
#if defined(BOARD_PICOCALC)
  // One address window for the whole picture, pixels streamed through writeColor()'s ping-pong DMA
  // staging: the palette lookup of one line overlaps the SPI transfer of the previous one, instead
  // of convert-then-wait per 8-line band.
  if (fill) {
    // 256x192 -> 320x240, nearest neighbour: each 4 source lines become 5 (sy = oy*4/5) and each
    // 4 source pixels become 5 (the first one doubled). ~57% more SPI time than 1:1.
    tft.setAddrWindow(0, 0, 320, 240);
    tft.startWrite();
    for (int oy = 0; oy < 240; oy++) {
      const uint8_t* src = msx::framebuffer + (oy * 4 / 5) * M_W;
      for (int x = 0; x < M_W; x += 4) {
        const uint16_t c0 = pal[src[x] & 0x0F];
        tft.writeColor(c0, 1);
        tft.writeColor(c0, 1);
        tft.writeColor(pal[src[x + 1] & 0x0F], 1);
        tft.writeColor(pal[src[x + 2] & 0x0F], 1);
        tft.writeColor(pal[src[x + 3] & 0x0F], 1);
      }
    }
    tft.endWrite();
    msx::frameReady = false;
    return;
  }
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
  msx::frameReady = false;     // done reading the framebuffer; core 1 may render the next frame
}

// msxPsgSetup() lives in msx_audio.cpp (it owns the I2S driver), like atariAudioSetup/sidSetup.

// ---- input (from joystick.cpp / touchkeyboard.cpp / usbkeyboard.cpp) ----
void msxKeyMatrix(uint8_t row, uint8_t col, bool down) { msx::kbSetKey(row, col, down); }
void msxSetInput(uint8_t joyMask) { msx::setJoystick(joyMask); }

// ---- settings hooks ----
// Load a .rom/.mx1 cartridge into slot 1 (and remove any disk).
static bool msxLoadCart(const char* path) {
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "MSX: cannot open %s", path); printLog(buf); return false; }
  int len = f.size();
  if (len <= 0 || len > 0x100000) { f.close(); printLog("MSX: cart size out of range"); return false; }
#if BOARD_ROM_IN_FLASH
  // PicoCalc: the image goes to the flash cartridge window (only changed sectors are rewritten).
  // Pull the old cart out first -- its bytes are about to be overwritten in place.
  msxCartEject(1);
  const uint8_t* cb = romFlashLoad(ROMFLASH_CART, f, len); f.close();
  if (!cb) { printLog("MSX: cart does not fit in flash"); return false; }
#else
  if (g_cartBuf) { free(g_cartBuf); g_cartBuf = nullptr; }
  uint8_t* cb = (uint8_t*)ps_malloc(len);
  if (!cb) { f.close(); printLog("MSX: cart malloc failed"); return false; }
  int got = f.read(cb, len); f.close();
  if (got != len) { free(cb); printLog("MSX: cart read short"); return false; }
  g_cartBuf = cb;
#endif
  msxCartLoadImage(1, cb, len);
  msxDiskClose();                                  // flush + close any disk write-back handle
  msx::diskSetRom(nullptr, 0); msx::diskEject();   // disk off when a cart is inserted
  if (g_diskBuf) { free(g_diskBuf); g_diskBuf = nullptr; }
  sprintf(buf, "MSX: cart %s (%dK) loaded", path, len / 1024);
  printLog(buf);
  return true;
}

// Load the MSX disk-interface ROM from the SD card on first use (cached for the session). The disk
// ROM used to be embedded; it now lives at /roms/msx/diskrom.rom. Returns false (disk unmountable)
// if it is missing or the wrong size.
static bool msxEnsureDiskRom() {
  if (g_diskRomBuf) return true;
  File f = FSTYPE.open("/roms/msx/diskrom.rom", FILE_READ);
  if (!f) { printLog("MSX: /roms/msx/diskrom.rom missing - cannot mount disk"); return false; }
  if ((int)f.size() != 0x4000) { f.close(); printLog("MSX: diskrom.rom wrong size (want 16K)"); return false; }
  uint8_t* b = (uint8_t*)ps_malloc(0x4000);
  if (!b) { f.close(); printLog("MSX: diskrom alloc failed"); return false; }
  int got = f.read(b, 0x4000); f.close();
  if (got != 0x4000) { free(b); return false; }
  g_diskRomBuf = b;
  return true;
}

// Mount a .dsk image (read into PSRAM) and install the C-DISK ROM in slot 2 (and remove any cart).
static bool msxMountDiskImage(const char* path) {
  if (!msxEnsureDiskRom()) return false;            // need the disk ROM (from SD) before mounting
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "MSX: cannot open %s", path); printLog(buf); return false; }
  int len = f.size();
  if (len <= 0 || len > 2 * 1024 * 1024) { f.close(); printLog("MSX: disk size out of range"); return false; }
  if (g_diskBuf) { free(g_diskBuf); g_diskBuf = nullptr; }
  uint8_t* db = (uint8_t*)ps_malloc(len);
  if (!db) { f.close(); printLog("MSX: disk malloc failed"); return false; }
  int rd = 0;
  while (rd < len) { int n = f.read(db + rd, (len - rd > 8192) ? 8192 : (len - rd)); if (n <= 0) break; rd += n; }
  f.close();
  if (rd != len) { free(db); printLog("MSX: disk read short"); return false; }
  g_diskBuf = db;
  msxCartEject(1);                                  // cart off when a disk is mounted
  if (g_cartBuf) { free(g_cartBuf); g_cartBuf = nullptr; }
  msx::diskSetRom(g_diskRomBuf, 0x4000);            // install the HB3600 disk ROM (from /roms/msx) in slot 2
  msx::diskSetImage(db, len);
  msxDiskClose();                                   // close any previous write-back handle
  g_diskFile = FSTYPE.open(path, "r+");             // random-access read/write (no truncate) for write-back
  g_diskFileOpen = (bool)g_diskFile;
  sprintf(buf, "MSX: disk %s (%dK) mounted%s", path, len / 1024, g_diskFileOpen ? "" : " (write-back off)");
  printLog(buf);
  return true;
}

bool msxLoadSelected(const char* path) {
  std::string p = path ? path : "";
  bool ok;
  if (msxEndsCI(p, ".dsk")) ok = msxMountDiskImage(path);
  else if (msxEndsCI(p, ".rom") || msxEndsCI(p, ".mx1")) ok = msxLoadCart(path);
  else { printLog("MSX: unsupported file (use .rom/.mx1 cart or .dsk disk)"); return false; }
  if (ok) { selectedMsxFileName = path; msxResetReq = true; }
  return ok;
}

void msxScanFiles() { loadMsxFilesSync(); }

// ---- startup overlay: no-BIOS error, or a one-time "C-BIOS = no Disk BASIC" note ----
bool msxRenderLoadWarning() {
  if (msx::biosLen == 0) {                       // hard error: cannot boot
    static bool drawn = false;
    if (!drawn) {
      tft.fillScreen(TFT_BLACK);
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK); tft.drawString("MSX: NO BIOS FOUND", 8, 8, 2);
      tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.drawString("Put MSXBIOS.ROM (32K) on the SD card root,", 8, 40, 1);
      tft.drawString("or embed C-BIOS (see src/msx/msx_cbios.cpp).", 8, 56, 1);
      tft.setTextDatum(MC_DATUM);
      drawn = true;
    }
    return true;                                 // hold this screen forever (nothing to run)
  }
  return false;   // embedded/SD BIOS present -> boot straight into it (no overlay)
}

#endif  // BOARD_HAS_MSX_CORE
