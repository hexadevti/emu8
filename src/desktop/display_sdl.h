// display_sdl.h — desktop SDL2 display backend. Declares `class DisplayGFX` with the SAME TFT_eSPI
// method subset the emulator/UI call (so emu.h's `extern DisplayGFX tft;` and globals.cpp's
// definition are unchanged). Unlike the 480x272 Arduino_GFX backend, the desktop draws straight into
// a 320x240 RGB565 framebuffer (like the CYD/TFT_eSPI path) and SDL scales it to the window — no
// panel centering / fill-screen mapping needed.
#pragma once

#include "../../board.h"

#if defined(BOARD_DESKTOP)

#include <Arduino.h>

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

class DisplayGFX {
public:
  // lifecycle
  void begin();                     // create the SDL window/renderer/texture (called on main thread)
  void setRotation(uint8_t) {}
  void invertDisplay(bool) {}
  void initDMA() {}
  void setUiMode(bool ui) { _uiMode = ui; }   // desktop draws 1:1 either way; tracked only

  // fills / shapes
  void fillScreen(uint16_t color);
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
  void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color);
  void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color);

  // 8x8 glyph blit (nearest-scaled into a pw x ph cell) — authentic IBM CP437 font for PC-XT, like
  // the Arduino_GFX backend's drawGlyph8. fg/bg are RGB565; transparentBg leaves the cell bg untouched.
  void drawGlyph8(int px, int py, int pw, int ph, const uint8_t *g, uint16_t fg, uint16_t bg, bool transparentBg = false);

  // blits
  void setSwapBytes(bool s) { _swap = s; }
  void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data);
  void setBypassCanvas(bool) {}     // device fast-path flag; desktop always presents the framebuffer
  void pushPanelBand(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data);
  void fillPanelBlack() {}

  // scanline window (Apple II raster path)
  void setAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h);
  void startWrite() {}
  void writeColor(uint16_t color, uint32_t len);
  void endWrite() {}

  // text (all sizes rendered with the bundled FreeSans9pt7b GFX font)
  void    setTextDatum(uint8_t d) { _datum = d; }
  void    setTextColor(uint16_t fg) { _textFg = fg; _textHasBg = false; }
  void    setTextColor(uint16_t fg, uint16_t bg) { _textFg = fg; _textBg = bg; _textHasBg = true; }
  int16_t drawString(const char *s, int32_t x, int32_t y, uint8_t font);
  int16_t drawString(const char *s, int32_t x, int32_t y) { return drawString(s, x, y, 1); }
  int16_t drawString(const String &s, int32_t x, int32_t y) { return drawString(s.c_str(), x, y, 1); }
  int16_t drawString(const String &s, int32_t x, int32_t y, uint8_t font) { return drawString(s.c_str(), x, y, font); }

  static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }

  void flush();                     // present the framebuffer to the SDL window (+ pump input events)

  // touch == mouse on desktop. input_sdl.cpp feeds window-space mouse via setMouseState().
  uint16_t getTouchRawZ();
  void     getTouchRaw(uint16_t *x, uint16_t *y);
  void     setMouseState(int winX, int winY, bool down);

private:
  void blit(const uint16_t *data, int32_t x, int32_t y, int32_t w, int32_t h);

  uint16_t *_fb = nullptr;          // 320x240 RGB565 framebuffer
  bool      _uiMode = true;
  uint8_t   _datum = TL_DATUM;
  uint16_t  _textFg = TFT_WHITE, _textBg = TFT_BLACK;
  bool      _textHasBg = false;
  bool      _swap = false;
  int32_t   _winX = 0, _winY = 0, _winW = 0, _winH = 0, _curX = 0, _curY = 0;  // scanline cursor
  // mouse (logical 320x240 coords + pressed)
  volatile int  _mx = 0, _my = 0;
  volatile bool _mdown = false;
};

void displayFlush();
// Resize the emulator framebuffer (and the SDL texture if already created). PC-XT / tiny386 call this
// from their setup() — BEFORE begin() — so the authentic PC fonts render at native resolution instead
// of being squished into 320x240. Other platforms keep the default 320x240.
void desktopSetEmuResolution(int w, int h);
void displaySetUiMode(bool ui);
void displaySetVideoRect(int topLogical, int hLogical);
void displaySetVideoFill(int leftLogical, int wLogical, bool stretch);

#endif // BOARD_DESKTOP
