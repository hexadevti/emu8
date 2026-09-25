// This ColecoVision translation unit is compiled out entirely on boards that clear
// BOARD_HAS_COLECO_CORE (it needs both the MSX VDP and the SMS PSG -- see board.h).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro.
#include "../../emu.h"
#if BOARD_HAS_COLECO_CORE

// coleco.cpp - device-side glue for the ColecoVision platform: allocation, BIOS + cartridge load from
// SD, the core-0 render push, the core-1 run loop, input injection, and the settings (file browser /
// load) hooks. The only Coleco file that depends on Arduino/board code; the machine itself
// (coleco_machine.cpp) stays host-portable.
//
// Wired into the platform dispatch by:
//   emu8.ino setup()/loop(), src/shared/video.cpp renderLoop(), and src/shared/optionsui.cpp.

#include "coleco.h"
#include "../msx/msx.h"              // TMS9918A: msx::vram / msx::framebuffer / MSX_PALETTE / vdp*()
#include "../sms/sms.h"              // SN76489 (reset from machineReset)
#include "../shared/filebrowser.h"   // shared SD image browser (subdirectories + sorting)

static uint16_t* colScratch = nullptr;   // per-frame RGB565 conversion band (non-PicoCalc path)
#if !BOARD_ROM_IN_FLASH
static uint8_t*  g_romBuf   = nullptr;   // device cartridge image (PSRAM); freed on reload
#endif
static volatile bool colResetReq = false;
static const int C_W = 256, C_H = 192, C_OX = (320 - 256) / 2;

static uint8_t* colAllocFast(size_t n) {                 // internal SRAM first (CPU hot path), PSRAM fallback
  uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t*)ps_malloc(n);
  return p;
}
static bool colEndsCI(const std::string& s, const char* suf) {
  size_t n = strlen(suf); if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i])) return false;
  return true;
}
static bool colEqCI(const std::string& s, const char* t) {
  return s.size() == strlen(t) && colEndsCI(s, t);
}

// ---- BIOS (8K, copyrighted: the user supplies it) -----------------------------------------------
static const char* const BIOS_NAMES[] = { "/roms/coleco/coleco.rom", "/roms/coleco/colecovision.rom",
                                          "/roms/coleco/bios.rom", "/coleco.rom", "/colecovision.rom" };
static bool isBiosName(const std::string& n) {
  return colEqCI(n, "coleco.rom") || colEqCI(n, "colecovision.rom") || colEqCI(n, "bios.rom");
}

static bool loadBiosFromSD() {
  for (const char* nm : BIOS_NAMES) {
    File f = FSTYPE.open(nm, FILE_READ);
    if (!f) continue;
    int len = f.size();
    if (len != coleco::BIOS_SIZE) { f.close(); continue; }
#if BOARD_ROM_IN_FLASH
    // PicoCalc: no PSRAM, so the BIOS goes to spare flash and is read through XIP (a no-op compare
    // on every boot after the first). See src/picocalc/romflash_picocalc.cpp.
    const uint8_t* fb = romFlashLoad(ROMFLASH_BIOS, f, len); f.close();
    if (fb) { coleco::bios = fb; coleco::biosLen = len;
              sprintf(buf, "COLECO: BIOS %s (flash)", nm); printLog(buf); return true; }
    continue;
#else
    uint8_t* b = colAllocFast(len);
    if (!b) { f.close(); continue; }
    int got = f.read(b, len); f.close();
    if (got == len) { coleco::bios = b; coleco::biosLen = len;
                      sprintf(buf, "COLECO: BIOS %s", nm); printLog(buf); return true; }
    free(b);
#endif
  }
  coleco::biosLen = 0;
  printLog("COLECO: NO BIOS - put coleco.rom (8K) in /roms/coleco on the SD card");
  return false;
}

// SD cartridge browser (.col/.rom/.bin + subdirectories); see src/shared/filebrowser.h.
// The BIOS image stays hidden -- it is loaded by name, not picked.
#define COLECO_MAX_FILES 200
static bool colAccept(const std::string &n) {
  if (isBiosName(n)) return false;
  return colEndsCI(n, ".col") || colEndsCI(n, ".rom") || colEndsCI(n, ".bin");
}
static FileBrowser colBrowser = { "COLECO", &colecoFiles, colAccept, nullptr, COLECO_MAX_FILES, "/" };

void loadColecoFilesSync()   { fbScan(colBrowser); }
void colecoBrowseEnter(const char *path) { fbEnter(colBrowser, path); }
void colecoBrowseUp()        { fbUp(colBrowser); }

// ============================ platform entry points =============================================
void colecoSetup() {
  printLog("ColecoVision Setup... (Z80 + TMS9918A VDP + SN76489 PSG)");

#if defined(BOARD_PICOCALC)
  // No PSRAM: the side buffers go in sharedBigBuf past the 49152-byte framebuffer, as on the MSX/SMS
  // -- that tail is only used while an Apple II runs, and a platform switch reboots.
  static_assert(sizeof(sharedBigBuf) >= 256 * 192 + 256 * 8 * 2 + 2 * 0x546,
                "Coleco framebuffer + scratch band + menu buffers must fit in sharedBigBuf");
  colScratch = (uint16_t*)(sharedBigBuf + 256 * 192);
  menuScreen = sharedBigBuf + 256 * 192 + 256 * 8 * 2;
  menuColor  = menuScreen + 0x546;
#else
  menuScreen = (unsigned char*)malloc(0x546);            // shared settings-window text buffers
  menuColor  = (unsigned char*)malloc(0x546);
  colScratch = (uint16_t*)malloc(256 * 8 * sizeof(uint16_t));
#endif

  coleco::ram = colAllocFast(coleco::RAM_SIZE);          // 1 KB work RAM
  msx::vram   = colAllocFast(coleco::VRAM_SIZE);         // 16 KB VDP RAM (the shared TMS9918A core)
  msx::framebuffer = sharedBigBuf;                       // 256*192 = 49152 <= sizeof(sharedBigBuf)
  if (coleco::ram) memset(coleco::ram, 0, coleco::RAM_SIZE);
  if (msx::vram)   memset(msx::vram, 0, coleco::VRAM_SIZE);
  if (!coleco::ram || !msx::vram) {
    sprintf(buf, "COLECO: out of memory for RAM/VRAM (heap=%u)", (unsigned)ESP.getFreeHeap());
    printLog(buf);
    return;                                              // biosLen stays 0: colecoLoop idles
  }

  loadBiosFromSD();
  coleco::machineWire();
  coleco::machineReset();

  if (selectedColecoFileName.length() > 1 && selectedColecoFileName != "/")
    colecoLoadSelected(selectedColecoFileName.c_str());

  sprintf(buf, "COLECO ready. internal free=%u, heap=%u",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)ESP.getFreeHeap());
  printLog(buf);
}

// Same NTSC timing as the SMS: 59736 T-states per 59.92 Hz frame = 16687 us/frame. NORMAL mode paces
// each frame to that wall-clock; FAST mode (colecoFast) runs uncapped.
void colecoLoop() {
  if (coleco::biosLen > 0 && !colResetReq) {             // uncapped benchmark (~0.8 s emulated)
    uint64_t c0 = coleco::cpu.cycles; uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 800) coleco::runFrame();
    uint32_t dt = millis() - t0; uint64_t dc = coleco::cpu.cycles - c0;
    colecoMeasuredMhz = (dt > 0) ? (float)((double)dc / ((double)dt * 1000.0)) : 0.0f;
    sprintf(buf, "COLECO: uncapped Z80 = %.2f MHz (real ColecoVision = 3.58)", colecoMeasuredMhz);
    printLog(buf);
  }

  const uint32_t FRAME_US = 16687;
  uint32_t nextUs = micros();
  for (;;) {
    if (OptionsWindow) { vTaskDelay(pdMS_TO_TICKS(20)); nextUs = micros(); continue; }
    if (colResetReq)   { colResetReq = false; coleco::machineReset(); nextUs = micros(); }
    if (coleco::biosLen == 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }   // no BIOS -> nothing to run

    coleco::runFrame();

    if (colecoFast) {
#if defined(BOARD_DESKTOP)
      taskYIELD();
#else
      vTaskDelay(1);
#endif
      nextUs = micros();
    } else {
      nextUs += FRAME_US;
      int32_t wait = (int32_t)(nextUs - micros());
      if (wait > 0) {
        uint32_t ms = (uint32_t)wait / 1000;
        vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
      } else if (wait < -100000) {
        nextUs = micros();
      } else {
        vTaskDelay(1);
      }
    }
  }
}

// Convert the 256x192 indexed (0..15) framebuffer to RGB565 through the fixed TMS9918A palette and
// push it, exactly like msxRenderFrame. Runs on the core-0 render task (owns the TFT).
void colecoRenderFrame() {
  if (!coleco::frameReady) return;
  if (!colScratch || !msx::framebuffer) { coleco::frameReady = false; return; }
  const uint16_t* pal = msx::MSX_PALETTE;
  const int outTop = oskRasterTop();
  const int outH   = oskRasterHeight();
  const int belowY = outTop + outH;
#if defined(BOARD_PICOCALC)
  // Borders are painted only when something may have drawn over them; SCREEN: FILL scales 5:4 to
  // the whole 320x240 screen (see smsRenderFrame for the details).
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
    tft.fillRect(0, 0, 320, outTop, TFT_BLACK);
    if (!osk) tft.fillRect(0, belowY, 320, 240 - belowY, TFT_BLACK);
    tft.fillRect(0, outTop, C_OX, outH, TFT_BLACK);
    tft.fillRect(C_OX + C_W, outTop, 320 - (C_OX + C_W), outH, TFT_BLACK);
  }
#if defined(BOARD_PICOCALC)
  if (fill) {
    tft.setAddrWindow(0, 0, 320, 240);
    tft.startWrite();
    for (int oy = 0; oy < 240; oy++) {
      const uint8_t* src = msx::framebuffer + (oy * 4 / 5) * C_W;
      for (int x = 0; x < C_W; x += 4) {
        const uint16_t c0 = pal[src[x] & 0x0F];
        tft.writeColor(c0, 1);
        tft.writeColor(c0, 1);
        tft.writeColor(pal[src[x + 1] & 0x0F], 1);
        tft.writeColor(pal[src[x + 2] & 0x0F], 1);
        tft.writeColor(pal[src[x + 3] & 0x0F], 1);
      }
    }
    tft.endWrite();
    coleco::frameReady = false;
    return;
  }
  tft.setAddrWindow(C_OX, outTop, C_W, outH);
  tft.startWrite();
  for (int oy = 0; oy < outH; oy++) {
    int sy = oy * C_H / outH;
    if (sy > C_H - 1) sy = C_H - 1;
    const uint8_t* src = msx::framebuffer + sy * C_W;
    for (int x = 0; x < C_W; x++) tft.writeColor(pal[src[x] & 0x0F], 1);
  }
  tft.endWrite();
#else
  tft.setSwapBytes(true);
  for (int oy = 0; oy < outH; ) {
    int n = 0;
    while (oy + n < outH && n < 8) {
      int sy = (oy + n) * C_H / outH;
      if (sy > C_H - 1) sy = C_H - 1;
      const uint8_t* src = msx::framebuffer + sy * C_W;
      uint16_t* dst = colScratch + n * C_W;
      for (int x = 0; x < C_W; x++) dst[x] = pal[src[x] & 0x0F];
      n++;
    }
    tft.pushImage(C_OX, outTop + oy, C_W, n, colScratch);
    oy += n;
  }
  tft.setSwapBytes(false);
#endif
  coleco::frameReady = false;
}

// colecoPsgSetup() lives in coleco_audio.cpp (it owns the audio output), like smsPsgSetup.

// ---- input (from joystick.cpp / usbkeyboard.cpp) ----
void colecoSetInput(uint8_t joyMask) { coleco::setInput(0, joyMask); }
void colecoSetKeypad(int key)        { coleco::setKeypad(0, key); }
void colecoHardReset()               { colResetReq = true; }   // power-cycle: BIOS title -> cart (F12)

// ---- settings hooks ----
bool colecoLoadSelected(const char* path) {
  std::string p = path ? path : "";
  if (!colEndsCI(p, ".col") && !colEndsCI(p, ".rom") && !colEndsCI(p, ".bin")) {
    printLog("COLECO: unsupported file (use .col/.rom/.bin)"); return false;
  }
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "COLECO: cannot open %s", path); printLog(buf); return false; }
  int len = f.size();
  if (len <= 0 || len > 0x100000) { f.close(); printLog("COLECO: ROM size out of range"); return false; }
#if BOARD_ROM_IN_FLASH
  // PicoCalc: the image goes to the flash cartridge window. The Z80 is parked while this runs
  // (settings open, or not started yet at boot).
  const uint8_t* cb = romFlashLoad(ROMFLASH_CART, f, len); f.close();
  if (!cb) { printLog("COLECO: ROM does not fit in flash"); return false; }
#else
  if (g_romBuf) { free(g_romBuf); g_romBuf = nullptr; }
  uint8_t* cb = (uint8_t*)ps_malloc(len);
  if (!cb) { f.close(); printLog("COLECO: ROM malloc failed"); return false; }
  int got = f.read(cb, len); f.close();
  if (got != len) { free(cb); printLog("COLECO: ROM read short"); return false; }
  g_romBuf = cb;
#endif
  coleco::cartSetImage(cb, len);
  selectedColecoFileName = path;
  colResetReq = true;
  sprintf(buf, "COLECO: %s (%dK%s) loaded", path, len / 1024, len > 0x8000 ? ", MegaCart" : "");
  printLog(buf);
  return true;
}

void colecoScanFiles() { loadColecoFilesSync(); }

// ---- startup overlay: no BIOS, or no cartridge ----
// Without a BIOS nothing can run. Without a cartridge the BIOS would just sit on its "turn power off"
// screen, so we say what to do instead. Like the SMS overlay it yields to SETTINGS (tap / F10 / pad
// menu combo) and redraws once each time the settings window closes.
bool colecoRenderLoadWarning() {
  const bool noBios = coleco::biosLen == 0;
  const bool noCart = coleco::romLen == 0;
  if (!noBios && !noCart) return false;
  static bool drawn = false;
  if (OptionsWindow) { drawn = false; return false; }
  if (!drawn) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK);
    if (noBios) {
      tft.drawString("COLECO: NO BIOS FOUND", 8, 8, 2);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("Put the 8K ColecoVision BIOS on the SD card", 8, 40, 1);
      tft.drawString("as /roms/coleco/coleco.rom and restart.", 8, 56, 1);
    } else {
      tft.drawString("COLECO: NO CARTRIDGE LOADED", 8, 8, 2);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("Put a .col / .rom cartridge on the SD card,", 8, 40, 1);
      tft.drawString("then tap the screen to open SETTINGS and pick it.", 8, 56, 1);
    }
    tft.setTextDatum(MC_DATUM);
    drawn = true;
  }
  return true;
}

#endif  // BOARD_HAS_COLECO_CORE
