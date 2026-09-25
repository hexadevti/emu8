// display_picocalc.h -- ClockworkPi PicoCalc display backend. Declares `class DisplayGFX` with the
// same TFT_eSPI method subset the emulator/UI call, so emu.h's `extern DisplayGFX tft;` and
// globals.cpp's definition are unchanged (same contract as src/shared/display_gfx.h and
// src/desktop/display_sdl.h -- three interchangeable classes, all named DisplayGFX).
//
// UNLIKE the Arduino_GFX backend there is NO canvas/framebuffer: a 320x240 RGB565 buffer would eat
// 150KB of the RP2350's 520KB SRAM, and the shared renderer does not want one anyway -- it streams
// the Apple II raster pixel-by-pixel through setAddrWindow()/writeColor(). So this backend draws
// STRAIGHT to the panel, exactly like the CYD's TFT_eSPI path.
//
// LETTERBOX: the panel is 320x320 but the emulator's logical screen is 320x240, so logical row 0
// lands on panel row DISP_OFFSET_Y (40) and rows 0..39 / 280..319 stay black (fillPanelBlack()).
// Every logical->panel mapping in the .cpp is that one constant; there is no scaling anywhere
// (SCREEN: FILL is done by the MSX/SMS renderers themselves: they scale into the 320x240 window).
#pragma once

#include "../../board.h"

#if defined(BOARD_PICOCALC)

#include <Arduino.h>
#include <stdint.h>

// RGB565 color constants (TFT_eSPI provides these on the CYD build).
#define TFT_BLACK     0x0000
#define TFT_WHITE     0xFFFF
#define TFT_GREEN     0x07E0
#define TFT_PURPLE    0x780F
#define TFT_SKYBLUE   0x867D
#define TFT_YELLOW    0xFFE0
#define TFT_DARKGREY  0x7BEF

// text datum constants (subset used by the UI)
#define TL_DATUM 0
#define TC_DATUM 1
#define TR_DATUM 2
#define ML_DATUM 3
#define MC_DATUM 4
#define MR_DATUM 5
#define BL_DATUM 6
#define BC_DATUM 7
#define BR_DATUM 8

#define DISP_LOGICAL_W 320
#define DISP_LOGICAL_H 240
// Top of the 320x240 logical screen inside the 320x320 panel. (320-240)/2 = 40.
#define DISP_OFFSET_Y  ((PANEL_NATIVE_H - DISP_LOGICAL_H) / 2)

// --- writeColor() staging (see display_picocalc.cpp) -------------------------------------------
// writeColor() is called ONCE PER PIXEL from inside the HGR/DHGR decode loops in video.cpp, so it
// must not touch SPI hardware per call. It appends into a ping-pong staging buffer; a full half is
// handed to DMA while the decoder keeps filling the other half. Only the append is inline here --
// everything that talks to the hardware lives out-of-line in the .cpp.
namespace pcd {
  static const uint32_t STAGE_LEN  = DISP_LOGICAL_W;  // one scanline per half
  static const uint32_t BULK_MIN   = 256;             // len >= this -> single no-CPU DMA descriptor
  extern uint16_t stage[2][STAGE_LEN];
  extern uint32_t stageFill;                          // pixels held in stage[stageHalf]
  extern uint8_t  stageHalf;
  void stageKick();                                   // DMA the full half, flip, reset stageFill
  void stageDrain();                                  // push a partial half and join the transfer
  void bulkFill(uint16_t color, uint32_t len);        // read-increment-off DMA of `len` copies
}

class DisplayGFX {
public:
  // lifecycle
  void begin();                     // reset + ST7365P/ILI9488 init, claim the DMA channel
  void setRotation(uint8_t) {}      // orientation is fixed by MADCTL in begin()
  void invertDisplay(bool) {}       // the init sequence already issues INVON (the panel needs it)
  void initDMA() {}                 // the channel is claimed in begin(); nothing left to do
  void setUiMode(bool ui) { _uiMode = ui; }   // 1:1 either way; tracked only
  // FULL-PANEL mode: logical space becomes the whole 320x320 panel (no letterbox offset). Only
  // the settings menu uses it (src/shared/optionsui.cpp turns it on; video.cpp turns it off and
  // re-blackens the bars when the menu closes). Returns true if the mode actually changed.
  bool setFullPanel(bool on);
  bool fullPanel() const { return _offY == 0; }
  int32_t height() const { return _logH; }

  // primitives
  void fillScreen(uint16_t color);  // the LOGICAL area only: 320x240, or 320x320 in full-panel mode
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
  void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color);
  void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color);

  // 8x8 glyph nearest-scaled into a pw x ph cell (the PC-XT/CGA text renderer uses this)
  void drawGlyph8(int px, int py, int pw, int ph, const uint8_t *g, uint16_t fg, uint16_t bg, bool transparentBg = false);

  // bitmaps
  void setSwapBytes(bool s) { _swap = s; }   // tracked only: 16-bit SPI frames already go out
                                             // MSB-first, i.e. native RGB565 (same as the
                                             // Arduino_GFX and SDL backends, which also ignore it)
  void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data);
  void setBypassCanvas(bool) {}     // canvas fast-path flag; there is no canvas here
  void pushPanelBand(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data);
  void drawCanvasRGB565(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data);  // RAW panel rows
  void fillPanelBlack();            // the two 320x40 letterbox bars
  void fillPanelRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);  // RAW panel coords
  void flushOskBand() {}            // DSI-only partial flush; nothing is buffered here
  // On-screen-keyboard overlay: no touchscreen on this board, so the overlay never opens.
  void oskOverlayBegin() {}
  void oskOverlayEnd() {}
  bool inOskOverlay() const { return false; }
  void oskOverlayFillCircle(int, int, int, uint16_t) {}

  // Apple II scanline window (the hot path -- see the namespace above)
  void setAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h);
  void startWrite();
  inline void writeColor(uint16_t color, uint32_t len) {
    if (len >= pcd::BULK_MIN) { pcd::bulkFill(color, len); return; }
    while (len--) {
      pcd::stage[pcd::stageHalf][pcd::stageFill++] = color;
      if (pcd::stageFill >= pcd::STAGE_LEN) pcd::stageKick();
    }
  }
  void endWrite();

  // text
  void    setTextDatum(uint8_t d) { _datum = d; }
  void    setTextColor(uint16_t fg) { _textFg = fg; _textHasBg = false; }
  void    setTextColor(uint16_t fg, uint16_t bg) { _textFg = fg; _textBg = bg; _textHasBg = true; }
  int16_t drawString(const char *s, int32_t x, int32_t y, uint8_t font);
  int16_t drawString(const char *s, int32_t x, int32_t y) { return drawString(s, x, y, 1); }
  int16_t drawString(const String &s, int32_t x, int32_t y) { return drawString(s.c_str(), x, y, 1); }
  int16_t drawString(const String &s, int32_t x, int32_t y, uint8_t font) { return drawString(s.c_str(), x, y, font); }
  int16_t textWidth(const char *s, uint8_t font);   // what drawString() would return, without drawing
  // RAW panel coordinates, the drawString counterpart to fillPanelRect(): the only way to put text
  // in the letterbox bars, which the logical 320x240 clip in pxRun() would otherwise throw away.
  int16_t drawPanelString(const char *s, int32_t x, int32_t y, uint8_t font);

  static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }

  void flush();                     // nothing to present (drawing is direct); pumps the keyboard

  // touch: this board has none. Kept so touchkeyboard.cpp / osg.cpp still link.
  uint16_t getTouchRawZ() { return 0; }
  void     getTouchRaw(uint16_t *x, uint16_t *y) { if (x) *x = 0; if (y) *y = 0; }
  void     setMouseState(int, int, bool) {}

private:
  // Single-pixel writes (text, rounded corners) coalesced into horizontal runs: pxRun() extends the
  // pending run while the caller walks a row left-to-right, pxFlush() emits it as ONE panel write.
  // Without this every set glyph pixel would cost its own CASET/RASET/RAMWR (~5us).
  void pxRun(int32_t x, int32_t y, uint16_t color);
  void pxFlush();

  bool      _uiMode = true;
  uint8_t   _datum = TL_DATUM;
  uint16_t  _textFg = TFT_WHITE, _textBg = TFT_BLACK;
  bool      _textHasBg = false;
  bool      _rawText = false;       // drawPanelString(): skip the logical clip + DISP_OFFSET_Y
  bool      _swap = false;
  int32_t   _spanX = 0, _spanY = -1, _spanLen = 0;   // pending horizontal run (_spanY < 0 = none)
  uint16_t  _spanColor = 0;
  int32_t   _offY = DISP_OFFSET_Y;  // panel row of logical row 0 (0 in full-panel mode)
  int32_t   _logH = DISP_LOGICAL_H; // logical height (PANEL_NATIVE_H in full-panel mode)
};

void displayFlush();
void displaySetUiMode(bool ui);
void displaySetVideoRect(int topLogical, int hLogical);
void displaySetVideoFill(int leftLogical, int wLogical, bool stretch);

#endif // BOARD_PICOCALC
