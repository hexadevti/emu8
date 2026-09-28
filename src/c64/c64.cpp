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
// Set when the 64K RAM block could not be allocated. Also halts the 6510 (via c64RomLoadFailed):
// running it on a null `ram` wrote through address 0, which faults on the RP2040 and took core 0
// -- and the USB port with it -- down, leaving the system menu frozen on the panel.
static bool c64NoRam = false;

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

// PicoCalc: take the 64K RAM block at the very start of the C64 boot, while the heap is still in
// one piece. Mounting the SD card first can split it so that no 64K run is left on the RP2040.
// (The ESP32 boards keep the old order: their SD DMA buffer needs low DRAM before this.)
void c64ReserveRam() { c64::memoryAlloc(); }

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
  if (!c64::ram) {
    c64NoRam = c64RomLoadFailed = true;            // halt: the render loop holds the error screen
    sprintf(buf, "C64: no 64K block for RAM (free heap=%u) - halted", (unsigned)ESP.getFreeHeap());
    printLog(buf);
    return;
  }
  if (!c64LoadRoms()) {                            // BASIC/KERNAL/CHARGEN from /roms/c64 on the SD
    c64RomLoadFailed = true;                       // halt: c64Loop holds the error screen
    printLog("C64: ROMs missing on SD (/roms/c64) - put basic.bin/kernal.bin/chargen.bin there");
    return;                                        // skip VIC/CIA; do NOT run the 6510 on null ROMs
  }
  c64::vicSetup(c64::ram, charset_rom);            // framebuffer (best-effort)
  printLog("C64: build " __DATE__ " " __TIME__ " (dirty bands, 1:3 cadence, SID gain x8)");
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

// renderLoop hook (src/shared/video.cpp): held while the SD card is missing the /roms/c64 system
// ROMs -- there is nothing to run. Yields to SETTINGS (ROMS can point at other files) and redraws
// once each time it closes.
bool c64RenderLoadWarning() {
  if (!c64RomLoadFailed) return false;
  static bool drawn = false;
  if (OptionsWindow) { drawn = false; return false; }
  if (drawn) return true;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  if (c64NoRam) {
    tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK); tft.drawString("C64: NOT ENOUGH MEMORY", 8, 8, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("No 64K block free for the C64's RAM.", 8, 40, 1);
    tft.drawString("Ctrl-F6: pick another system.", 8, 56, 1);
    tft.setTextDatum(MC_DATUM);
    drawn = true;
    return true;
  }
  tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK); tft.drawString("C64: ROMs NOT FOUND", 8, 8, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Put basic.bin, kernal.bin, chargen.bin", 8, 40, 1);
  tft.drawString("in /roms/c64 on the SD card, or pick", 8, 56, 1);
  tft.drawString("other files in SETTINGS (Ctrl-F1) > ROMS.", 8, 72, 1);
  tft.setTextDatum(MC_DATUM);
  drawn = true;
  return true;
}

void c64Loop() {
  if (c64RomLoadFailed) { delay(50); return; }   // halt; the render loop holds the error screen
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
#if defined(BOARD_PICOCALC)
// Framebuffer handoff with the VIC (see the frame-pacing note in c64_vic.cpp). Before the push: wait
// for the VIC to start a frame. For each band: wait until the VIC has drawn its last line. Both
// waits give up after a while (emulation paused, reset or stalled) and push what is there.
static bool c64FbChase;
static uint32_t c64FbWaitMs;
static void c64FbBegin() {
  uint32_t t0 = millis();
  while (c64::vicFbState == c64::VIC_FB_FREE && millis() - t0 < 100) vTaskDelay(1);
  c64FbChase = c64::vicFbState != c64::VIC_FB_FREE;
  c64FbWaitMs = millis();
}
static void c64FbNeedLine(int dline) {   // dline 0..199 must be complete
  while (c64FbChase && c64::vicFbState == c64::VIC_FB_WRITING && c64::vicLinesDone <= dline) {
    if (millis() - c64FbWaitMs > 100) { c64FbChase = false; break; }
    vTaskDelay(1);
  }
  __asm volatile("dmb" ::: "memory");
}
static void c64FbEnd() { c64::vicFbState = c64::VIC_FB_FREE; }   // the VIC may draw the next frame

// TEMP debug: 'D' on the serial port sends the next 40 drawn frames, each as
// "C64FBDMP" | drawn frame no u32 | frame no now u32 | chase ok u8 | vicreg[64] | 64000 fb bytes | 200 x (d011 d016 d018 bank).
// The log is muted meanwhile so no line lands inside a frame.
static uint8_t c64DumpLeft = 0;
static void c64DumpFrame() {
  while (Serial.available() && Serial.peek() == 'D') { Serial.read(); c64DumpLeft = 40; }   // other bytes are someone else's (PicoCalc loader command)
  if (!c64DumpLeft) return;
  c64FbNeedLine(199);
  sdSerialActive = true;
  uint8_t hdr[8 + 4 + 4 + 1 + 64];
  memcpy(hdr, "C64FBDMP", 8);
  uint32_t a = c64::vicDrawnFrameNo, b = c64::vicFrameNo;
  memcpy(hdr + 8, &a, 4); memcpy(hdr + 12, &b, 4);
  hdr[16] = c64FbChase;
  memcpy(hdr + 17, c64::vicreg, 64);
  Serial.write(hdr, sizeof(hdr));
  Serial.write(c64::fbTop, 32000);
  Serial.write(c64::fbBot, 32000);
  Serial.write(&c64::vicLineRegs[0][0], 800);
  Serial.flush();
  if (--c64DumpLeft == 0) sdSerialActive = false;
}

// Dirty bands. The full picture costs ~60 ms of SPI, which is what held the panel at 12-14 fps. Most
// C64 screens change in a few places per frame (a scroller, a score, some sprites), so each 8-line
// band is checksummed and only the ones that changed are sent -- a title-screen scroller then goes
// out at the VIC's full 16.7 fps, at an even step. Everything is re-sent after a gap in rendering
// (a menu was drawn over it), when the border colour changes, and once a second regardless, so an
// overlay drawn over the picture by anything else never lingers.
static uint32_t c64BandSum(int dline, int n) {
  uint32_t h = 2166136261u;
  for (int l = 0; l < n; l++) {
    const uint32_t *w = (const uint32_t *)c64FbLine(dline + l);   // lines are 4-byte aligned
    for (int i = 0; i < 320 / 4; i++) h = (h ^ w[i]) * 16777619u;
  }
  return h;
}
#define C64_DIRTY_BANDS 1
#else
static inline void c64FbBegin() {}
static inline void c64FbNeedLine(int) {}
static inline void c64FbEnd() {}
static inline void c64DumpFrame() {}
#endif

static void c64RenderBitmap() {
  uint16_t *band = c64Scratch;
  if (!band) return;
  c64FbBegin();
  uint16_t border = c64::c64Colors[c64::vicreg[0x20] & 15];
  tft.setSwapBytes(true);
#if defined(C64_DIRTY_BANDS)
  static uint32_t bandSum[25];
  static uint8_t lastBorder = 0xFF;
  static bool lastOsk = true;
  static uint32_t lastCallMs = 0, lastFullMs = 0;
  const uint32_t now = millis();
  const bool full = (now - lastCallMs > 150) || (now - lastFullMs > 1000) ||
                    ((c64::vicreg[0x20] & 15) != lastBorder) || lastOsk;
  lastCallMs = now;
  lastOsk = oskActive();
  if (full) { lastFullMs = now; lastBorder = c64::vicreg[0x20] & 15; }
#else
  const bool full = true;
#endif
  if (!oskActive()) {
    if (full) {
      tft.fillRect(0, 0, 320, 20, border);
      tft.fillRect(0, 220, 320, 20, border);
    }
    for (int dline = 0; dline < 200; dline += 8) {    // 25 bands of 8 lines (cross the half boundary)
      c64FbNeedLine(dline + 7);
#if defined(C64_DIRTY_BANDS)
      const uint32_t sum = c64BandSum(dline, 8);
      if (!full && sum == bandSum[dline >> 3]) continue;   // unchanged since it was last sent
      bandSum[dline >> 3] = sum;
#endif
      for (int n = 0; n < 8; n++) c64ConvertLine(&band[n * 320], dline + n);
      tft.pushImage(0, 20 + dline, 320, 8, band);
    }
  } else {
    const int H = oskRasterHeight();                  // 112 output lines
    const int S = 200;                                // source lines
    for (int oy = 0; oy < H; ) {                       // batch up to 8 scaled lines per push
      int n = 0;
      int last = oy + 7 < H - 1 ? oy + 7 : H - 1;
      c64FbNeedLine(last * S / H);
      while (oy + n < H && n < 8) { c64ConvertLine(&band[n * 320], (oy + n) * S / H); n++; }
      tft.pushImage(0, oy, 320, n, band);
      oy += n;
    }
  }
  c64DumpFrame();
  c64FbEnd();
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
  p.toLowerCase();                // .Crt too: anything else would be flashed from the READY trap
  if (p.endsWith(".crt")) c64LoadCRT(selectedC64FileName.c_str());
  else                                          c64AutoloadPending = true;
  snprintf(buf, sizeof(buf), "C64 autoload: %.100s", selectedC64FileName.c_str());
  printLog(buf);
}
