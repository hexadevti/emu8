// emu.h must be included FIRST because it is what pulls in board.h and defines the macro.
#include "../../emu.h"
#if BOARD_HAS_ZX_CORE

// zx.cpp - device-side glue for the ZX Spectrum 48K: buffers, ROM load from SD, the core-0 render
// push, the core-1 run loop, snapshot (.SNA/.Z80) and tape (.TAP/.TZX) loading, and the settings
// (file browser / load) hooks. The machine itself is zx_machine.cpp.
//
// Wired into the platform dispatch by:
//   emu8.ino setup()/loop(), src/shared/video.cpp renderLoop(), and src/shared/optionsui.cpp.

#include "zx.h"
#include "../shared/filebrowser.h"   // shared SD image browser (subdirectories + sorting)
#include "../shared/dirty_bands.h"     // PicoCalc: push only the bands that changed
#include "../shared/speedmeter.h"     // live Z80 MHz for the settings title

// ---- buffers --------------------------------------------------------------------------------------
// Everything lives in sharedBigBuf, on every board: the 48K RAM first (so no heap at all for it),
// then the screen snapshot, the per-row border colours, the settings-window text buffers, the tape
// block index and one output line. That tail of sharedBigBuf is only ever used by the running
// platform, and a platform switch reboots.
static const int OFF_SCREEN = zx::RAM_SIZE;
static const int OFF_BORDER = OFF_SCREEN + zx::SCR_SIZE;
static const int OFF_MENU   = OFF_BORDER + zx::OUT_H;
static const int OFF_TAPE   = OFF_MENU + 2 * 0x546;
static const int TAPE_MAX_BLOCKS = 256;
static const int OFF_LINE   = OFF_TAPE + TAPE_MAX_BLOCKS * (int)sizeof(zx::TapeBlock);
static const int OFF_BEEP   = OFF_LINE + zx::OUT_W * 2;
static const int OFF_END    = OFF_BEEP + zx::BEEP_RING;
static_assert(OFF_TAPE % 4 == 0, "tape index must be 4-byte aligned");
static_assert(OFF_END <= (int)sizeof(sharedBigBuf), "ZX RAM + side buffers must fit in sharedBigBuf");

static uint16_t* zxLine    = nullptr;   // one 320-pixel output row
static uint16_t* zxScratch = nullptr;   // 320x8 RGB565 band for pushImage (not the PicoCalc)
#if !BOARD_ROM_IN_FLASH
static uint8_t*  g_tapeBuf = nullptr;   // resident tape image; freed on reload
#endif
static volatile bool zxResetReq = false;
static volatile bool zxLoopRunning = false;
static volatile bool zxParked = false;  // the loop is idling in the settings branch (safe to load)
static bool     zxSnapIn = false;       // selectedZxFileName is a snapshot that is running in RAM
static uint16_t zxPal[16];              // RGB565: 0-7 normal, 8-15 BRIGHT
static bool     zxPalColor = true;

static bool zxEndsCI(const std::string& s, const char* suf) {
  size_t n = strlen(suf); if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i])) return false;
  return true;
}

static void zxBuildPalette(bool color) {
  for (int i = 0; i < 16; i++) {
    const int v = (i & 8) ? 0xFF : 0xD7;
    int r = (i & 2) ? v : 0, g = (i & 4) ? v : 0, b = (i & 1) ? v : 0;
    if (!color) { int y = (r * 30 + g * 59 + b * 11) / 100; r = g = b = y; }
    zxPal[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  zxPalColor = color;
}

// ---- ROM (16K, loaded from the SD card) -----------------------------------------------------------
static bool loadRomFromSD() {
  // Exactly one file: the ROM picked on the settings ROMS page, else /roms/48.rom.
  char picked[ROMSEL_SLOT_LEN];
  strcpy(picked, romPath(ROMSEL_ZX_ROM));
  const char* names[] = { picked };
  for (const char* nm : names) {
    if (!*nm) continue;
    File f = FSTYPE.open(nm, FILE_READ);
    if (!f) continue;
    int len = f.size();
    if (len != zx::ROM_SIZE) { f.close(); continue; }
#if BOARD_ROM_IN_FLASH
    // PicoCalc: no PSRAM, so the ROM goes to spare flash and is read through XIP (a no-op compare
    // on every boot after the first). See src/picocalc/romflash_picocalc.cpp.
    const uint8_t* fb = romFlashLoad(ROMFLASH_BIOS, f, len); f.close();
    if (fb) { zx::rom = fb; zx::romLen = len;
              sprintf(buf, "ZX: ROM %s (flash)", nm); printLog(buf); return true; }
    continue;
#else
    uint8_t* b = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!b) b = (uint8_t*)ps_malloc(len);
    if (!b) { f.close(); continue; }
    int got = f.read(b, len); f.close();
    if (got == len) { zx::rom = b; zx::romLen = len;
                      sprintf(buf, "ZX: ROM %s", nm); printLog(buf); return true; }
    free(b);
#endif
  }
  zx::romLen = 0;
  printLog("ZX: NO ROM - put 48.rom (16K) in /roms on the SD card");
  return false;
}

// SD browser (.sna/.z80/.tap/.tzx + subdirectories); see src/shared/filebrowser.h.
#define ZX_MAX_FILES FB_MAX_ENTRIES
static bool zxAccept(const std::string &n) {
  return zxEndsCI(n, ".sna") || zxEndsCI(n, ".z80") || zxEndsCI(n, ".tap") || zxEndsCI(n, ".tzx");
}
static FileBrowser zxBrowser = { "ZX", &zxFiles, zxAccept, nullptr, ZX_MAX_FILES, "" };

void loadZxFilesSync()   { fbScan(zxBrowser); }
void zxBrowseEnter(const char *path) { fbEnter(zxBrowser, path); }
void zxBrowseUp()        { fbUp(zxBrowser); }
void zxScanFiles()       { loadZxFilesSync(); }

// ============================ platform entry points =============================================
void zxSetup() {
  printLog("ZX Spectrum 48K Setup... (Z80 + ULA + beeper)");

  zx::ram        = sharedBigBuf;
  zx::screen     = sharedBigBuf + OFF_SCREEN;
  zx::borderLine = sharedBigBuf + OFF_BORDER;
  menuScreen     = sharedBigBuf + OFF_MENU;
  menuColor      = menuScreen + 0x546;
  zx::tapeBlocks = (zx::TapeBlock*)(sharedBigBuf + OFF_TAPE);
  zx::tapeMaxBlocks = TAPE_MAX_BLOCKS;
  zxLine         = (uint16_t*)(sharedBigBuf + OFF_LINE);
  zx::beepRing   = sharedBigBuf + OFF_BEEP;
#if !defined(BOARD_PICOCALC)
  zxScratch = (uint16_t*)malloc(zx::OUT_W * 8 * sizeof(uint16_t));
#endif
  memset(zx::screen, 0, zx::SCR_SIZE);
  memset(zx::borderLine, 7, zx::OUT_H);
  zxBuildPalette(videoColor);

  loadRomFromSD();
  zx::machineWire();
  zx::machineReset();

  if (zx::romLen > 0 && selectedZxFileName.length() > 1 && selectedZxFileName != "/")
    zxLoadSelected(selectedZxFileName.c_str());

  sprintf(buf, "ZX ready. internal free=%u, heap=%u",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)ESP.getFreeHeap());
  printLog(buf);
}

// 69888 T-states per frame at 3.5 MHz = 19968 us (50.08 Hz). NORMAL mode paces each frame to that
// wall-clock; FAST mode (zxFast) runs uncapped.
void zxLoop() {
  zxLoopRunning = true;
  if (zx::romLen > 0) {                                  // uncapped benchmark (~0.8 s emulated)
    uint32_t c0 = zx::cpu.cycles; uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 800) zx::runFrame();
    uint32_t dt = millis() - t0; uint32_t dc = zx::cpu.cycles - c0;
    zxMeasuredMhz = (dt > 0) ? (float)((double)dc / ((double)dt * 1000.0)) : 0.0f;
    sprintf(buf, "ZX: uncapped Z80 = %.2f MHz (real Spectrum = 3.50)", zxMeasuredMhz);
    printLog(buf);
  }

  const uint32_t FRAME_US = 19968;
  uint32_t nextUs = micros();
  SpeedMeter meter; meter.restart(zx::cpu.cycles);
  for (;;) {
    if (OptionsWindow || paused) { zxParked = true; vTaskDelay(pdMS_TO_TICKS(20)); nextUs = micros(); meter.restart(zx::cpu.cycles); continue; }
    zxParked = false;
    if (zxResetReq)   { zxResetReq = false; zx::machineReset(); nextUs = micros(); meter.restart(zx::cpu.cycles); }
    if (zx::romLen == 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }   // no ROM -> nothing to run

    zx::runFrame();
    meter.tick(zx::cpu.cycles, zxMeasuredMhz);

    if (zxFast) {
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

// ---- render (core 0) ------------------------------------------------------------------------------
// One 256-pixel paper line of the snapshot, bitmap y = 0..191, into dst.
static void zxPaperLine(int y, uint16_t* dst) {
  const uint8_t* pix = zx::screen + (((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2));
  const uint8_t* att = zx::screen + 6144 + (y >> 3) * 32;
  const bool flash = zx::flashPhase;
  for (int c = 0; c < 32; c++) {
    const uint8_t a = att[c];
    const int br = (a & 0x40) >> 3;
    uint16_t ink = zxPal[(a & 7) | br], paper = zxPal[((a >> 3) & 7) | br];
    if ((a & 0x80) && flash) { uint16_t t = ink; ink = paper; paper = t; }
    uint8_t b = pix[c];
    for (int i = 0; i < 8; i++, b <<= 1) *dst++ = (b & 0x80) ? ink : paper;
  }
}

#if UI_PICOCALC
// Paper pixels x..x+3 (x a multiple of 4) of bitmap line y, decoded straight from the snapshot:
// SCREEN: ENLARGE / STRETCH blend two lines at a time and sharedBigBuf has no room for a second line buffer.
static inline void zxPaper4(int y, int x, uint16_t* o) {
  const uint8_t a = zx::screen[6144 + (y >> 3) * 32 + (x >> 3)];
  const int br = (a & 0x40) >> 3;
  uint16_t ink = zxPal[(a & 7) | br], paper = zxPal[((a >> 3) & 7) | br];
  if ((a & 0x80) && zx::flashPhase) { uint16_t t = ink; ink = paper; paper = t; }
  uint8_t b = zx::screen[(((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2)) + (x >> 3)];
  if (x & 4) b <<= 4;
  for (int i = 0; i < 4; i++, b <<= 1) o[i] = (b & 0x80) ? ink : paper;
}
#endif

// Output row r (0..239) = border + paper, into zxLine.
static void zxOutLine(int r, uint16_t* dst) {
  const uint16_t bc = zxPal[zx::borderLine[r] & 7];
  const int py = r - zx::BORDER_Y;
  if (py < 0 || py >= 192) { for (int x = 0; x < zx::OUT_W; x++) dst[x] = bc; return; }
  for (int x = 0; x < zx::BORDER_X; x++) dst[x] = bc;
  zxPaperLine(py, dst + zx::BORDER_X);
  for (int x = zx::BORDER_X + 256; x < zx::OUT_W; x++) dst[x] = bc;
}

#if UI_PICOCALC
// Checksum of everything that draws 8-line band b: the paper lines' bitmap bytes and their one
// attribute row (bands line up with character rows: BORDER_Y is a multiple of 8), the FLASH phase
// when any of those attributes flashes, and -- 1:1 only -- the border colour of each row.
static uint32_t zxBandSum(int b, bool fill) {
  uint32_t h = dband::SEED;
  const int r0 = fill ? b * 8 + zx::BORDER_Y : b * 8;          // output row (1:1 numbering)
  if (!fill) h = dband::hash(h, zx::borderLine + r0, 8);
  const int py0 = r0 - zx::BORDER_Y;
  if (py0 < 0 || py0 >= 192) return h;
  for (int y = py0; y < py0 + 8; y++)
    h = dband::hash(h, zx::screen + (((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2)), 32);
  const uint8_t* att = zx::screen + 6144 + (py0 >> 3) * 32;
  h = dband::hash(h, att, 32);
  uint8_t any = 0;
  for (int c = 0; c < 32; c++) any |= att[c];
  if (any & 0x80) h = dband::mix(h, zx::flashPhase);
  return h;
}

// Dirty-band push: ORIG (1:1) is 30 bands of 8 rows (border included); ENLARGE is the 24 paper
// bands, 10 output rows each; STRETCH the 24 paper bands on the raw 320x320 panel, band b owning
// rows stretchRow(b*8)..stretchRow(b*8 + 8). Runs of consecutive dirty bands share one window.
static void zxPushDirty(uint8_t mode, bool repaint) {
  const bool fill = mode != SCREEN_ORIG, stretch = mode == SCREEN_STRETCH;
  const int bands = fill ? 24 : zx::OUT_H / 8, rowsPer = fill ? 10 : 8;
  dband::begin(dband::mix(dband::hash(0x5A583438u, (const uint8_t*)zxPal, sizeof(zxPal)), mode), repaint);
  bool dirty[zx::OUT_H / 8];
  for (int b = 0; b < bands; b++) dirty[b] = dband::changed(b, zxBandSum(b, fill));
  int sent = 0;
  for (int b = 0; b < bands; ) {
    if (!dirty[b]) { b++; continue; }
    int e = b;
    while (e < bands && dirty[e]) e++;
    sent += e - b;
    if (stretch) {
      const int r0 = dband::stretchRow(b * 8, 192, PANEL_NATIVE_H), r1 = dband::stretchRow(e * 8, 192, PANEL_NATIVE_H);
      tft.setPanelAddrWindow(0, r0, zx::OUT_W, r1 - r0);
    } else {
      tft.setAddrWindow(0, b * rowsPer, zx::OUT_W, (e - b) * rowsPer);
    }
    tft.startWrite();
    if (stretch) {
      // 5:4 across (smooth5), 5:3 down: each row blends its two nearest paper lines in quarters,
      // clamped to its own band (as dband::writeStretchLines256).
      for (int k = b; k < e; k++) {
        const int y0 = k * 8, r1 = dband::stretchRow(y0 + 8, 192, PANEL_NATIVE_H);
        for (int oy = dband::stretchRow(y0, 192, PANEL_NATIVE_H); oy < r1; oy++) {
          int s4 = ((2 * oy + 1) * 192 * 2 - 2 * PANEL_NATIVE_H) / PANEL_NATIVE_H;
          if (s4 < 0) s4 = 0;
          int sa = s4 >> 2, q = s4 & 3;
          if (sa < y0)      { sa = y0; q = 0; }
          if (sa >= y0 + 7) { sa = y0 + 7; q = 0; }
          uint16_t c[4], d[4], o[5];
          for (int x = 0; x < 256; x += 4) {
            zxPaper4(sa, x, c);
            if (q) { zxPaper4(sa + 1, x, d); for (int i = 0; i < 4; i++) c[i] = dband::blendQ(c[i], d[i], q); }
            dband::smooth5(c[0], c[1], c[2], c[3], o);
            for (int i = 0; i < 5; i++) tft.writeColor(o[i], 1);
          }
        }
      }
    } else if (fill) {
      // Bilinear 5:4 (dband::smooth5): row k of each 5-row group blends two of its 4 paper lines.
      for (int oy = b * 10; oy < e * 10; oy++) {
        const int base = oy / 5 * 4, k = oy % 5, w = dband::kRowW[k];
        const int m = base + dband::kRowMain[k], t = base + dband::kRowOther[k];
        uint16_t c[4], o[5], q[5];
        for (int x = 0; x < 256; x += 4) {
          zxPaper4(m, x, c);
          dband::smooth5(c[0], c[1], c[2], c[3], o);
          if (w) {
            zxPaper4(t, x, c);
            dband::smooth5(c[0], c[1], c[2], c[3], q);
            for (int i = 0; i < 5; i++) o[i] = (w == 2) ? dband::avg565(o[i], q[i]) : dband::q3_565(o[i], q[i]);
          }
          for (int i = 0; i < 5; i++) tft.writeColor(o[i], 1);
        }
      }
    } else {
      for (int r = b * 8; r < e * 8; r++) {
        const uint16_t bc = zxPal[zx::borderLine[r] & 7];
        const int py = r - zx::BORDER_Y;
        if (py < 0 || py >= 192) { tft.writeColor(bc, zx::OUT_W); continue; }
        zxPaperLine(py, zxLine);
        tft.writeColor(bc, zx::BORDER_X);
        for (int x = 0; x < 256; x++) tft.writeColor(zxLine[x], 1);
        tft.writeColor(bc, zx::OUT_W - zx::BORDER_X - 256);
      }
    }
    tft.endWrite();
    b = e;
  }
  dband::frameDone("ZX", sent);
}
#endif

void zxRenderFrame() {
  if (!zx::frameReady) return;
  if (!zxLine) { zx::frameReady = false; return; }
  if (videoColor != zxPalColor) zxBuildPalette(videoColor);
  // The 320x240 picture (paper + border) fills the whole screen. An open on-screen keyboard squeezes
  // it into the rows above the keyboard (the P4's keyboard is a transparent overlay: never squeezed).
  const bool osk = oskActive();
#if BOARD_PANEL_DSI
  const int outH = zx::OUT_H;
#else
  const int outH = osk ? oskRasterHeight() : zx::OUT_H;
#endif
  const int outTop = 0;
#if UI_PICOCALC
  // SCREEN: ENLARGE drops the border and scales the 256x192 paper 5:4 bilinear to the whole 320x240
  // screen; STRETCH 5:4 x 5:3 to the whole raw 320x320 panel. Leaving STRETCH blackens its bars.
  extern uint8_t screenFill;
  static uint8_t lastFill = SCREEN_ORIG;
  const uint8_t fill = osk ? (uint8_t)SCREEN_ORIG : screenFillNow();
  if (lastFill == SCREEN_STRETCH && fill != SCREEN_STRETCH) tft.fillPanelBlack();
  lastFill = fill;
  const bool repaint = clearScr;                     // a menu closed: anything may be on the panel
  clearScr = false;
  if (!osk) { zxPushDirty(fill, repaint); zx::frameReady = false; return; }
  tft.setAddrWindow(0, outTop, zx::OUT_W, outH);
  tft.startWrite();
  for (int oy = 0; oy < outH; oy++) {
    int sr = oy * zx::OUT_H / outH;
    if (sr > zx::OUT_H - 1) sr = zx::OUT_H - 1;
    const uint16_t bc = zxPal[zx::borderLine[sr] & 7];
    const int py = sr - zx::BORDER_Y;
    if (py < 0 || py >= 192) { tft.writeColor(bc, zx::OUT_W); continue; }
    zxPaperLine(py, zxLine);
    tft.writeColor(bc, zx::BORDER_X);
    for (int x = 0; x < 256; x++) tft.writeColor(zxLine[x], 1);
    tft.writeColor(bc, zx::OUT_W - zx::BORDER_X - 256);
  }
  tft.endWrite();
#else
  if (!zxScratch) { zx::frameReady = false; return; }
  tft.setSwapBytes(true);
  for (int oy = 0; oy < outH; ) {
    int n = 0;
    while (oy + n < outH && n < 8) {
      int sr = (oy + n) * zx::OUT_H / outH;
      if (sr > zx::OUT_H - 1) sr = zx::OUT_H - 1;
      zxOutLine(sr, zxScratch + n * zx::OUT_W);
      n++;
    }
    tft.pushImage(0, outTop + oy, zx::OUT_W, n, zxScratch);
    oy += n;
  }
  tft.setSwapBytes(false);
#endif
  fpsFrameShown();
  zx::frameReady = false;
}

// ---- input (from joystick.cpp / usbkeyboard.cpp / touchkeyboard.cpp) ----
void zxSetKempston(uint8_t v)          { zx::setKempston(v); }
void zxKey(int row, int bit, bool down) { zx::keySet(row, bit, down); }
void zxKeysReleaseAll()                { zx::keyClearAll(); }
void zxHardReset()                     { zxResetReq = true; }   // F12: back to the (C) 1982 screen
void zxCancelReset()                   { zxResetReq = false; }  // snapshot loaded: resume it, do not reset

// ---- file loading ---------------------------------------------------------------------------------
// Snapshots write RAM and the CPU directly, so the Z80 must not be running: at boot the loop has not
// started, and from the settings window it parks within a frame. Wait for that before touching state.
static bool zxWaitParked() {
  if (!zxLoopRunning) return true;
  for (int i = 0; i < 50 && !zxParked; i++) vTaskDelay(pdMS_TO_TICKS(5));
  return zxParked;
}

// Small buffered reader over an SD File (the .Z80 decoder reads byte by byte).
struct ZxReader {
  File* f; uint8_t b[256]; int n = 0, i = 0;
  int get() {
    if (i >= n) { n = f->read(b, sizeof(b)); i = 0; if (n <= 0) { n = 0; return -1; } }
    return b[i++];
  }
};

static inline void zxPoke(uint16_t a, uint8_t v) { if (a >= 0x4000) zx::ram[a - 0x4000] = v; }

// .SNA (48K): 27-byte register header, then 0x4000-0xFFFF. PC is on the stack (RETN semantics).
static bool zxLoadSna(File& f) {
  if (f.size() != 49179) { printLog("ZX: .SNA is not a 48K snapshot"); return false; }
  uint8_t h[27];
  if (f.read(h, 27) != 27) return false;
  zx::machineReset();
  if (f.read(zx::ram, zx::RAM_SIZE) != zx::RAM_SIZE) return false;
  Z80& c = zx::cpu;
  c.I = h[0];
  c.L_ = h[1]; c.H_ = h[2]; c.E_ = h[3]; c.D_ = h[4]; c.C_ = h[5]; c.B_ = h[6]; c.F_ = h[7]; c.A_ = h[8];
  c.L = h[9]; c.H = h[10]; c.E = h[11]; c.D = h[12]; c.C = h[13]; c.B = h[14];
  c.IYL = h[15]; c.IYH = h[16]; c.IXL = h[17]; c.IXH = h[18];
  c.IFF1 = c.IFF2 = (h[19] & 0x04) != 0;
  c.R = h[20] & 0x7F; c.R7 = h[20] & 0x80;
  c.F = h[21]; c.A = h[22];
  c.SP = (uint16_t)(h[23] | (h[24] << 8));
  c.IM = h[25] & 3;
  zx::setBorder(h[26]);
  c.PC = (uint16_t)(zx::memRead8(c.SP) | (zx::memRead8((uint16_t)(c.SP + 1)) << 8));
  c.SP += 2;
  return true;
}

// .Z80 RLE: "ED ED nn bb" = nn copies of bb. v1 data ends with 00 ED ED 00.
static void zxUnpack(ZxReader& r, uint16_t addr, uint32_t outLen, int32_t inLen) {
  uint32_t o = 0;
  while (o < outLen && inLen != 0) {
    int b = r.get(); if (b < 0) return; if (inLen > 0) inLen--;
    if (b == 0xED) {
      int b2 = r.get(); if (b2 < 0) return; if (inLen > 0) inLen--;
      if (b2 == 0xED) {
        int cnt = r.get(), v = r.get(); if (v < 0) return; if (inLen > 0) inLen -= 2;
        for (int k = 0; k < cnt && o < outLen; k++) zxPoke((uint16_t)(addr + o++), (uint8_t)v);
      } else {
        zxPoke((uint16_t)(addr + o++), 0xED);
        if (o < outLen) zxPoke((uint16_t)(addr + o++), (uint8_t)b2);
      }
    } else {
      zxPoke((uint16_t)(addr + o++), (uint8_t)b);
    }
  }
}

static bool zxLoadZ80(File& f) {
  uint8_t h[30];
  if (f.read(h, 30) != 30) return false;
  uint16_t pc = (uint16_t)(h[6] | (h[7] << 8));
  uint8_t flags1 = (h[12] == 0xFF) ? 1 : h[12];
  uint8_t ext[64]; int extLen = 0;
  if (pc == 0) {                                          // v2 / v3: extended header
    uint8_t l[2];
    if (f.read(l, 2) != 2) return false;
    extLen = l[0] | (l[1] << 8);
    if (extLen < 23 || extLen > (int)sizeof(ext)) { printLog("ZX: unsupported .Z80 header"); return false; }
    if (f.read(ext, extLen) != extLen) return false;
    pc = (uint16_t)(ext[0] | (ext[1] << 8));
    const uint8_t hw = ext[2];
    const bool is48 = (extLen == 23) ? (hw == 0 || hw == 1) : (hw == 0 || hw == 1 || hw == 3);
    if (!is48) { printLog("ZX: .Z80 is not a 48K snapshot"); return false; }
  }
  zx::machineReset();
  ZxReader r; r.f = &f;
  if (extLen == 0) {
    if (flags1 & 0x20) zxUnpack(r, 0x4000, 0xC000, -1);
    else if (f.read(zx::ram, zx::RAM_SIZE) != zx::RAM_SIZE) return false;
  } else {
    for (;;) {                                            // 16K pages: 8 -> 0x4000, 4 -> 0x8000, 5 -> 0xC000
      int l0 = r.get(), l1 = r.get(), pg = r.get();
      if (pg < 0) break;
      const uint32_t len = (uint32_t)(l0 | (l1 << 8));
      const uint16_t base = pg == 8 ? 0x4000 : pg == 4 ? 0x8000 : pg == 5 ? 0xC000 : 0;
      if (len == 0xFFFF) {
        for (uint32_t k = 0; k < 0x4000; k++) { int v = r.get(); if (v < 0) break; if (base) zxPoke((uint16_t)(base + k), (uint8_t)v); }
      } else if (base) {
        zxUnpack(r, base, 0x4000, (int32_t)len);
      } else {
        for (uint32_t k = 0; k < len; k++) if (r.get() < 0) break;   // ROM / other pages: skip
      }
    }
  }
  Z80& c = zx::cpu;
  c.A = h[0]; c.F = h[1];
  c.C = h[2]; c.B = h[3]; c.L = h[4]; c.H = h[5];
  c.PC = pc;
  c.SP = (uint16_t)(h[8] | (h[9] << 8));
  c.I = h[10];
  c.R = h[11] & 0x7F; c.R7 = (uint8_t)((flags1 & 1) << 7);
  zx::setBorder((flags1 >> 1) & 7);
  c.E = h[13]; c.D = h[14];
  c.C_ = h[15]; c.B_ = h[16]; c.E_ = h[17]; c.D_ = h[18]; c.L_ = h[19]; c.H_ = h[20];
  c.A_ = h[21]; c.F_ = h[22];
  c.IYL = h[23]; c.IYH = h[24]; c.IXL = h[25]; c.IXH = h[26];
  c.IFF1 = h[27] != 0; c.IFF2 = h[28] != 0;
  c.IM = h[29] & 3;
  return true;
}

// LOAD "" typed after a tape is inserted: J (LOAD in K mode), SYMBOL SHIFT+P twice, ENTER.
#define ZXK(row, bit) ((uint16_t)(((row) << 3 | (bit)) + 1))
static const uint16_t ZX_LOAD_CHORDS[] = {
  ZXK(6, 3),                                  // J -> LOAD
  (uint16_t)(ZXK(7, 1) | (ZXK(5, 0) << 8)),   // SYMBOL SHIFT + P -> "
  (uint16_t)(ZXK(7, 1) | (ZXK(5, 0) << 8)),
  ZXK(6, 0),                                  // ENTER
};

static bool zxLoadTape(File& f, bool tzx) {
  const int len = f.size();
  if (len <= 0) return false;
#if BOARD_ROM_IN_FLASH
  if ((uint32_t)len > romFlashCapacity(ROMFLASH_CART)) { printLog("ZX: tape does not fit in flash"); return false; }
  const uint8_t* d = romFlashLoad(ROMFLASH_CART, f, len);
  if (!d) { printLog("ZX: tape flash write failed"); return false; }
#else
  if (g_tapeBuf) { free(g_tapeBuf); g_tapeBuf = nullptr; zx::tapeData = nullptr; zx::tapeCount = 0; }
  uint8_t* d = (uint8_t*)ps_malloc(len);
  if (!d) { printLog("ZX: tape malloc failed"); return false; }
  if (f.read(d, len) != len) { free(d); printLog("ZX: tape read short"); return false; }
  g_tapeBuf = d;
#endif
  zx::tapeData = d;
  const int n = tzx ? zx::tapeIndexTzx(d, len) : zx::tapeIndexTap(d, len);
  if (n == 0) { printLog("ZX: no loadable blocks on the tape"); return false; }
  zx::machineReset();
  zx::autoTypeStart(ZX_LOAD_CHORDS, (int)(sizeof(ZX_LOAD_CHORDS) / sizeof(ZX_LOAD_CHORDS[0])), 10);
  sprintf(buf, "ZX: tape with %d block%s, typing LOAD \"\"", n, n == 1 ? "" : "s");
  printLog(buf);
  return true;
}

bool zxLoadSelected(const char* path) {
  std::string p = path ? path : "";
  const bool sna = zxEndsCI(p, ".sna"), z80 = zxEndsCI(p, ".z80");
  const bool tap = zxEndsCI(p, ".tap"), tzx = zxEndsCI(p, ".tzx");
  if (!sna && !z80 && !tap && !tzx) { printLog("ZX: unsupported file (use .sna/.z80/.tap/.tzx)"); return false; }
  if (zx::romLen == 0) { printLog("ZX: no ROM"); return false; }
  if (!zxWaitParked()) { printLog("ZX: machine busy, try again"); return false; }
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "ZX: cannot open %s", path); printLog(buf); return false; }
  if (sna || z80) zx::tapeCount = 0;                      // a snapshot ejects the tape
  bool ok = sna ? zxLoadSna(f) : z80 ? zxLoadZ80(f) : zxLoadTape(f, tzx);
  f.close();
  if (!ok) {
    sprintf(buf, "ZX: failed to load %s", path); printLog(buf);
    zx::machineReset();                                   // never leave a half-written snapshot running
    zxSnapIn = false;
    return false;
  }
  zx::frameReady = false;
  zxSnapIn = sna || z80;
  selectedZxFileName = path;
  sprintf(buf, "ZX: %s loaded", path);
  printLog(buf);
  return true;
}

// ---- standard media unmount (proto.h) ----
// A tape is in while it has blocks indexed (a snapshot load ejects it); a snapshot while it is the
// last thing loaded. Either way selectedZxFileName names it.
bool zxMediaMounted(const char* path) {
  if (!path || !*path || selectedZxFileName != path) return false;
  return zx::tapeCount > 0 || zxSnapIn;
}

// Tape: eject live -- the program keeps running (LD-BYTES just finds no more blocks). Snapshot: the
// program is the RAM, so pull it by resetting to the (C) 1982 screen when the CPU resumes.
void zxUnmount(const char* path) {
  if (!zxMediaMounted(path)) return;
  if (zx::tapeCount > 0) {
    zx::tapeCount = 0; zx::tapePos = 0;                   // the LD-BYTES trap stops seeing the tape
#if !BOARD_ROM_IN_FLASH
    if (zxWaitParked() && g_tapeBuf) {                    // tapeTrap runs on the CPU task: free only when parked
      zx::tapeData = nullptr; free(g_tapeBuf); g_tapeBuf = nullptr;
    }                                                     // else the next tape load frees it
#endif
    sprintf(buf, "ZX: tape %s ejected", path);
  } else {
    zxSnapIn = false;
    zxResetReq = true;                                    // zxLoop resets once the settings window closes
    sprintf(buf, "ZX: snapshot %s unloaded, resetting", path);
  }
  printLog(buf);
  selectedZxFileName = "";
}

// ---- startup overlay: no ROM ----
// Without the ROM nothing can run. Yields to SETTINGS and redraws once each time it closes.
bool zxRenderLoadWarning() {
  if (zx::romLen != 0) return false;
  static bool drawn = false;
  if (OptionsWindow) { drawn = false; return false; }
  if (!drawn) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK);
    tft.drawString("ZX SPECTRUM: NO ROM FOUND", 8, 8, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Put the 16K Spectrum 48K ROM on the SD card", 8, 40, 1);
    tft.drawString("as /roms/48.rom and restart,", 8, 56, 1);
    tft.drawString("or pick one in SETTINGS (Ctrl-F1) > ROMS.", 8, 72, 1);
    tft.setTextDatum(MC_DATUM);
    drawn = true;
  }
  return true;
}

#endif  // BOARD_HAS_ZX_CORE
