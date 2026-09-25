#include "../../emu.h"
#include "atari.h"

// Atari 2600 core C-linkage glue, called by the platform dispatch:
//   setup() -> atariSetup(),  loop() -> atariLoop(),  renderLoop() -> atariRenderFrame(),
//   joystick task -> atariSetInput().

// Per-frame conversion scratch: one 320-wide band of up to 8 output lines (RGB565). MALLOC'd on
// the Atari path only (like the NES path's nesScratch) so it never burdens the static budget.
static uint16_t *atariScratch = nullptr;

// The TIA picture is 160 wide x (usually) 192 lines; we double it horizontally to 320 (fills the
// panel width) and centre it vertically: 24px borders for 192 lines, less for taller fields.
static const int A_W = 160, A_H = ATARI_FB_H, A_MINH = 192;
// Extra left/right nudge of the displayed picture (TIA pixels). 0 normally — the HMOVE comb is now
// emulated in the TIA, which is the real cause of the left-edge alignment.
static const int A_XSHIFT = 0;

void atariSetup() {
  printLog("Atari 2600 Setup... (6507 + TIA + RIOT, carts 2K-32K)");
  // The shared text interface (clearScreen/print) used by the settings window writes these two
  // buffers; the Atari path skips Apple's memoryAlloc(), so allocate them here to avoid a null
  // store if the user opens the options menu (same fix the C64/NES paths needed).
  menuScreen = (unsigned char *)malloc(0x546);
  menuColor  = (unsigned char *)malloc(0x546);

  // Heap-allocate the startup-warning buffer before the loader writes it (not static BSS — the
  // static DRAM budget is full with four cores resident; see atari_cpu.cpp).
  atari::loadWarn = (char *)malloc(256);
  if (atari::loadWarn) atari::loadWarn[0] = 0;

  // 160x240 8-bit indexed framebuffer lives in the shared static buffer (mutually exclusive with
  // Apple RAM / C64 / NES); 160*240 = 38400 <= sizeof(sharedBigBuf). Set before tiaReset (it clears it).
  atari::framebuffer = sharedBigBuf;
  memset(sharedBigBuf, 0, A_W * A_H);

  atariScratch = (uint16_t *)malloc(320 * 8 * sizeof(uint16_t));
  // Snapshot of the last completed field for the renderer (tear-free). Optional: if the heap
  // cannot spare 30KB, rendering falls back to reading the live framebuffer.
  atari::frontBuf = (uint8_t *)malloc(A_W * A_H);
  if (atari::frontBuf) memset(atari::frontBuf, 0, A_W * A_H);
  else printLog("Atari: no heap for frame snapshot; rendering live buffer");

  atari::tiaReset();
  atari::riotReset();
  atari::atariBuildGrayPalette();    // for the VIDEO=MONO option
  atari::atariLoadFirstRom();        // scan SD root + load the first ROM (malloc here)

  sprintf(buf, "Atari ready. free heap=%u", (unsigned)ESP.getFreeHeap());
  printLog(buf);
}

void atariLoop() {
  atari::cpuLoop();      // runs forever (6507 + TIA beam timing)
}

// Convert the indexed framebuffer to RGB565 (2x horizontal) and push it to the TFT, centred with
// top/bottom borders. Runs on the core-0 render task (which owns the TFT), like nesRenderFrame.
void atariRenderFrame() {
  if (!atariScratch || !atari::framebuffer) return;
  // Tear-free path: draw only a completed field. No new one yet -> skip (the TIA may be filling
  // the snapshot right now); the next field lands within ~17ms.
  const uint8_t *fb = atari::framebuffer;
  if (atari::frontBuf) {
    if (atari::frontState != 1) return;
    __sync_synchronize();
    fb = atari::frontBuf;
  }
  int rows = atari::frontRows;
  // Height to show: at least 192 so standard games keep their usual 24px borders and position.
  int h = rows < A_MINH ? A_MINH : (rows > A_H ? A_H : rows);
  int oy = (240 - h) / 2;
  const uint16_t *pal = videoColor ? atari::atariPalette : atari::atariPaletteGray;
#if defined(BOARD_PICOCALC)
  // SCREEN: FILL (Settings): stretch the field vertically to all 240 lines (nearest neighbour), so
  // the picture is 4:3 like on a TV instead of a 320x192 strip with 24px bars. ~25% more SPI time.
  extern bool screenFill;
  if (screenFill) {
    tft.setSwapBytes(true);
    for (int y = 0; y < 240; ) {
      int n = 0;
      while (y + n < 240 && n < 8) {
        const uint8_t *src = fb + ((y + n) * h / 240) * A_W;
        uint16_t *dst = atariScratch + n * 320;
        for (int x = 0; x < A_W; x++) {
          int sc = x + A_XSHIFT;
          uint16_t c = ((unsigned)sc < A_W) ? pal[src[sc] & 0x7F] : 0;
          dst[x * 2] = c; dst[x * 2 + 1] = c;
        }
        n++;
      }
      tft.pushImage(0, y, 320, n, atariScratch);
      y += n;
    }
    tft.setSwapBytes(false);
    if (atari::frontBuf) { __sync_synchronize(); atari::frontState = 0; }
    return;
  }
#endif
  if (oy > 0) tft.fillRect(0, 0, 320, oy, TFT_BLACK);                    // top border
  if (oy + h < 240) tft.fillRect(0, oy + h, 320, 240 - (oy + h), TFT_BLACK); // bottom border
  tft.setSwapBytes(true);
  for (int y = 0; y < h; ) {
    int n = 0;
    while (y + n < h && n < 8) {
      const uint8_t *src = fb + (y + n) * A_W;
      uint16_t *dst = atariScratch + n * 320;
      for (int x = 0; x < A_W; x++) {
        int sc = x + A_XSHIFT;                                     // shift content left to re-centre
        uint16_t c = ((unsigned)sc < A_W) ? pal[src[sc] & 0x7F] : 0;
        dst[x * 2] = c; dst[x * 2 + 1] = c;                        // pixel-double to 320 wide
      }
      n++;
    }
    tft.pushImage(0, oy + y, 320, n, atariScratch);
    y += n;
  }
  tft.setSwapBytes(false);
  if (atari::frontBuf) { __sync_synchronize(); atari::frontState = 0; }   // snapshot free for the next field
}

// Joystick task -> TIA/RIOT input ports. dirBits: bit0=Up,1=Down,2=Left,3=Right (active-high here,
// inverted to the active-low hardware lines below). Pb0=Fire, Pb1=Select, Pb2=Reset (see joystick.cpp).
void atariSetInput(uint8_t dirBits, bool fire, bool select, bool reset) {
  uint8_t a = 0xFF;                            // SWCHA, active-low; P0 = high nibble
  if (dirBits & 0x01) a &= ~0x10;              // up
  if (dirBits & 0x02) a &= ~0x20;              // down
  if (dirBits & 0x04) a &= ~0x40;              // left
  if (dirBits & 0x08) a &= ~0x80;              // right
  atari::swcha = a;

  uint8_t b = 0x3F;                            // SWCHB: colour on, difficulty B, reset/select released
  if (select) b &= ~0x02;                      // SELECT pressed
  if (reset)  b &= ~0x01;                      // RESET pressed
  atari::swchb = b;

  atari::inpt4 = fire ? 0x00 : 0x80;           // P0 fire on bit7 (0 = pressed)
}

// Settings: load a ROM picked in the options window (CPU paused there) + request a reset on resume.
bool atariLoadSelected(const char *path) {
  bool ok = atari::atariLoadROM(path);
  if (ok) atari::atariResetReq = true;         // serviced by cpuLoop after the paused spin
  return ok;
}
// Settings: (re)scan the SD root for *.a26 / *.bin so freshly-added ROMs show in the browser.
void atariScanFiles() { atari::loadAtariFilesSync(); }
// Settings: subdirectory navigation for that browser. Same forwarding as nesBrowse* -- the
// workers live inside namespace atari and the shared options UI calls these.
void atariBrowseEnter(const char *path) { atari::browseEnter(path); }
void atariBrowseUp()                    { atari::browseUp(); }

// ---- startup ROM-skip warning overlay (runs on the core-0 render task, which owns the TFT) ----
static void atariDrawWarning() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("ATARI 2600: ROM NOTE", 8, 8, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  int y = 38;
  const char *p = atari::loadWarn;
  char line[64];
  while (*p && y < 172) {
    int n = 0;
    while (*p && *p != '\n' && n < 62) line[n++] = *p++;
    line[n] = 0;
    if (*p == '\n') p++;
    if (n) tft.drawString(line, 8, y, 1);
    y += 14;
  }
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("Supports: .a26/.bin 2K/4K + F8/F6/F4 (8/16/32K).", 8, 182, 1);
  tft.drawString("e.g. Combat, Pitfall, River Raid, Pac-Man, Adventure", 8, 196, 1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Put .a26 or .bin files on the SD card root.", 8, 216, 1);
  tft.setTextDatum(MC_DATUM);                  // restore the datum the rest of the UI expects
}

bool atariRenderLoadWarning() {
  if (!atari::loadWarn || !atari::loadWarn[0]) return false;
  static uint32_t until = 0;
  static bool started = false, drawn = false;
  if (!started) { until = millis() + 6000; started = true; }   // ~6s after the game first renders
  if ((int32_t)(millis() - until) >= 0) { atari::loadWarn[0] = 0; return false; }
  if (!drawn) { atariDrawWarning(); drawn = true; }            // draw once, then hold
  return true;
}
