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

// Set when the SD card has no usable /roms C64 ROMs: the 6510 is left halted (it would
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
  // They, and the VIC colour RAM, live in the Apple II's Disk II track buffer: the platforms are
  // mutually exclusive, and the running C64 has almost no heap to spare (the renderer is in SRAM).
#if defined(BOARD_PICOCALC)
  extern uint8_t *const trackEncodedData;   // a pointer into platformPool here (apple2/disk.cpp)
#else
  extern uint8_t trackEncodedData[];
#endif
  menuScreen = trackEncodedData;
  menuColor  = trackEncodedData + 0x546;
  c64::colormap = trackEncodedData + 2 * 0x546;    // 1K color RAM ($D800); 3724 of the 5856 bytes
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
  if (!c64LoadRoms()) {                            // BASIC/KERNAL/CHARGEN from /roms on the SD
    c64RomLoadFailed = true;                       // halt: c64Loop holds the error screen
    printLog("C64: ROMs missing on SD - put VICE's basic-901226-01.bin/kernal-901227-03.bin/chargen-901225-01.bin in /roms");
    return;                                        // skip VIC/CIA; do NOT run the 6510 on null ROMs
  }
  c64::vicSetup(c64::ram, charset_rom);            // framebuffer (best-effort)
  printLog("C64: build " __DATE__ " " __TIME__ " (video: even frames, 25 fps)");
  if (c64::bitmap) printLog("C64: VIC framebuffer ON (bitmap/sprites/multicolor)");
  else             printLog("C64: framebuffer alloc FAILED -> text-mode only");
  // The text fallback's band; never needed while vicSetup() provides the line records.
  if (!c64::vicRec) c64Scratch = (uint16_t *)malloc(320 * 8 * sizeof(uint16_t));
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

// renderLoop hook (src/shared/video.cpp): held while the SD card is missing the /roms C64 system
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
#if defined(BOARD_PICOCALC)
    tft.drawString("Ctrl-Shift-F1: pick another system.", 8, 56, 1);
#else
    tft.drawString("Ctrl-F6: pick another system.", 8, 56, 1);
#endif
    tft.setTextDatum(MC_DATUM);
    drawn = true;
    return true;
  }
  tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK); tft.drawString("C64: ROMs NOT FOUND", 8, 8, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Put basic-901226-01.bin, kernal-901227-03", 8, 40, 1);
  tft.drawString(".bin, chargen-901225-01.bin in /roms, or pick", 8, 56, 1);
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

// Draw and push one C64 frame (see the video note in c64_vic.cpp). The emulator core records each
// visible line's VIC state as the beam passes it; this draws the line from that record right
// behind the beam, converts it to RGB565 into a ring of 8-line strips (in sharedBigBuf) and streams
// the strips to the panel by DMA while it draws the next lines. Every frame is drawn, 50 a second.
// If this falls a whole frame behind (a record it needs was overwritten), the frame is dropped.
// No frame within 100 ms (paused, reset, menu): nothing is pushed and the panel keeps the last one.
// Centred with a top/bottom border when the keyboard is hidden; vertically flattened into the top
// 112px when it is open (so all 200 lines stay visible above the keys).
#if defined(BOARD_PICOCALC)
#define C64_BARRIER() __asm volatile("dmb" ::: "memory")
static inline void c64StripPush(int y, int n, const uint16_t *p) { tft.pushImageAsync(0, y, 320, n, p); }
static inline bool c64StripBusy() { return tft.pushBusy(); }
static inline void c64StripWait() { tft.pushWait(); }
#else
#define C64_BARRIER() __sync_synchronize()
static inline void c64StripPush(int y, int n, const uint16_t *p) { tft.pushImage(0, y, 320, n, p); }
static inline bool c64StripBusy() { return false; }
static inline void c64StripWait() {}
#endif

// The strip ring: strips are filled in order; `filled` of them are complete, `started` have been
// handed to the DMA (at most one is in flight: starting one waits for the previous). The counts
// run on across frames, so the tail of one frame is still going out while the next is drawn --
// draining the ring at every frame end serialised drawing and SPI and cost a frame in four.
static uint16_t *c64Strip(uint32_t i) {
  return (uint16_t *)sharedBigBuf + (i % VIC_STRIPS) * (VIC_STRIP_LINES * 320);
}
// A strip's changed rows (bit r = row r) go out as runs, one DMA each; unchanged rows are not even
// converted to RGB565, so their place in the strip holds whatever an older strip left there.
static int c64StripY[VIC_STRIPS];
static uint8_t c64StripMask[VIC_STRIPS];
static uint32_t c64Filled, c64Started;
static void c64StripService(bool block) {
  if (c64Started != c64Filled && (block || !c64StripBusy())) {
    const int s = c64Started % VIC_STRIPS;
    uint8_t m = c64StripMask[s];
    if (m) {
      const int a = __builtin_ctz(m);
      int b = a;
      while (b < VIC_STRIP_LINES && (m >> b) & 1) b++;
      c64StripPush(c64StripY[s] + a, b - a, c64Strip(c64Started) + a * 320);
      m &= ~(((1u << b) - 1) & ~((1u << a) - 1));
      c64StripMask[s] = m;
    }
    if (!m) c64Started++;                    // every changed run of the strip is out
  }
}
// Strip `i` may be written once the one VIC_STRIPS before it has been sent.
static void c64StripClaim(uint32_t i) {
  while ((int32_t)(i - VIC_STRIPS - (c64Started - (c64StripBusy() ? 1 : 0))) >= 0) {
    if (c64Started != c64Filled) c64StripService(true);
    else c64StripWait();
  }
}
// Send everything queued and wait for it: before the panel is left to anyone else for a while.
static void c64StripFlush() {
  while (c64Started != c64Filled) c64StripService(true);
  c64StripWait();
}

// Stats for the serial log, once every 5 s: frames drawn whole, frames drawn with a tear (the
// emulator lapped this task mid-frame), frames skipped, and where the render core's time went.
static uint32_t c64VidFrames, c64VidTorn, c64VidSkipped, c64VidLogMs;
static uint32_t c64VidLagFrames;             // frames in which the drawing fell > 30 lines behind
static uint32_t c64VidLagSum, c64VidLagN, c64VidLagMax;
static uint32_t c64VidDrawUs, c64VidBeamUs, c64VidPushUs, c64VidRowsSent;

// What each output row showed last frame (a hash of its 320 colour indices). Only rows that
// changed go to the panel: the SPI is the bottleneck -- a full 320x200 frame is 16.4ms of the
// 20ms a PAL frame lasts even at 62.5MHz -- and most of a C64 screen is the same from one frame
// to the next. Invalid after anything else may have drawn over the picture.
#if defined(BOARD_PICOCALC)
static_assert(POOL_C64_ROWHASH + 200 * 4 <= PLATFORM_POOL_SIZE, "C64 row hashes overflow platformPool");
static uint32_t *const c64RowHash = (uint32_t *)(platformPool + POOL_C64_ROWHASH);   // shared pool, emu.h
#else
static uint32_t c64RowHash[200];
#endif
static bool c64RowHashValid = false;
static inline uint32_t c64LineHash(const uint8_t *line) {
  const uint32_t *w = (const uint32_t *)line;
  uint32_t h = 2166136261u;
  for (int i = 0; i < 80; i++) h = (h ^ w[i]) * 16777619u;
  return h;
}

// Copy line record `dline` of frame `f` (bank f & 1), waiting for the beam to reach it. If the
// emulator has lapped us and frame f + 2 already overwrote it, take that newer record: a tear
// across the frame beats leaving the rest of the screen stale. False: nothing within 100 ms
// (paused, reset, a menu up).
static bool c64GetRecord(uint8_t dline, uint8_t f, c64::VicLineRec &out, uint32_t t0, bool &torn) {
  const c64::VicLineRec *r = c64::vicRecFor(f, dline);
  for (;;) {
    const uint8_t rf = r->frame;
    if (rf != 0 && (rf == f || c64::vicRecFrame != f)) {
      // rf == f: ours. Otherwise the beam is past frame f, so the whole bank was written for f --
      // a different number means f + 2 has overwritten this line since.
      C64_BARRIER();
      memcpy(&out, r, sizeof(out));
      C64_BARRIER();
      if (r->frame == rf) {
        if (rf != f) torn = true;
        return true;
      }
      continue;                                           // rewritten while copying: again
    }
    if (millis() - t0 > 100) return false;
    // Spin rather than sleep: a 1 ms tick would leave the drawing 16 lines behind the beam, and
    // it draws a line in about the time the beam takes, so it would never catch up. Behind the
    // beam it reads RAM the program has already rewritten for the next lines.
    if (c64Started != c64Filled) c64StripService(false);
    if (millis() - t0 > 20) vTaskDelay(1);                // stalled: let the core breathe
  }
}

static uint8_t c64LastFrame = 0;

static void c64RenderBitmap() {
  alignas(4) static uint8_t line[320 + 16];
  // Wait for a frame we haven't drawn to start. If more than one started since the last one we
  // drew, take the newest: the older one's bank is being overwritten already.
  uint32_t t0 = millis();
  uint32_t tb = micros();
  uint8_t f;
  static uint32_t lastFrameUs = 0;
  while ((f = c64::vicRecFrame) == c64LastFrame || f == 0 || c64::vicRecFor(f, 0)->frame != f) {
    if (millis() - t0 > 100) { c64StripFlush(); return; }   // no frame coming: finish the last one
    if (c64Started != c64Filled) c64StripService(false);
    // Sleep until the next frame is about 1.5 ms away, then spin so its first line is drawn as
    // soon as it is recorded.
    const uint32_t since = micros() - lastFrameUs;
    if (since < 18500 || since > 25000) vTaskDelay(1);
  }
  lastFrameUs = micros();
  if (c64LastFrame) {
    const uint8_t want = c64LastFrame >= 254 ? 1 : c64LastFrame + 1;
    if (f != want) c64VidSkipped += (f - c64LastFrame + 254) % 254 - 1;
  }
  c64LastFrame = f;
  c64VidBeamUs += micros() - tb;
#if defined(BOARD_PICOCALC)
  // Only even frames reach the panel (25 fps). With every frame sent, Ghostbusters II flickered
  // about once a second and its scroller shook; showing the even frames alone was steady on the
  // device, the odd frames alone were not.
  if (f & 1) return;
#endif


  const bool osk = oskActive();
  const int outH = osk ? oskRasterHeight() : 200;   // output rows (112 when the keyboard is open)
  const int yOff = osk ? 0 : 20;
  // The picture may have been drawn over (a menu, the keyboard opening or closing): send it all.
  static bool lastOsk = true;
  static uint32_t lastCallMs = 0;
  if (millis() - lastCallMs > 150 || osk != lastOsk) c64RowHashValid = false;
  uint8_t chg = 0;                                  // changed rows of the current strip
  uint32_t frameLagMax = 0;
  c64::VicLineRec rec;
  const uint32_t base = c64Filled;                  // this frame's first strip
  tft.setSwapBytes(true);
  bool torn = false, stopped = false;
  uint8_t border = 0xFF;
  for (int o = 0; o < outH; o++) {
    const int dline = osk ? (o * 200 / outH) : o;
    const uint32_t si = base + o / VIC_STRIP_LINES;
    const int row = o % VIC_STRIP_LINES;
    uint32_t t = micros();
    if (row == 0) c64StripClaim(si);
    uint32_t t2 = micros();
    c64VidPushUs += t2 - t;
    if (!c64GetRecord((uint8_t)dline, f, rec, t0, torn)) { stopped = true; break; }
    {                                    // how many lines the beam is ahead of the drawing
      const uint16_t rl = c64::rasterline;
      int lag = (c64::vicRecFrame == f ? (int)rl - 51 : (int)rl + 312 - 51) - dline;
      if (lag < 0) lag = 0;
      c64VidLagSum += lag;
      c64VidLagN++;
      if ((uint32_t)lag > c64VidLagMax) c64VidLagMax = lag;
      if ((uint32_t)lag > frameLagMax) frameLagMax = lag;
    }
    c64StripService(false);
    t = micros();
    c64VidBeamUs += t - t2;
    if (dline == 0) border = rec.reg[0x20] & 15;
    c64::vicRenderRecord(rec, (uint8_t)dline, line, nullptr, nullptr);
    const uint32_t h = c64LineHash(line);
    if (!c64RowHashValid || h != c64RowHash[o]) {      // only a changed row is converted and sent
      c64RowHash[o] = h;
      c64::vicLineToRgb(line, c64Strip(si) + row * 320, c64::c64Colors);
      chg |= 1u << row;
    }
    c64VidDrawUs += micros() - t;
    if (row == VIC_STRIP_LINES - 1 || o == outH - 1) {
      const int s = si % VIC_STRIPS;
      c64StripY[s] = yOff + (o / VIC_STRIP_LINES) * VIC_STRIP_LINES;
      c64StripMask[s] = chg;
      c64VidRowsSent += __builtin_popcount(chg);
      chg = 0;
      c64Filled = si + 1;
      c64StripService(false);
    }
  }
  tft.setSwapBytes(false);
  if (stopped) {                           // the emulator stopped mid-frame; the panel keeps it
    c64StripFlush();
    c64RowHashValid = false;               // the rows past the stop were not sent
    return;
  }
  c64RowHashValid = true;
  if (torn) c64VidTorn++; else c64VidFrames++;
  if (frameLagMax > 30) c64VidLagFrames++;

  // The border strips are filled only when they may be stale: the colour changed, the keyboard was
  // just closed, or nothing was pushed for a while (a menu was drawn over the screen).
  static uint8_t lastBorder = 0xFF;
  const uint32_t now = millis();
  if (!osk && border != 0xFF &&
      ((now - lastCallMs > 150) || border != lastBorder || lastOsk)) {
    const uint16_t bc = c64::c64Colors[border];
    tft.fillRect(0, 0, 320, 20, bc);
    tft.fillRect(0, 220, 320, 20, bc);
    lastBorder = border;
  }
  lastCallMs = now;
  lastOsk = osk;

  if (now - c64VidLogMs >= 5000) {
    if (c64VidLogMs) {
      char vb[400];                        // longer than the shared 255-byte buf
      snprintf(vb, sizeof(vb), "C64 video: %lu whole + %lu torn frames, %lu skipped in %lu ms; ms drawing %lu, "
              "waiting beam %lu, waiting SPI %lu; %lu rows sent; lag avg %lu max %lu lines, %lu frames > 30; "
              "6510 %lu kHz, VIC %u%%, longest line gap %lu us; free heap %u, LCD SPI %lu kHz",
              (unsigned long)c64VidFrames, (unsigned long)c64VidTorn, (unsigned long)c64VidSkipped,
              (unsigned long)(now - c64VidLogMs), (unsigned long)(c64VidDrawUs / 1000),
              (unsigned long)(c64VidBeamUs / 1000), (unsigned long)(c64VidPushUs / 1000),
              (unsigned long)c64VidRowsSent,
              (unsigned long)(c64VidLagN ? c64VidLagSum / c64VidLagN : 0), (unsigned long)c64VidLagMax,
              (unsigned long)c64VidLagFrames, (unsigned long)c64::c64PerfKhz, (unsigned)c64::c64PerfVicPct,
              (unsigned long)c64::c64MaxLineGapUs,
              (unsigned)ESP.getFreeHeap(),
#if defined(BOARD_PICOCALC)
              (unsigned long)(lcdSpiHz() / 1000));
#else
              0UL);
#endif
      printLog(vb);
    }
    c64VidFrames = c64VidTorn = c64VidSkipped = 0;
    c64VidDrawUs = c64VidBeamUs = c64VidPushUs = c64VidRowsSent = 0;
    c64VidLagSum = c64VidLagN = c64VidLagMax = c64VidLagFrames = 0;
    c64::c64MaxLineGapUs = 0;
    c64VidLogMs = now;
  }
}

// Render a frame. Uses the full VIC-II framebuffer when it was allocated; otherwise falls
// back to the direct text-mode renderer below (standard char mode only).
//
// Text fallback: the 40x25 screen draws 1:1 (200px, centred + border); when the touch
// keyboard is open the whole screen is FLATTENED (scaled) into the top 112px.
void c64RenderFrame() {
  if (c64::vicRec) { c64RenderBitmap(); return; }

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
