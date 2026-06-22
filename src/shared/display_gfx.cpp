// display_gfx.cpp - Arduino_GFX display backend (JC4827W543 / ESP32-S3, NV3041A QSPI).
//
// DisplayGFX presents the TFT_eSPI method subset the emulator uses, backed by an Arduino_Canvas
// framebuffer in PSRAM that covers the full 480x272 panel and is flushed over QSPI each frame.
//
// All emulator/UI code still draws in the original 320x240 logical space. DisplayGFX maps that
// onto the 480x272 panel per the current mode (setUiMode):
//   * UI mode    -> the 320x240 UI (menus / keyboard / splash) is SCALED to fill 480x272.
//   * VIDEO mode -> the 320x240 emulator framebuffer is CENTERED via (DISP_OFFSET_X/Y), with a
//                   black border (kept until the emulator is upscaled later).

#include "display_gfx.h"

#if BOARD_DISPLAY_GFX

#include <Arduino_GFX_Library.h>
#include "fonts/FreeSans9pt7b.h"    // crisp proportional UI font for titles / buttons (TFT font >= 2)

extern bool screenFill;   // user option: scale the emulator video to fill the panel (keep aspect)

// Fill-screen: the render reports the ACTIVE video content rect (top + height, in the logical
// 320x240 space) before drawing, so fill-screen can scale just the content to the panel's full
// height — dropping the cores' black top/bottom margins (Apple/Atari center 192 lines in 240; the
// C64 screen is 200) — instead of the whole 320x240 (which would scale those margins up too).
static int gVideoTop = 0, gVideoHeight = DISP_LOGICAL_H;
void displaySetVideoRect(int topLogical, int hLogical) {
  gVideoTop = topLogical; gVideoHeight = (hLogical > 0) ? hLogical : DISP_LOGICAL_H;
}

// Arduino_Canvas allocates its framebuffer with aligned_alloc() = internal DRAM. A 480x272x2
// (~255 KB) buffer must NOT come from internal DRAM, so this subclass forces it into PSRAM.
class PsramCanvas : public Arduino_Canvas {
public:
  PsramCanvas(int16_t w, int16_t h, Arduino_G *output) : Arduino_Canvas(w, h, output, 0, 0) {}
  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    if (speed != GFX_SKIP_OUTPUT_BEGIN && _output) {
      if (!_output->begin(speed)) return false;
    }
    if (!_framebuffer) {
      size_t s = (size_t)_width * _height * 2;
      _framebuffer = (uint16_t *)ps_malloc(s);   // PSRAM, not internal DRAM
      if (!_framebuffer) return false;
      memset(_framebuffer, 0, s);
    }
    return true;
  }
};

static inline Arduino_GFX *gfx(Arduino_Canvas *c) { return static_cast<Arduino_GFX *>(c); }

// logical(320x240) -> physical(480x272) point/size mapping per mode.
void DisplayGFX::mapPt(int32_t x, int32_t y, int32_t &px, int32_t &py) const {
  if (_uiMode) {
    px = x * PANEL_NATIVE_W / DISP_LOGICAL_W;
    py = y * PANEL_NATIVE_H / DISP_LOGICAL_H;
  } else {
    px = x + DISP_OFFSET_X;
    py = y + DISP_OFFSET_Y;
  }
}
void DisplayGFX::mapSz(int32_t w, int32_t h, int32_t &pw, int32_t &ph) const {
  if (_uiMode) {
    pw = w * PANEL_NATIVE_W / DISP_LOGICAL_W;
    ph = h * PANEL_NATIVE_H / DISP_LOGICAL_H;
  } else { pw = w; ph = h; }
}

void DisplayGFX::begin() {
  _bus = new Arduino_ESP32QSPI(GFX_QSPI_CS_PIN, GFX_QSPI_SCK_PIN,
                               GFX_QSPI_D0_PIN, GFX_QSPI_D1_PIN,
                               GFX_QSPI_D2_PIN, GFX_QSPI_D3_PIN);
  _panel = new Arduino_NV3041A(_bus, GFX_RST_PIN, 0 /*rotation: native landscape*/, true /*IPS*/);
  _canvas = new PsramCanvas(PANEL_NATIVE_W, PANEL_NATIVE_H, (Arduino_G *)_panel);
  if (!_canvas->begin()) {
    Serial.println("DisplayGFX: canvas/panel init FAILED");
    return;
  }
  _fb = _canvas->getFramebuffer();
  gfx(_canvas)->fillScreen(TFT_BLACK);

  if (GFX_BL_PIN >= 0) {             // backlight on (active HIGH)
    pinMode(GFX_BL_PIN, OUTPUT);
    digitalWrite(GFX_BL_PIN, HIGH);
  }
}

void DisplayGFX::setRotation(uint8_t) {}      // panel is natively landscape 480x272
void DisplayGFX::invertDisplay(bool)  {}      // NV3041A IPS shows correct colors w/o inversion
void DisplayGFX::initDMA()            {}       // the canvas flush handles the transfer

void DisplayGFX::fillScreen(uint16_t color) {  // always clears the whole panel
  if (_canvas) gfx(_canvas)->fillScreen(color);
}
void DisplayGFX::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  if (!_canvas) return;
  int32_t px, py, pw, ph; mapPt(x, y, px, py); mapSz(w, h, pw, ph);
  gfx(_canvas)->fillRect(px, py, pw, ph, color);
}
void DisplayGFX::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  if (!_canvas) return;
  int32_t px, py, pw, ph; mapPt(x, y, px, py); mapSz(w, h, pw, ph);
  int32_t pr = _uiMode ? r * PANEL_NATIVE_H / DISP_LOGICAL_H : r;
  gfx(_canvas)->fillRoundRect(px, py, pw, ph, pr, color);
}
void DisplayGFX::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  if (!_canvas) return;
  int32_t px, py, pw, ph; mapPt(x, y, px, py); mapSz(w, h, pw, ph);
  int32_t pr = _uiMode ? r * PANEL_NATIVE_H / DISP_LOGICAL_H : r;
  gfx(_canvas)->drawRoundRect(px, py, pw, ph, pr, color);
}

void DisplayGFX::setSwapBytes(bool swap) { _swap = swap; }

void DisplayGFX::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) {
  if (!_canvas || !data || !_fb) return;
  int32_t px, py, pw, ph; mapPt(x, y, px, py); mapSz(w, h, pw, ph);
  if (!_uiMode) {
    // video: 1:1 blit at the centered offset.
    gfx(_canvas)->draw16bitRGBBitmap(px, py, (uint16_t *)data, w, h);
    return;
  }
  // UI: nearest-neighbor scale the image to (pw x ph) so it fills proportionally (e.g. the
  // splash logo stays centered because both position and size scale by the same factor).
  for (int32_t dy = 0; dy < ph; dy++) {
    int32_t sy = dy * h / ph, cy = py + dy;
    if (cy < 0 || cy >= PANEL_NATIVE_H) continue;
    const uint16_t *srow = data + sy * w;
    uint16_t *drow = _fb + cy * PANEL_NATIVE_W;
    for (int32_t dx = 0; dx < pw; dx++) {
      int32_t cx = px + dx;
      if (cx < 0 || cx >= PANEL_NATIVE_W) continue;
      drow[cx] = srow[dx * w / pw];
    }
  }
}

// --- NES direct-to-panel fast path: push a converted band straight to the panel at the centered
//     video offset, bypassing the PSRAM canvas write AND the full 480x272 QSPI flush. This removes
//     the core-0 PSRAM round-trip that contends with the core-1 interpreter on the shared S3 MSPI bus.
void DisplayGFX::pushPanelBand(int32_t logicalX, int32_t logicalY, int32_t w, int32_t h, const uint16_t *data) {
  if (!_panel || !data) return;
  _panel->draw16bitRGBBitmap(logicalX + DISP_OFFSET_X, logicalY + DISP_OFFSET_Y, (uint16_t *)data, w, h);
}
void DisplayGFX::fillPanelBlack() { if (_panel) _panel->fillScreen(TFT_BLACK); }

// --- Apple II scanline path (always VIDEO mode): centered window, direct framebuffer fill. ---
void DisplayGFX::setAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h) {
  int32_t px, py, pw, ph; mapPt(x, y, px, py); mapSz(w, h, pw, ph);
  _winX = px; _winY = py; _winW = pw; _winH = ph; _curX = px; _curY = py;
}
void DisplayGFX::startWrite() {}
void DisplayGFX::endWrite()   {}
void DisplayGFX::writeColor(uint16_t color, uint32_t len) {
  if (!_fb) return;
  const int32_t xEnd = _winX + _winW, yEnd = _winY + _winH;
  while (len--) {
    if (_curX >= 0 && _curX < PANEL_NATIVE_W && _curY >= 0 && _curY < PANEL_NATIVE_H)
      _fb[_curY * PANEL_NATIVE_W + _curX] = color;
    if (++_curX >= xEnd) { _curX = _winX; if (++_curY >= yEnd) _curY = _winY; }
  }
}

// --- text ---
void DisplayGFX::setTextDatum(uint8_t d) { _datum = d; }
void DisplayGFX::setTextColor(uint16_t fg) { _textFg = fg; _textHasBg = false; }
void DisplayGFX::setTextColor(uint16_t fg, uint16_t bg) { _textFg = fg; _textBg = bg; _textHasBg = true; }

int16_t DisplayGFX::drawString(const char *s, int32_t x, int32_t y, uint8_t font) {
  if (!_canvas || !s) return 0;
  Arduino_GFX *g = gfx(_canvas);
  // Titles/buttons (TFT font >= 2) use a crisp proportional GFX font at native size. Body text
  // (font 1: labels, file-browser rows) uses the compact built-in 6x8 font so it stays legible in
  // the dense rows (a 22px FreeSans glyph overflows the ~16px rows). Both are crisp (no scaling).
  g->setFont((_uiMode && font >= 2) ? &FreeSans9pt7b : nullptr);
  g->setTextSize(1);
  g->setTextWrap(false);
  int16_t bx, by; uint16_t bw, bh;
  g->getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  int32_t px, py; mapPt(x, y, px, py);
  const uint8_t horiz = _datum % 3;   // 0 left, 1 centre, 2 right
  const uint8_t vert  = _datum / 3;   // 0 top,  1 middle, 2 bottom
  int32_t cx = px, cy = py;
  if (horiz == 1) cx = px - bw / 2; else if (horiz == 2) cx = px - bw;
  if (vert  == 1) cy = py - bh / 2; else if (vert  == 2) cy = py - bh;
  if (_textHasBg) g->setTextColor(_textFg, _textBg); else g->setTextColor(_textFg);
  g->setCursor(cx - bx, cy - by);
  g->print(s);
  g->setFont(nullptr);                // restore default font for any subsequent text
  return (int16_t)bw;
}
int16_t DisplayGFX::drawString(const char *s, int32_t x, int32_t y)            { return drawString(s, x, y, 1); }
int16_t DisplayGFX::drawString(const String &s, int32_t x, int32_t y)         { return drawString(s.c_str(), x, y, 1); }
int16_t DisplayGFX::drawString(const String &s, int32_t x, int32_t y, uint8_t font) { return drawString(s.c_str(), x, y, font); }

void DisplayGFX::flush() {
  if (!_canvas) return;
  // NES direct-to-panel frame: the band push already put this frame on the panel; nothing to flush.
  if (_bypassCanvas) return;

  // Normal path: push the 480x272 canvas 1:1. Used for all UI (menus / keyboard / splash) and for
  // the emulator video when fill-screen is off. _uiMode is false only on a pure emulator-video frame
  // (the render loop leaves it false after drawing the centered video, before this top-of-loop flush).
  if (!screenFill || _uiMode) { _canvas->flush(true); return; }

  // Fill-screen path: scale just the active content rect [gVideoTop, +gVideoHeight] of the canvas
  // (the cores draw centered at DISP_OFFSET_X/Y) up to the panel's full HEIGHT, keeping aspect (the
  // same scale horizontally -> scaledW), centered with black side bars. This drops the cores' black
  // top/bottom margins so the picture truly fills the screen vertically.
  int vTop = gVideoTop, vH = gVideoHeight;
  if (vH <= 0 || vTop < 0 || vTop + vH > DISP_LOGICAL_H) { vTop = 0; vH = DISP_LOGICAL_H; }   // sanity
  int scaledW = (int)((long)DISP_LOGICAL_W * PANEL_NATIVE_H / vH);   // horizontal scale == vertical
  if (scaledW > PANEL_NATIVE_W) scaledW = PANEL_NATIVE_W;            // never overflow the panel width
  int outX = (PANEL_NATIVE_W - scaledW) / 2;

  static uint16_t *fillBuf = nullptr;
  if (!fillBuf) {
    fillBuf = (uint16_t *)ps_malloc((size_t)PANEL_NATIVE_W * PANEL_NATIVE_H * 2);   // max size (480x272)
    if (!fillBuf) { _canvas->flush(true); return; }   // OOM -> fall back to the 1:1 flush
  }
  // Nearest-neighbor source maps (recomputed each frame; the rect changes with mode/platform).
  static int16_t colMap[PANEL_NATIVE_W], rowMap[PANEL_NATIVE_H];
  for (int oy = 0; oy < PANEL_NATIVE_H; oy++)
    rowMap[oy] = (int16_t)(DISP_OFFSET_Y + vTop + (int)((long)oy * vH / PANEL_NATIVE_H));
  for (int ox = 0; ox < scaledW; ox++)
    colMap[ox] = (int16_t)(DISP_OFFSET_X + (int)((long)ox * DISP_LOGICAL_W / scaledW));

  for (int oy = 0; oy < PANEL_NATIVE_H; oy++) {
    const uint16_t *srow = _fb + (size_t)rowMap[oy] * PANEL_NATIVE_W;
    uint16_t *drow = fillBuf + (size_t)oy * scaledW;
    for (int ox = 0; ox < scaledW; ox++) drow[ox] = srow[colMap[ox]];
  }
  _panel->draw16bitRGBBitmap(outX, 0, fillBuf, scaledW, PANEL_NATIVE_H);
  if (outX > 0) {                                     // letterbox the sides (video fills the height)
    _panel->fillRect(0, 0, outX, PANEL_NATIVE_H, TFT_BLACK);
    _panel->fillRect(outX + scaledW, 0, PANEL_NATIVE_W - outX - scaledW, PANEL_NATIVE_H, TFT_BLACK);
  }
}

// Touch (XPT2046 on its own SPI) is read directly in touchkeyboard.cpp; these stay stubs.
uint16_t DisplayGFX::getTouchRawZ() { return 0; }
void DisplayGFX::getTouchRaw(uint16_t *x, uint16_t *y) { if (x) *x = 0; if (y) *y = 0; }

extern DisplayGFX tft;
void displayFlush() { tft.flush(); }
void displaySetUiMode(bool ui) { tft.setUiMode(ui); }

#endif // BOARD_DISPLAY_GFX
