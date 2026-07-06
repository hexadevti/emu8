#include "../../emu.h"
#include "c64.h"
#if BOARD_HAS_BLE
#include "esp_bt.h"   // to release the unused Bluetooth controller's reserved DRAM
#endif               // (absent on the radio-less ESP32-P4)

// Shared render scratch (one 320x8 line band). MALLOC'd on the C64 path only - NOT a static
// array, because a static would be reserved in BSS for the Apple path too (~10K) and starve
// its already-fragmented heap (disk tasks can't allocate). c64RenderBitmap and the text-mode
// renderer never run at the same time, so they share this one buffer.
static uint16_t *c64Scratch = nullptr;

// Set when the SD card has no usable /roms/c64 ROMs: the 6510 is left halted (it would
// dereference the null ROM pointers) and c64Loop holds an error screen instead.
static bool c64RomLoadFailed = false;

// C64 core entry points (C linkage), called by the platform dispatch:
//   setup() -> c64Setup(),  loop() -> c64Loop(),  renderLoop() -> c64RenderFrame().

// Release the unused Bluetooth controller's reserved DRAM (this board has no BLE since
// ble.ino was removed). Called at the very start of the C64 boot, BEFORE the SD mount and
// the big C64 allocations, so the reclaimed DMA-capable memory is available to all of them.
void c64FreeBtMem() {
#if BOARD_HAS_BLE
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
#endif
  // The ESP32-P4 has no Bluetooth controller, so there is nothing to release there.
}

void c64Setup() {
  printLog("C64 Setup...");
  // The C64 path skips the Apple memoryAlloc(), but the shared text interface
  // (clearScreen/print, used when the settings window opens) writes to these two
  // buffers, so allocate them here to avoid a null-pointer store.
  menuScreen = (unsigned char *)malloc(0x546);
  menuColor  = (unsigned char *)malloc(0x546);
  // 64K RAM FIRST (essential - needs one contiguous 64K block; if it loses the race the
  // machine can't run). Then the VIC framebuffer (two 32K halves) fits in the leftovers; if
  // it can't, we fall back to text mode (drawRasterline is gated on `bitmap`), no crash.
  c64::memoryAlloc();                              // 64K RAM (first - needs a contiguous block)
  if (!c64LoadRoms()) {                            // BASIC/KERNAL/CHARGEN from /roms/c64 on the SD
    c64RomLoadFailed = true;                       // halt: c64Loop holds the error screen
    printLog("C64: ROMs missing on SD (/roms/c64) - put basic.bin/kernal.bin/chargen.bin there");
    return;                                        // skip VIC/CIA; do NOT run the 6510 on null ROMs
  }
  c64::vicSetup(c64::ram, charset_rom);            // framebuffer (best-effort)
  if (c64::bitmap) printLog("C64: VIC framebuffer ON (bitmap/sprites/multicolor)");
  else             printLog("C64: framebuffer alloc FAILED -> text-mode only");
  c64Scratch = (uint16_t *)malloc(320 * 8 * sizeof(uint16_t));   // shared render band (small)
  c64::ciaReset();
  c64::kbReset();
  c64::register1 = 0x37;
  c64::decodeRegister1(0x37 & 7);                  // default banking: BASIC + KERNAL + I/O
  // NOTE: SID/I2S is initialised LAST (after FSSetup) in setup() — the I2S DMA buffers must
  // not be allocated before the SD card mounts, or SD's own DMA allocation fails on the
  // fragmented C64 heap ("Card Mount Failed").
  sprintf(buf, "C64 ready. free heap=%u", (unsigned)ESP.getFreeHeap());
  printLog(buf);
}

// Held forever when the SD card is missing the /roms/c64 system ROMs: there is nothing to run.
static void c64ShowRomError() {
  static bool drawn = false;
  if (drawn) return;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK); tft.drawString("C64: ROMs NOT FOUND", 8, 8, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Put basic.bin, kernal.bin, chargen.bin", 8, 40, 1);
  tft.drawString("in /roms/c64 on the SD card.", 8, 56, 1);
  tft.setTextDatum(MC_DATUM);
  drawn = true;
}

void c64Loop() {
  if (c64RomLoadFailed) { c64ShowRomError(); delay(50); return; }   // halt, hold the error screen
  c64::cpuLoop();   // runs forever (6510 + VIC raster + CIA)
}

// Build one 320px scanline of the text screen for source scanline sy (0..199) into buf.
static inline void c64BuildScanline(uint16_t *buf, int sy, uint16_t bg) {
  int cy = sy >> 3, row = sy & 7;
  uint16_t *p = buf;
  for (int cx = 0; cx < 40; cx++) {
    uint8_t ch = c64::ram[c64::screenmemstart + cy * 40 + cx];
    uint8_t cdata = c64::charset[(ch << 3) + row];
    uint16_t fg = c64::c64Colors[c64::colormap[cy * 40 + cx] & 15];
    for (uint8_t bitval = 128; bitval; bitval >>= 1)
      *p++ = (cdata & bitval) ? fg : bg;
  }
}

// Start of display line `dline` (0..199) in the correct 8-bit framebuffer half.
static inline const uint8_t *c64FbLine(int dline) {
  return (dline < 100) ? (c64::fbTop + dline * 320) : (c64::fbBot + (dline - 100) * 320);
}

// Convert one indexed framebuffer line (0..199) to RGB565 in dst (320 px).
static inline void c64ConvertLine(uint16_t *dst, int dline) {
  const uint8_t *src = c64FbLine(dline);
  for (int x = 0; x < 320; x++) dst[x] = c64::c64Colors[src[x] & 15];
}

// Push the full VIC-II framebuffer (320x200, 8-bit indexed, built scanline-by-scanline by
// drawRasterline during the emulated frame; converted to RGB565 here). Centred with a
// top/bottom border when the keyboard is hidden; vertically flattened into the top 112px
// when it is open (so all 200 lines stay visible above the keys).
static void c64RenderBitmap() {
  uint16_t *band = c64Scratch;
  if (!band) return;
  uint16_t border = c64::c64Colors[c64::vicreg[0x20] & 15];
  tft.setSwapBytes(true);
  if (!oskActive()) {
    tft.fillRect(0, 0, 320, 20, border);
    tft.fillRect(0, 220, 320, 20, border);
    for (int dline = 0; dline < 200; ) {              // 8-line bands (cross the half boundary)
      int n = 0;
      while (dline + n < 200 && n < 8) { c64ConvertLine(&band[n * 320], dline + n); n++; }
      tft.pushImage(0, 20 + dline, 320, n, band);
      dline += n;
    }
  } else {
    const int H = oskRasterHeight();                  // 112 output lines
    const int S = 200;                                // source lines
    for (int oy = 0; oy < H; ) {                       // batch up to 8 scaled lines per push
      int n = 0;
      while (oy + n < H && n < 8) { c64ConvertLine(&band[n * 320], (oy + n) * S / H); n++; }
      tft.pushImage(0, oy, 320, n, band);
      oy += n;
    }
  }
  tft.setSwapBytes(false);
}

// Render a frame. Uses the full VIC-II framebuffer when it was allocated; otherwise falls
// back to the direct text-mode renderer below (standard char mode only).
//
// Text fallback: the 40x25 screen draws 1:1 (200px, centred + border); when the touch
// keyboard is open the whole screen is FLATTENED (scaled) into the top 112px.
void c64RenderFrame() {
  if (c64::fbTop) { c64RenderBitmap(); return; }

  uint16_t *lineBuf = c64Scratch;     // shared render band (up to 8 scanlines per push)
  if (!lineBuf) return;
  uint16_t bg     = c64::c64Colors[c64::vicreg[0x21] & 15];
  uint16_t border = c64::c64Colors[c64::vicreg[0x20] & 15];
  if (!c64::ram || !c64::colormap || !c64::charset) return;

  if (oskActive()) {
    const int H = oskRasterHeight(); // 112 output lines (rows above the keyboard)
    const int S = 25 * 8;            // 200 source lines
    for (int oy = 0; oy < H; ) {     // batch up to 8 output lines per pushImage
      int n = 0;
      while (oy + n < H && n < 8) {
        c64BuildScanline(&lineBuf[n * 320], (oy + n) * S / H, bg);
        n++;
      }
      tft.setSwapBytes(true);
      tft.pushImage(0, oy, 320, n, lineBuf);
      tft.setSwapBytes(false);
      oy += n;
    }
    return;
  }

  tft.fillRect(0, 0, 320, 20, border);
  tft.fillRect(0, 220, 320, 20, border);
  for (int cy = 0; cy < 25; cy++) {
    for (int row = 0; row < 8; row++)
      c64BuildScanline(&lineBuf[row * 320], cy * 8 + row, bg);
    tft.setSwapBytes(true);
    tft.pushImage(0, 20 + cy * 8, 320, 8, lineBuf);
    tft.setSwapBytes(false);
  }
}

namespace c64 {
const uint16_t *getC64Colors() { return c64Colors; }
void drawFrame(uint16_t frameColor) { (void)frameColor; }  // border drawn in c64RenderFrame
} // namespace c64

// Touch keyboard -> CIA1 keyboard matrix (C-linkage wrapper for src/shared/touchkeyboard.cpp).
void c64KeyMatrix(uint8_t row, uint8_t col, bool down) { c64::kbSetKey(row, col, down); }

// Joystick -> CIA1 (C-linkage wrapper for src/shared/joystick.cpp). mask is active-low.
// Routed to port 2 ($DC00) or port 1 ($DC01) per the joyPort setting; the other port is released.
void c64SetJoystick(uint8_t mask) { c64::kbSetJoystickPort(joyPort, mask); }

// Boot-autoload: if enabled and an image is remembered, launch it. A .crt mounts now (its
// reset autostarts it); a .prg/.d64 is deferred until the KERNAL reaches the BASIC READY
// prompt (handled by a one-shot trap in cpuLoop), since the machine isn't ready yet at setup.
void c64Autostart() {
  if (c64RomLoadFailed) return;   // no ROMs -> the machine never booted; nothing to autoload into
  if (!c64Autoload || selectedC64FileName.length() < 2) return;
  String p = selectedC64FileName;
  if (p.endsWith(".crt") || p.endsWith(".CRT")) c64LoadCRT(p.c_str());
  else                                          c64AutoloadPending = true;
  sprintf(buf, "C64 autoload: %s", p.c_str());
  printLog(buf);
}
