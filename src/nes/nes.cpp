#include "../../emu.h"
#include "nes.h"

// NES core C-linkage glue, called by the platform dispatch:
//   setup() -> nesSetup(),  loop() -> nesLoop(),  renderLoop() -> nesRenderFrame(),
//   joystick task -> nesSetController().

// Per-frame conversion scratch: one 256-wide band of up to 8 lines (RGB565). MALLOC'd on the
// NES path only (like the C64's c64Scratch) so it never burdens the Apple/C64 static budget.
static uint16_t *nesScratch = nullptr;

// The NES picture is 256x240; the panel is 320x240, so pillarbox by 32px each side.
static const int NES_W = 256, NES_H = 240, NES_OX = (320 - 256) / 2;

void nesSetup() {
  printLog("NES Setup... (mappers 0-4 + PRG streaming)");
  // The shared text interface (clearScreen/print) used by the settings window writes these
  // two buffers; the NES path skips Apple's memoryAlloc(), so allocate them here to avoid a
  // null-pointer store if the user opens the options menu (same fix the C64 path needed).
  menuScreen = (unsigned char *)malloc(0x546);
  menuColor  = (unsigned char *)malloc(0x546);

  // Heap-allocate the startup-warning buffer before the loader writes it (not static BSS — the
  // static DRAM budget is full now four cores are resident; same reason cpuRam/vram are malloc'd).
  nes::loadWarn = (char *)malloc(256);
  if (nes::loadWarn) nes::loadWarn[0] = 0;

  // 256x240 8-bit indexed framebuffer lives in the shared static buffer (mutually exclusive
  // with Apple RAM / the C64 framebuffer); 256*240 = 61440 <= sizeof(sharedBigBuf).
  nes::framebuffer = sharedBigBuf;
  memset(sharedBigBuf, 0, NES_W * NES_H);

  // 2K CPU RAM + 2K nametable VRAM are malloc'd here (not static BSS — see nes.h).
  nes::cpuRam = (uint8_t *)nesAllocFast(0x800);   // internal DRAM: zero-page/stack, hottest RAM
  nes::vram   = (uint8_t *)nesAllocFast(0x800);   // internal DRAM: nametable fetch every tile
  if (nes::cpuRam) memset(nes::cpuRam, 0, 0x800);
  if (nes::vram)   memset(nes::vram, 0, 0x800);

  nesScratch = (uint16_t *)malloc(NES_W * 8 * sizeof(uint16_t));

  nes::ppuReset();
  nes::nesBuildGrayPalette();   // for the VIDEO=MONO option
  nes::nesLoadFirstRom();   // scan SD root + load the first .nes (PRG/CHR malloc here)

  sprintf(buf, "NES ready. free heap=%u", (unsigned)ESP.getFreeHeap());
  printLog(buf);
}

void nesLoop() {
  nes::cpuLoop();      // runs forever (6502 + PPU scanline timing)
}

// FNV-1a over a band of the indexed framebuffer, read 32 bits at a time (rows are 256 bytes, so
// every row is word-aligned). Used to tell whether a band still matches what is on the panel; a
// collision would hold one stale 8-line band for one frame, at odds of 2^-32.
static inline uint32_t bandDigest(const uint8_t *p, int words) {
  const uint32_t *w = (const uint32_t *)(const void *)p;
  uint32_t h = 0x811C9DC5u;
  for (int i = 0; i < words; i++) h = (h ^ w[i]) * 0x01000193u;
  return h;
}

// Convert the indexed framebuffer to RGB565 and push it to the TFT, centred with pillarbox
// borders. Runs on the core-0 render task (which owns the TFT), like c64RenderFrame.
static void nesPushFrame(const uint16_t *pal) {
#if BOARD_DISPLAY_GFX
  // S3 fast path (default, unless fill-screen scaling is on): convert each 8-line band and push it
  // STRAIGHT to the panel, bypassing the PSRAM canvas write and the full 480x272 QSPI flush. This is
  // the lever that recovers the fps lost to core-0 PSRAM/MSPI contention with the core-1 interpreter.
  extern bool screenFill;
  if (!screenFill) {
    static bool bordersDrawn = false;
    if (!bordersDrawn) { tft.fillPanelBlack(); bordersDrawn = true; }   // static black frame, once
    tft.setBypassCanvas(true);                  // make the top-of-loop displayFlush() a no-op
    for (int y = 0; y < NES_H; ) {
      int n = 0;
      while (y + n < NES_H && n < 8) {
        const uint8_t *src = nes::framebuffer + (y + n) * NES_W;
        uint16_t *dst = nesScratch + n * NES_W;
        for (int xx = 0; xx < NES_W; xx++) dst[xx] = pal[src[xx] & 0x3F];
        n++;
      }
      nes::fbReadLine = y + n;                             // these lines are ours now; PPU may reuse them
      tft.pushPanelBand(NES_OX, y, NES_W, n, nesScratch);   // direct to panel, centered
      y += n;
    }
    return;
  }
#endif

  // Original path (CYD via TFT_eSPI, PicoCalc, or S3 with fill-screen scaling on): draw through the
  // abstraction (canvas on S3, panel otherwise) and let the per-frame flush present it. On S3
  // fill-screen, ask the flush to STRETCH the 256x240 picture across the whole panel (no 4:3 aspect,
  // no side bars) so the NES truly fills the screen; the no-op elsewhere leaves 320x240 centred.
  //
  // Everything below is aimed at the display link, because on the PicoCalc that is what caps the
  // visible frame rate: 256x240 of RGB565 is 123KB, and at 50MHz SPI the transfer alone is ~20ms.
  // Two things were being sent that did not need to be:
  //   * the pillarbox borders are static black, yet were repainted every frame -- 15360 more
  //     pixels, a fifth of the whole transfer, for a picture that never changes;
  //   * whole 8-line bands are frequently identical frame to frame (status bars, HUDs, letterboxed
  //     menus, anything not scrolling). Digesting a band costs ~15us; sending it costs ~660us.
  // A full repaint is forced when the panel may have been painted over from outside (the options
  // window or the startup notice -- seen as a gap since the last push), when the palette changes
  // (VIDEO=COLOR/MONO), and every 64th push as a cheap backstop against a wrongly-held band.
  static uint32_t bandDig[NES_H / 8];
  static const uint16_t *lastPal = nullptr;
  static uint32_t lastPushMs = 0, pushCount = 0;
  uint32_t nowMs = millis();
  bool forceAll = (pal != lastPal) || (nowMs - lastPushMs > 100) || ((pushCount++ & 63) == 0);
  lastPal = pal; lastPushMs = nowMs;

  displaySetVideoFill(NES_OX, NES_W, true);
  if (forceAll) {
    tft.fillRect(0, 0, NES_OX, NES_H, TFT_BLACK);                 // left border
    tft.fillRect(NES_OX + NES_W, 0, 320 - (NES_OX + NES_W), NES_H, TFT_BLACK); // right border
  }
  tft.setSwapBytes(true);
  for (int y = 0; y < NES_H; y += 8) {
    int n = (NES_H - y < 8) ? (NES_H - y) : 8;
    const uint8_t *src = nes::framebuffer + (size_t)y * NES_W;
    uint32_t dig = bandDigest(src, (n * NES_W) / 4);
    bool same = (!forceAll && dig == bandDig[y >> 3]);
    if (!same) {
      bandDig[y >> 3] = dig;
      for (int i = 0; i < n * NES_W; i++) nesScratch[i] = pal[src[i] & 0x3F];
    }
    // Everything up to here has been copied out of the framebuffer, so the PPU is free to draw
    // over it (see the overlap note in nes.h). Publish before the SPI wait, not after: the pixels
    // are already in nesScratch by then, and the wait is nearly all of the band's cost.
    nes::fbReadLine = y + n;
    if (same) continue;                                           // already on the panel
    tft.pushImage(NES_OX, y, NES_W, n, nesScratch);
  }
  tft.setSwapBytes(false);
}

void nesRenderFrame() {
  if (!nesScratch || !nes::framebuffer) return;
  const uint16_t *pal = videoColor ? nes::nesPalette : nes::nesPaletteGray;  // VIDEO color/mono

  // Collect the picture the PPU was granted last time round (see the handover note in nes.h). It
  // publishes at the end of scanline 239, so by the time we get here it is either already waiting
  // or a few milliseconds away -- this is a single wait, not a handshake: the old "wait for the PPU
  // to start a frame, THEN ask it to stop" ordering spent an extra half frame per push doing
  // nothing, which is most of the display frame rate that was missing.
  //
  // The timeout only bites if the interpreter is wedged, and then the picture is frozen anyway, so
  // re-pushing the last one is the right thing to do.
  static uint32_t lastPushed = 0;
  uint32_t t0 = millis();
  while (nes::fbFrames == lastPushed && (millis() - t0) < 120) vTaskDelay(1);
  lastPushed = nes::fbFrames;

  // Open the push, THEN grant. The order matters: the PPU tests fbPushing/fbReadLine to decide
  // whether it may start, so it must never see the grant while those still describe the last push.
  nes::fbReadLine = 0;
  nes::fbPushing  = true;
  nes::fbGrant    = true;    // the PPU releases itself once we are FB_RELEASE_LINE down the picture

  nesPushFrame(pal);

  nes::fbPushing  = false;
  nes::fbReadLine = NES_H;   // idle: never hold the PPU up
  nes::nesPushCount++;
}

// Controller bits from the joystick task -> NES controller 1 shift register.
void nesSetController(uint8_t buttons) { nes::setController(buttons); }

// Settings: load a .nes ROM picked in the options window (the CPU is paused there, so freeing the
// old cart + mallocing the new one is safe) and request a reset so the new game starts on resume.
bool nesLoadSelected(const char *path) {
  bool ok = nes::nesLoadROM(path);
  if (ok) nes::nesResetReq = true;     // serviced by cpuLoop after the paused spin
  return ok;
}
// Settings: (re)scan the SD root for *.nes so freshly-added ROMs show in the browser.
void nesScanFiles() { nes::loadNesFilesSync(); }
// Settings: subdirectory navigation for that browser. The workers live inside namespace nes,
// so these forwarders are what the shared options UI (which knows nothing about the core) calls.
void nesBrowseEnter(const char *path) { nes::browseEnter(path); }
void nesBrowseUp()                    { nes::browseUp(); }

// ---- startup ROM-skip warning overlay (runs on the core-0 render task, which owns the TFT) ----
// Lists ROMs skipped at load time (unsupported mapper / too big for RAM) so the user understands
// why their game didn't boot. Drawn once, auto-dismisses after a few seconds.
static void nesDrawWarning() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("NES: SOME ROMS SKIPPED", 8, 8, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  int y = 38;
  const char *p = nes::loadWarn;
  char line[64];
  while (*p && y < 172) {
    int n = 0;
    while (*p && *p != '\n' && n < 62) line[n++] = *p++;
    line[n] = 0;
    if (*p == '\n') p++;
    if (n) tft.drawString(line, 8, y, 1);
    y += 14;
  }
  // What DOES fit, so the user knows what to try instead.
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("Fits: NROM / CNROM / UxROM, + MMC1 with CHR-RAM.", 8, 182, 1);
  tft.drawString("e.g. SMB, Mega Man, Castlevania, Metroid, Zelda 1", 8, 196, 1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Big-CHR games (SMB3, Zelda 2) don't fit - no PSRAM.", 8, 216, 1);
  tft.setTextDatum(MC_DATUM);          // restore the datum the rest of the UI expects
}

bool nesRenderLoadWarning() {
  if (!nes::loadWarn || !nes::loadWarn[0]) return false;
  static uint32_t until = 0;
  static bool started = false, drawn = false;
  if (!started) { until = millis() + 6000; started = true; }   // ~6s after the game first renders
  if ((int32_t)(millis() - until) >= 0) { nes::loadWarn[0] = 0; return false; }
  if (!drawn) { nesDrawWarning(); drawn = true; }              // draw once, then hold
  return true;
}
