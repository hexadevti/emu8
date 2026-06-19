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

  // 256x240 8-bit indexed framebuffer lives in the shared static buffer (mutually exclusive
  // with Apple RAM / the C64 framebuffer); 256*240 = 61440 <= sizeof(sharedBigBuf).
  nes::framebuffer = sharedBigBuf;
  memset(sharedBigBuf, 0, NES_W * NES_H);

  // 2K CPU RAM + 2K nametable VRAM are malloc'd here (not static BSS — see nes.h).
  nes::cpuRam = (uint8_t *)malloc(0x800);
  nes::vram   = (uint8_t *)malloc(0x800);
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

// Convert the indexed framebuffer to RGB565 and push it to the TFT, centred with pillarbox
// borders. Runs on the core-0 render task (which owns the TFT), like c64RenderFrame.
void nesRenderFrame() {
  if (!nesScratch || !nes::framebuffer) return;
  const uint16_t *pal = videoColor ? nes::nesPalette : nes::nesPaletteGray;  // VIDEO color/mono
  tft.fillRect(0, 0, NES_OX, NES_H, TFT_BLACK);                 // left border
  tft.fillRect(NES_OX + NES_W, 0, 320 - (NES_OX + NES_W), NES_H, TFT_BLACK); // right border
  tft.setSwapBytes(true);
  for (int y = 0; y < NES_H; ) {
    int n = 0;
    while (y + n < NES_H && n < 8) {
      const uint8_t *src = nes::framebuffer + (y + n) * NES_W;
      uint16_t *dst = nesScratch + n * NES_W;
      for (int xx = 0; xx < NES_W; xx++) dst[xx] = pal[src[xx] & 0x3F];
      n++;
    }
    tft.pushImage(NES_OX, y, NES_W, n, nesScratch);
    y += n;
  }
  tft.setSwapBytes(false);
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
  if (!nes::loadWarn[0]) return false;
  static uint32_t until = 0;
  static bool started = false, drawn = false;
  if (!started) { until = millis() + 6000; started = true; }   // ~6s after the game first renders
  if ((int32_t)(millis() - until) >= 0) { nes::loadWarn[0] = 0; return false; }
  if (!drawn) { nesDrawWarning(); drawn = true; }              // draw once, then hold
  return true;
}
