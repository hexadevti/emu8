// display_gfx.h - Arduino_GFX display backend for the JC4827W543 (ESP32-S3, NV3041A QSPI).
//
// DisplayGFX exposes the exact subset of the TFT_eSPI API that this codebase uses, so the
// emulator cores and UI keep calling `tft.<method>()` unchanged. It is backed by an
// Arduino_Canvas framebuffer in PSRAM (480x272) that is flushed to the panel over QSPI.
//
// The emulator keeps drawing in its original 320x240 logical coordinate space; DisplayGFX
// centers that region inside the 480x272 panel via a fixed (OFFSET_X, OFFSET_Y) translation,
// leaving a black border. (Full 480x272 use is a later enhancement.)
//
// This file is compiled/included only on the Arduino_GFX board (see emu.h); on the CYD the
// real TFT_eSPI provides these names and constants instead.
#pragma once

#include "../../board.h"

#if BOARD_DISPLAY_GFX

#include <Arduino.h>

// Backend types live in the Arduino_GFX library; forward-declare them so emu.h (which pulls
// in this header) doesn't have to include the whole library. Real types are used in display_gfx.cpp.
class Arduino_DataBus;
class Arduino_GFX;
class Arduino_Canvas;

// --- RGB565 color constants (TFT_eSPI provides these on the CYD build) ---
#define TFT_BLACK     0x0000
#define TFT_WHITE     0xFFFF
#define TFT_GREEN     0x07E0
#define TFT_PURPLE    0x780F
#define TFT_SKYBLUE   0x867D
#define TFT_YELLOW    0xFFE0
#define TFT_DARKGREY  0x7BEF

// --- text datum constants (subset used by the UI) ---
#define TL_DATUM 0   // Top Left
#define TC_DATUM 1   // Top Centre
#define TR_DATUM 2   // Top Right
#define ML_DATUM 3   // Middle Left
#define MC_DATUM 4   // Middle Centre
#define MR_DATUM 5   // Middle Right
#define BL_DATUM 6   // Bottom Left
#define BC_DATUM 7   // Bottom Centre
#define BR_DATUM 8   // Bottom Right

// The canvas is the full 480x272 panel. Code still draws in the original 320x240 logical
// space; DisplayGFX maps logical->physical two ways depending on the current mode:
//   * UI mode    (menus / keyboard / splash): the 320x240 UI is SCALED to fill 480x272.
//   * VIDEO mode (emulator framebuffer):       the 320x240 video is CENTERED via an offset,
//                                              leaving a black border (kept until upscaling).
#define DISP_LOGICAL_W 320
#define DISP_LOGICAL_H 240
#define DISP_OFFSET_X  ((PANEL_NATIVE_W - DISP_LOGICAL_W) / 2)   // 80
#define DISP_OFFSET_Y  ((PANEL_NATIVE_H - DISP_LOGICAL_H) / 2)   // 16

class DisplayGFX {
public:
  // Drawing mode: true = UI (scale 320x240 -> 480x272), false = video (center 320x240).
  // Set by the render loop before each UI vs emulator-video draw section.
  void setUiMode(bool ui) { _uiMode = ui; }

  // --- lifecycle ---
  void begin();
  void setRotation(uint8_t r);     // no-op: NV3041A is natively landscape 480x272
  void invertDisplay(bool i);
  void initDMA();                  // no-op: the canvas flush handles transfer

  // --- fills / shapes ---
  void fillScreen(uint16_t color);
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
  void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color);
  void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color);

  // --- blits ---
  void setSwapBytes(bool swap);
  void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data);
  // Direct 1:1 RGB565 blit into the canvas at PANEL-native (x,y) — NO logical 320x240 scaling and NO
  // centering offset (unlike pushImage). For cores that compose a full-panel image themselves and
  // need exact pixel placement (the tiny386 PC renderer). UI-mode flush then pushes the canvas 1:1.
  void drawCanvasRGB565(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data);

  // --- direct-to-panel fast path (NES video): push a band straight to the panel at the centered
  //     video offset, skipping the PSRAM canvas + the full-panel QSPI flush. setBypassCanvas(true)
  //     makes the next flush() a no-op (the frame is already on the panel). fillPanelBlack clears
  //     the static border once. Used only when not in fill-screen mode. ---
  void setBypassCanvas(bool b) { _bypassCanvas = b; }
  void pushPanelBand(int32_t logicalX, int32_t logicalY, int32_t w, int32_t h, const uint16_t *data);
  // Like pushPanelBand but at RAW panel coords (no DISP_OFFSET centering). The tiny386 PC renderer
  // pushes its full-panel image straight to the panel (bypassing the PSRAM canvas + its flush) to
  // halve the per-frame PSRAM traffic -- the canvas write + flush read were the FPS bottleneck.
  void drawPanelRGB565(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data);
  // Composite + push ONLY the on-screen-keyboard's panel band in one DSI transfer, so a key press
  // doesn't re-flush the whole 1024x600 frame (the tiny386 OSK was laggy from the full composite).
  // The caller setBypassCanvas(true) to skip the loop-top full flush. P4 (DSI) only; no-op elsewhere.
  void flushOskBand();
  void fillPanelBlack();

  // --- scanline window writes (Apple II raster path) ---
  void setAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h);
  void startWrite();
  void writeColor(uint16_t color, uint32_t len);
  void endWrite();

  // --- text ---
  void setTextDatum(uint8_t d);
  void setTextColor(uint16_t fg);
  void setTextColor(uint16_t fg, uint16_t bg);
  int16_t drawString(const char *s, int32_t x, int32_t y);
  int16_t drawString(const String &s, int32_t x, int32_t y);
  int16_t drawString(const char *s, int32_t x, int32_t y, uint8_t font);
  int16_t drawString(const String &s, int32_t x, int32_t y, uint8_t font);

  // --- color helper (pure math; usable at static-init time) ---
  static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }

  // --- per-frame flush of the PSRAM canvas to the panel over QSPI ---
  void flush();

  // --- touch (XPT2046 lands in M4; stubs for now) ---
  uint16_t getTouchRawZ();
  void getTouchRaw(uint16_t *x, uint16_t *y);

#if BOARD_PANEL_DSI
  // On-screen-keyboard overlay (P4): the keyboard is drawn into a SEPARATE transparent buffer so it
  // never overwrites the emulator video in the main canvas; flushDSI alpha-blends it on top. Drawing
  // between oskOverlayBegin()/End() is redirected to that buffer (see fillRect/drawString/...).
  void oskOverlayBegin();           // clear the overlay to "transparent" and redirect drawing to it
  void oskOverlayEnd();             // stop redirecting
  bool inOskOverlay() const { return _toOsk; }
  // Fill a TRUE circle straight into the overlay buffer in PANEL pixels (the on-screen gamepad's
  // analog stick — drawing it via the UI-scaled path would make it oval, since X/Y scale differ).
  void oskOverlayFillCircle(int cx, int cy, int r, uint16_t color);
  // Blit an 8x8 font glyph scaled (nearest-neighbour) into the canvas at PANEL rect (px,py,pw,ph) with
  // fg for set bits / bg for clear bits (transparentBg leaves clear pixels untouched). Used by the
  // PC-XT text renderer to draw the original IBM CP437 8x8 font crisply on the big panel.
  void drawGlyph8(int px, int py, int pw, int ph, const uint8_t *g, uint16_t fg, uint16_t bg, bool transparentBg = false);
#endif

private:
  // logical(320x240) -> physical(panel): UI mode scales, video mode centers via offset.
  void mapPt(int32_t x, int32_t y, int32_t &px, int32_t &py) const;
  void mapSz(int32_t w, int32_t h, int32_t &pw, int32_t &ph) const;
  // current shape/text draw target: the keyboard overlay buffer (P4, while redirecting) or the canvas.
  Arduino_Canvas *dtgt() {
#if BOARD_PANEL_DSI
    return (_toOsk && _oskCanvas) ? _oskCanvas : _canvas;
#else
    return _canvas;
#endif
  }
#if BOARD_PANEL_DSI
  void flushDSI();                  // ESP32-P4: compose + push the canvas to the JD9165 panel via esp_lcd
  void fillVideoFrame(uint16_t *out); // fill-scale the canvas video into a full-panel frame
#endif

  bool     _uiMode   = true;        // default UI (splash draws first); render loop toggles it
  uint8_t  _datum    = TL_DATUM;
  uint16_t _textFg   = TFT_WHITE;
  uint16_t _textBg   = TFT_BLACK;
  bool     _textHasBg = false;      // TFT_eSPI: setTextColor(fg) is transparent; (fg,bg) is opaque
  bool     _swap     = false;
  bool     _bypassCanvas = false;   // NES direct-to-panel: skip the next canvas flush (already drawn)
  // scanline cursor for setAddrWindow/writeColor (Apple II raster path) — physical canvas coords
  int32_t  _winX = 0, _winY = 0, _winW = 0, _winH = 0;
  int32_t  _curX = 0, _curY = 0;
  // Arduino_GFX backend (allocated in begin(); real types resolved in display_gfx.cpp)
  Arduino_DataBus *_bus    = nullptr;
  Arduino_GFX     *_panel  = nullptr;
  Arduino_Canvas  *_canvas = nullptr;
  uint16_t        *_fb     = nullptr;   // canvas framebuffer (PSRAM), 480x272 row-major
#if BOARD_PANEL_DSI
  Arduino_Canvas  *_oskCanvas = nullptr;  // separate panel-size canvas for the keyboard overlay
  uint16_t        *_oskFb     = nullptr;  // its framebuffer (blended over the video in flushDSI)
  bool             _toOsk     = false;    // true while drawing should go to the overlay buffer
#endif
};

// Flush the PSRAM canvas to the panel over QSPI. Called once per rendered frame (and after
// UI redraws). On the CYD build (TFT_eSPI draws straight to the panel) this is a no-op; see
// the inline definition in emu.h-side code / video.cpp guards.
void displayFlush();

#endif // BOARD_DISPLAY_GFX
