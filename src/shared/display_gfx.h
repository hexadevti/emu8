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

  // --- direct-to-panel fast path (NES video): push a band straight to the panel at the centered
  //     video offset, skipping the PSRAM canvas + the full-panel QSPI flush. setBypassCanvas(true)
  //     makes the next flush() a no-op (the frame is already on the panel). fillPanelBlack clears
  //     the static border once. Used only when not in fill-screen mode. ---
  void setBypassCanvas(bool b) { _bypassCanvas = b; }
  void pushPanelBand(int32_t logicalX, int32_t logicalY, int32_t w, int32_t h, const uint16_t *data);
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

private:
  // logical(320x240) -> physical(480x272): UI mode scales, video mode centers via offset.
  void mapPt(int32_t x, int32_t y, int32_t &px, int32_t &py) const;
  void mapSz(int32_t w, int32_t h, int32_t &pw, int32_t &ph) const;

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
};

// Flush the PSRAM canvas to the panel over QSPI. Called once per rendered frame (and after
// UI redraws). On the CYD build (TFT_eSPI draws straight to the panel) this is a no-op; see
// the inline definition in emu.h-side code / video.cpp guards.
void displayFlush();

#endif // BOARD_DISPLAY_GFX
