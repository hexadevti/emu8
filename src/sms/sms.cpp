// This SMS translation unit is compiled out entirely on boards that clear
// BOARD_HAS_SMS_CORE (currently none -- see board.h; it is on for every board).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro;
// the guard below then empties the file, keeping the Z80 core's static RAM out of the image.
#include "../../emu.h"
#if BOARD_HAS_SMS_CORE

// sms.cpp - device-side glue for the Sega Master System platform: allocation, ROM load from SD, the
// core-0 render push, the core-1 run loop, input injection, and the settings (file browser / load)
// hooks. This is the only SMS file that pulls in emu.h (Arduino/board), keeping the rest of src/sms/
// host-portable. SMS boots the cartridge directly - there is no BIOS to load.
//
// Wired into the platform dispatch by:
//   emu8.ino setup()/loop(), src/shared/video.cpp renderLoop(), and src/shared/optionsui.cpp.

#include "../../emu.h"
#include "sms.h"
#include "sms_cart.h"
#include "../shared/filebrowser.h"   // shared SD image browser (subdirectories + sorting)

static uint16_t* smsScratch = nullptr;   // per-frame RGB565 conversion band (like nesScratch/msxScratch)
#if !BOARD_ROM_IN_FLASH
static uint8_t*  g_romBuf   = nullptr;   // device cartridge image (PSRAM); freed on reload
#endif
static volatile bool smsResetReq = false;
static const int S_W = 256, S_H = 192, S_OX = (320 - 256) / 2;

static uint8_t* smsAllocFast(size_t n) {                 // internal SRAM first (CPU hot path), PSRAM fallback
  uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t*)ps_malloc(n);
  return p;
}
static bool smsEndsCI(const std::string& s, const char* suf) {
  size_t n = strlen(suf); if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i])) return false;
  return true;
}

// Scan SD root for *.sms / *.bin into smsFiles.
#define SMS_MAX_FILES 200
// SD ROM browser (.sms/.bin + subdirectories); see src/shared/filebrowser.h.
static bool smsAccept(const std::string &n) {
  return smsEndsCI(n, ".sms") || smsEndsCI(n, ".bin");
}
static FileBrowser smsBrowser = { "SMS", &smsFiles, smsAccept, nullptr, SMS_MAX_FILES, "/" };

void loadSmsFilesSync()      { fbScan(smsBrowser); }
void smsBrowseEnter(const char *path) { fbEnter(smsBrowser, path); }
void smsBrowseUp()           { fbUp(smsBrowser); }

// ============================ platform entry points =============================================
void smsSetup() {
  printLog("SMS Setup... (Z80 + 315-5124 VDP + SN76489 PSG)");

#if defined(BOARD_PICOCALC)
  // No PSRAM: the side buffers go in sharedBigBuf past the 49152-byte framebuffer, as on the MSX
  // (msxSetup) -- that tail is only used while an Apple II runs, and a platform switch reboots.
  static_assert(sizeof(sharedBigBuf) >= 256 * 192 + 256 * 8 * 2 + 2 * 0x546,
                "SMS framebuffer + scratch band + menu buffers must fit in sharedBigBuf");
  smsScratch = (uint16_t*)(sharedBigBuf + 256 * 192);           // 4096 bytes, 16-bit aligned
  menuScreen = sharedBigBuf + 256 * 192 + 256 * 8 * 2;
  menuColor  = menuScreen + 0x546;
#else
  menuScreen = (unsigned char*)malloc(0x546);            // shared settings-window text buffers
  menuColor  = (unsigned char*)malloc(0x546);
  smsScratch = (uint16_t*)malloc(256 * 8 * sizeof(uint16_t));
#endif

  sms::ram  = smsAllocFast(sms::WRAM_SIZE);              // 8 KB work RAM - keep internal for speed
  sms::vram = smsAllocFast(sms::VRAM_SIZE);              // 16 KB VDP RAM - read every frame
  sms::framebuffer = sharedBigBuf;                       // 256*192 = 49152 <= sizeof(sharedBigBuf)
  if (sms::ram)  memset(sms::ram, 0, sms::WRAM_SIZE);
  if (sms::vram) memset(sms::vram, 0, sms::VRAM_SIZE);
  if (!sms::ram || !sms::vram) {
    sprintf(buf, "SMS: out of memory for RAM/VRAM (heap=%u)", (unsigned)ESP.getFreeHeap());
    printLog(buf);
    return;                                              // romLen stays 0: smsLoop idles, overlay shows
  }

  sms::machineWire();
  sms::machineReset();

  if (selectedSmsFileName.length() > 1 && selectedSmsFileName != "/")
    smsLoadSelected(selectedSmsFileName.c_str());

  sprintf(buf, "SMS ready. internal free=%u, heap=%u",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)ESP.getFreeHeap());
  printLog(buf);
}

// NTSC SMS runs the Z80 at 3.579545 MHz: 59736 T-states per 59.92 Hz frame = 16687 us/frame. NORMAL
// mode paces each frame to that wall-clock; FAST mode (smsFast) runs uncapped. A one-time boot
// benchmark reports the host's effective Z80 speed.
void smsLoop() {
  if (sms::romLen > 0) {                                 // uncapped benchmark (~0.8 s emulated)
    uint64_t c0 = sms::cpu.cycles; uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 800) sms::runFrame();
    uint32_t dt = millis() - t0; uint64_t dc = sms::cpu.cycles - c0;
    smsMeasuredMhz = (dt > 0) ? (float)((double)dc / ((double)dt * 1000.0)) : 0.0f;
    sprintf(buf, "SMS: uncapped Z80 = %.2f MHz (real SMS = 3.58)", smsMeasuredMhz);
    printLog(buf);
  }

  const uint32_t FRAME_US = 16687;
  uint32_t nextUs = micros();
  for (;;) {
    if (OptionsWindow) { vTaskDelay(pdMS_TO_TICKS(20)); nextUs = micros(); continue; }
    if (smsResetReq)   { smsResetReq = false; sms::machineReset(); nextUs = micros(); }
    if (sms::romLen == 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }   // no ROM -> nothing to run

    sms::runFrame();

    if (smsFast) {
#if defined(BOARD_DESKTOP)
      taskYIELD();             // desktop: full host speed (render/input on other threads)
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

// Convert the 256x192 indexed (0..31) framebuffer to RGB565 via the live CRAM palette and push it in
// 8-line bands, like msxRenderFrame. Runs on the core-0 render task (owns the TFT).
void smsRenderFrame() {
  if (!sms::frameReady) return;
  if (!smsScratch || !sms::framebuffer) { sms::frameReady = false; return; }
  uint16_t pal[32];
  sms::vdpBuildPalette(pal);
  // SMS "blank leftmost column" (R0 bit5): games hide the 8px column where new tiles scroll in. On a
  // real SMS it shows the backdrop colour (lost in TV overscan); on the LCD we render it black so it
  // merges with the side border instead of looking like an unfilled strip.
  const bool hideLeft8 = (sms::vdpRegister(0) & 0x20) != 0;
  const int outTop = oskRasterTop();
  const int outH   = oskRasterHeight();
  const int belowY = outTop + outH;
#if defined(BOARD_PICOCALC)
  // PicoCalc: SPI is the bottleneck (50MHz, ~16ms for the 256x192 picture alone), so the black
  // borders -- another ~28K pixels -- are painted only when something may have drawn over them
  // (a menu closed -> clearScr) or the raster moved, not every frame.
  static int  lastTop = -1, lastH = -1;
  static bool lastOsk = false;
  const bool osk = oskActive();
  const bool borders = clearScr || outTop != lastTop || outH != lastH || osk != lastOsk;
  clearScr = false;
  lastTop = outTop; lastH = outH; lastOsk = osk;
#else
  const bool borders = true;
  const bool osk = oskActive();
#endif
  if (borders) {
    tft.fillRect(0, 0, 320, outTop, TFT_BLACK);
    if (!osk) tft.fillRect(0, belowY, 320, 240 - belowY, TFT_BLACK);
    tft.fillRect(0, outTop, S_OX, outH, TFT_BLACK);
    tft.fillRect(S_OX + S_W, outTop, 320 - (S_OX + S_W), outH, TFT_BLACK);
  }
#if defined(BOARD_PICOCALC)
  // One address window for the whole picture, pixels streamed through writeColor()'s ping-pong DMA
  // staging: the palette lookup of one line overlaps the SPI transfer of the previous one, instead
  // of convert-then-wait per 8-line band.
  tft.setAddrWindow(S_OX, outTop, S_W, outH);
  tft.startWrite();
  for (int oy = 0; oy < outH; oy++) {
    int sy = oy * S_H / outH;
    if (sy > S_H - 1) sy = S_H - 1;
    const uint8_t* src = sms::framebuffer + sy * S_W;
    int x = 0;
    if (hideLeft8) { for (; x < 8; x++) tft.writeColor(0x0000, 1); }
    for (; x < S_W; x++) tft.writeColor(pal[src[x] & 0x1F], 1);
  }
  tft.endWrite();
#else
  tft.setSwapBytes(true);
  for (int oy = 0; oy < outH; ) {
    int n = 0;
    while (oy + n < outH && n < 8) {
      int sy = (oy + n) * S_H / outH;
      if (sy > S_H - 1) sy = S_H - 1;
      const uint8_t* src = sms::framebuffer + sy * S_W;
      uint16_t* dst = smsScratch + n * S_W;
      for (int x = 0; x < S_W; x++) dst[x] = (hideLeft8 && x < 8) ? 0x0000 : pal[src[x] & 0x1F];
      n++;
    }
    tft.pushImage(S_OX, outTop + oy, S_W, n, smsScratch);
    oy += n;
  }
  tft.setSwapBytes(false);
#endif
  sms::frameReady = false;
}

// smsPsgSetup() lives in sms_audio.cpp (it owns the I2S driver), like msxPsgSetup/atariAudioSetup.

// ---- input (from joystick.cpp / usbkeyboard.cpp) ----
void smsSetInput(uint8_t joyMask) { sms::setInput1(joyMask); }
void smsPauseButton() { sms::smsPause(); }            // PAUSE button -> Z80 NMI (F11)
void smsHardReset()   { smsResetReq = true; }         // soft power-cycle: re-run the cart (F12)

// ---- settings hooks ----
bool smsLoadSelected(const char* path) {
  std::string p = path ? path : "";
  if (!smsEndsCI(p, ".sms") && !smsEndsCI(p, ".bin")) { printLog("SMS: unsupported file (use .sms/.bin)"); return false; }
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "SMS: cannot open %s", path); printLog(buf); return false; }
  int len = f.size();
  int skip = ((len & 0x3FFF) == 512) ? 512 : 0;          // strip the optional 512-byte .sms header
  len -= skip;
  if (len <= 0 || len > 0x100000) { f.close(); printLog("SMS: ROM size out of range"); return false; }
  if (skip) f.seek(skip);
#if BOARD_ROM_IN_FLASH
  // PicoCalc: the image goes to the flash cartridge window (only changed sectors are rewritten).
  // The Z80 is parked while this runs (settings open, or not started yet at boot), and the old
  // pointer only ever reaches into that same flash, so there is nothing to unmap first.
  const uint8_t* cb = romFlashLoad(ROMFLASH_CART, f, len); f.close();
  if (!cb) { printLog("SMS: ROM does not fit in flash"); return false; }
#else
  if (g_romBuf) { free(g_romBuf); g_romBuf = nullptr; }
  uint8_t* cb = (uint8_t*)ps_malloc(len);
  if (!cb) { f.close(); printLog("SMS: ROM malloc failed"); return false; }
  int got = f.read(cb, len); f.close();
  if (got != len) { free(cb); printLog("SMS: ROM read short"); return false; }
  g_romBuf = cb;
#endif
  smsCartLoadImage(cb, len);
  selectedSmsFileName = path;
  smsResetReq = true;
  sprintf(buf, "SMS: %s (%dK) loaded%s", path, len / 1024, skip ? " [hdr stripped]" : "");
  printLog(buf);
  return true;
}

void smsScanFiles() { loadSmsFilesSync(); }

// ---- startup overlay: no ROM loaded ----
// Held while no cartridge is loaded, but it must NOT lock the user out: it yields to the settings
// window (so the ROM browser can take over) and the render loop still polls touch over it, so a tap /
// F10 / gamepad menu-combo opens SETTINGS. Redraws once each time we return from the settings window.
bool smsRenderLoadWarning() {
  if (sms::romLen != 0) return false;                  // a cartridge is loaded -> run the game
  static bool drawn = false;
  if (OptionsWindow) { drawn = false; return false; }  // let SETTINGS own the screen; redraw on return
  if (!drawn) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK); tft.drawString("SMS: NO ROM LOADED", 8, 8, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.drawString("Put a .sms / .bin ROM on the SD card root,", 8, 40, 1);
    tft.drawString("then tap the screen to open SETTINGS and pick it.", 8, 56, 1);
    tft.setTextDatum(MC_DATUM);
    drawn = true;
  }
  return true;
}

#endif  // BOARD_HAS_SMS_CORE
