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

#if BOARD_PANEL_DSI
#include "p4/jd9165_dsi.h"          // ESP32-P4: flush the canvas to a JD9165 MIPI-DSI panel (esp_lcd)
#endif

extern bool screenFill;   // user option: scale the emulator video to fill the panel (keep aspect)

// Fill-screen: the render reports the ACTIVE video content rect (top + height, in the logical
// 320x240 space) before drawing, so fill-screen can scale just the content to the panel's full
// height — dropping the cores' black top/bottom margins (Apple/Atari center 192 lines in 240; the
// C64 screen is 200) — instead of the whole 320x240 (which would scale those margins up too).
// gVideoLeft/Width are the matching horizontal rect, and gVideoStretch asks the fill flush to
// stretch that rect across the FULL panel width (ignoring 4:3 aspect) instead of letterboxing —
// every core sets it so its picture fills the whole screen in FILL mode. displaySetVideoRect()
// resets the horizontal rect + stretch to defaults each frame; displaySetVideoFill() overrides after.
static int gVideoTop = 0, gVideoHeight = DISP_LOGICAL_H;
static int gVideoLeft = 0, gVideoWidth = DISP_LOGICAL_W;
static bool gVideoStretch = false;
// Set when a core declares a video content rect this frame (the small/centered emulator video that the
// P4 flush should FILL-scale). Cores that draw the whole canvas themselves in UI mode (PC-XT/IIGS text)
// never call displaySetVideoRect, so this stays false and flushDSI pushes their canvas 1:1.
static bool gFrameUsesFill = false;
void displaySetVideoRect(int topLogical, int hLogical) {
  gVideoTop = topLogical; gVideoHeight = (hLogical > 0) ? hLogical : DISP_LOGICAL_H;
  gVideoLeft = 0; gVideoWidth = DISP_LOGICAL_W; gVideoStretch = false;   // defaults; fill overrides
  gFrameUsesFill = true;
}
void displaySetVideoFill(int leftLogical, int wLogical, bool stretch) {
  gVideoLeft = leftLogical; gVideoWidth = (wLogical > 0) ? wLogical : DISP_LOGICAL_W; gVideoStretch = stretch;
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
#if BOARD_PANEL_DSI
  // ESP32-P4 (JD9165 MIPI-DSI): the in-memory canvas has NO Arduino_GFX output; we flush its PSRAM
  // framebuffer to the panel via esp_lcd (see p4/jd9165_dsi.cpp). _bus/_panel stay null; all the
  // canvas drawing primitives (shapes/text/blits) work unchanged because they only touch _fb.
  if (!dsiPanelBegin()) Serial.println("DisplayGFX: DSI panel init FAILED (check the JD9165 vendor driver)");
  _bus = nullptr;
  _panel = nullptr;
  _canvas = new PsramCanvas(PANEL_NATIVE_W, PANEL_NATIVE_H, nullptr);
  if (!_canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {   // allocate the PSRAM framebuffer only (no output)
    Serial.println("DisplayGFX: canvas alloc FAILED");
    return;
  }
  _fb = _canvas->getFramebuffer();
  gfx(_canvas)->fillScreen(TFT_BLACK);
  dsiPanelFillScreen(TFT_BLACK);    // clear the panel so nothing flashes before the first flush
  return;
#else
  _bus = new Arduino_ESP32QSPI(GFX_QSPI_CS_PIN, GFX_QSPI_SCK_PIN,
                               GFX_QSPI_D0_PIN, GFX_QSPI_D1_PIN,
                               GFX_QSPI_D2_PIN, GFX_QSPI_D3_PIN);
  _panel = new Arduino_NV3041A(_bus, GFX_RST_PIN, 2 /*rotation: landscape, flipped 180 deg*/, true /*IPS*/);
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
#endif
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
  gfx(dtgt())->fillRect(px, py, pw, ph, color);
}
void DisplayGFX::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  if (!_canvas) return;
  int32_t px, py, pw, ph; mapPt(x, y, px, py); mapSz(w, h, pw, ph);
  int32_t pr = _uiMode ? r * PANEL_NATIVE_H / DISP_LOGICAL_H : r;
  gfx(dtgt())->fillRoundRect(px, py, pw, ph, pr, color);
}
void DisplayGFX::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  if (!_canvas) return;
  int32_t px, py, pw, ph; mapPt(x, y, px, py); mapSz(w, h, pw, ph);
  int32_t pr = _uiMode ? r * PANEL_NATIVE_H / DISP_LOGICAL_H : r;
  gfx(dtgt())->drawRoundRect(px, py, pw, ph, pr, color);
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

// Direct 1:1 blit of an RGB565 image into the canvas at PANEL-native (x,y) — no logical 320x240
// scaling, no centering offset (unlike pushImage). The tiny386 PC renderer composes its own
// full-panel image and needs exact placement; clipped to the canvas, the UI-mode flush pushes it 1:1.
void DisplayGFX::drawCanvasRGB565(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) {
  if (!_fb || !data) return;
  for (int32_t r = 0; r < h; r++) {
    int32_t cy = y + r;
    if (cy < 0 || cy >= PANEL_NATIVE_H) continue;
    const uint16_t *srow = data + (size_t)r * w;
    uint16_t *drow = _fb + (size_t)cy * PANEL_NATIVE_W;
    for (int32_t c = 0; c < w; c++) {
      int32_t cx = x + c;
      if (cx >= 0 && cx < PANEL_NATIVE_W) drow[cx] = srow[c];
    }
  }
}

// --- NES direct-to-panel fast path: push a converted band straight to the panel at the centered
//     video offset, bypassing the PSRAM canvas write AND the full 480x272 QSPI flush. This removes
//     the core-0 PSRAM round-trip that contends with the core-1 interpreter on the shared S3 MSPI bus.
void DisplayGFX::pushPanelBand(int32_t logicalX, int32_t logicalY, int32_t w, int32_t h, const uint16_t *data) {
  if (!data) return;
#if BOARD_PANEL_DSI
  dsiPanelDrawBitmap(logicalX + DISP_OFFSET_X, logicalY + DISP_OFFSET_Y, w, h, data);
#else
  if (!_panel) return;
  _panel->draw16bitRGBBitmap(logicalX + DISP_OFFSET_X, logicalY + DISP_OFFSET_Y, (uint16_t *)data, w, h);
#endif
}
// Raw 1:1 blit straight to the panel at panel coords (no DISP_OFFSET). Used by the tiny386 renderer
// to bypass the PSRAM canvas + its full flush (it composes the whole panel itself).
void DisplayGFX::drawPanelRGB565(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) {
  if (!data) return;
#if BOARD_PANEL_DSI
  dsiPanelDrawBitmap(x, y, w, h, data);
#else
  if (!_panel) return;
  _panel->draw16bitRGBBitmap(x, y, (uint16_t *)data, w, h);
#endif
}
void DisplayGFX::fillPanelBlack() {
#if BOARD_PANEL_DSI
  dsiPanelFillScreen(TFT_BLACK);
#else
  if (_panel) _panel->fillScreen(TFT_BLACK);
#endif
}

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
  Arduino_GFX *g = gfx(dtgt());
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

#if BOARD_PANEL_DSI
  flushDSI();    // ESP32-P4: push the canvas to the JD9165 panel via esp_lcd (own scale path below)
  return;
#endif

  // Normal path: push the 480x272 canvas 1:1. Used for all UI (menus / keyboard / splash) and for
  // the emulator video when fill-screen is off. _uiMode is false only on a pure emulator-video frame
  // (the render loop leaves it false after drawing the centered video, before this top-of-loop flush).
  if (!screenFill || _uiMode) { _canvas->flush(true); return; }

  // Fill-screen path: scale just the active content rect [gVideoLeft/Top, +gVideoWidth/Height] of
  // the canvas (the cores draw centered at DISP_OFFSET_X/Y) up to the panel's full HEIGHT. By
  // default it keeps aspect (same scale horizontally -> scaledW) and centers with black side bars,
  // dropping the cores' black top/bottom margins so the picture fills the screen vertically. When
  // gVideoStretch is set (all cores do) it stretches the rect across the FULL width — true 100%.
  int vTop = gVideoTop, vH = gVideoHeight;
  if (vH <= 0 || vTop < 0 || vTop + vH > DISP_LOGICAL_H) { vTop = 0; vH = DISP_LOGICAL_H; }   // sanity
  int vLeft = gVideoLeft, vW = gVideoWidth;
  if (vW <= 0 || vLeft < 0 || vLeft + vW > DISP_LOGICAL_W) { vLeft = 0; vW = DISP_LOGICAL_W; }  // sanity

  int scaledW, outX;
  if (gVideoStretch) {                  // stretch the content rect over the whole width
    scaledW = PANEL_NATIVE_W; outX = 0; //   (no 4:3 aspect, no side bars)
  } else {                              // keep aspect: horizontal scale == vertical, letterbox sides
    scaledW = (int)((long)vW * PANEL_NATIVE_H / vH);
    if (scaledW > PANEL_NATIVE_W) scaledW = PANEL_NATIVE_W;          // never overflow the panel width
    outX = (PANEL_NATIVE_W - scaledW) / 2;
  }

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
    colMap[ox] = (int16_t)(DISP_OFFSET_X + vLeft + (int)((long)ox * vW / scaledW));

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

#if BOARD_PANEL_DSI
extern bool oskActive();              // on-screen keyboard state (touchkeyboard.cpp)
extern bool osgActive();              // on-screen virtual gamepad state (osg.cpp); both use the overlay
#define OSK_OVERLAY_TRANSPARENT 0xF81F  // sentinel "see-through" pixel in the keyboard overlay buffer

// Redirect keyboard drawing into a separate, panel-size overlay buffer (lazily allocated) cleared to
// the transparent sentinel, so the keys never touch the emulator video in the main canvas.
void DisplayGFX::oskOverlayBegin() {
  if (!_oskCanvas) {
    _oskCanvas = new PsramCanvas(PANEL_NATIVE_W, PANEL_NATIVE_H, nullptr);
    if (!_oskCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) { delete _oskCanvas; _oskCanvas = nullptr; return; }
    _oskFb = _oskCanvas->getFramebuffer();
  }
  if (_oskFb) {
    size_t n = (size_t)PANEL_NATIVE_W * PANEL_NATIVE_H;
    for (size_t i = 0; i < n; i++) _oskFb[i] = OSK_OVERLAY_TRANSPARENT;
  }
  _toOsk = true;
}
void DisplayGFX::oskOverlayEnd() { _toOsk = false; }

void DisplayGFX::drawGlyph8(int px, int py, int pw, int ph, const uint8_t *g, uint16_t fg, uint16_t bg, bool transparentBg) {
  if (!_fb || !g || pw <= 0 || ph <= 0) return;
  for (int oy = 0; oy < ph; oy++) {
    int yy = py + oy;
    if (yy < 0 || yy >= PANEL_NATIVE_H) continue;
    uint8_t bits = g[(oy * 8) / ph];                 // nearest-neighbour source row
    uint16_t *row = _fb + (size_t)yy * PANEL_NATIVE_W;
    for (int ox = 0; ox < pw; ox++) {
      int xx = px + ox;
      if (xx < 0 || xx >= PANEL_NATIVE_W) continue;
      bool on = bits & (0x80 >> ((ox * 8) / pw));
      if (on) row[xx] = fg;
      else if (!transparentBg) row[xx] = bg;
    }
  }
}

void DisplayGFX::oskOverlayFillCircle(int cx, int cy, int r, uint16_t color) {
  if (!_oskFb || r <= 0) return;
  for (int dy = -r; dy <= r; dy++) {
    int y = cy + dy;
    if (y < 0 || y >= PANEL_NATIVE_H) continue;
    int hw = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
    int x0 = cx - hw, x1 = cx + hw;
    if (x0 < 0) x0 = 0;
    if (x1 >= PANEL_NATIVE_W) x1 = PANEL_NATIVE_W - 1;
    uint16_t *row = _oskFb + (size_t)y * PANEL_NATIVE_W;
    for (int x = x0; x <= x1; x++) row[x] = color;
  }
}

// Fill-scale the canvas video into a full-panel `out` frame (stretch, or 4:3-letterbox the active
// content rect). Same math the QSPI fill path uses; factored out so the OSK path can reuse it.
void DisplayGFX::fillVideoFrame(uint16_t *out) {
  int vTop = gVideoTop, vH = gVideoHeight;
  if (vH <= 0 || vTop < 0 || vTop + vH > DISP_LOGICAL_H) { vTop = 0; vH = DISP_LOGICAL_H; }
  int vLeft = gVideoLeft, vW = gVideoWidth;
  if (vW <= 0 || vLeft < 0 || vLeft + vW > DISP_LOGICAL_W) { vLeft = 0; vW = DISP_LOGICAL_W; }

  int scaledW, outX;
  if (gVideoStretch) { scaledW = PANEL_NATIVE_W; outX = 0; }    // stretch over the whole width
  else {                                                        // keep 4:3 aspect, letterbox sides
    scaledW = (int)((long)vW * PANEL_NATIVE_H / vH);
    if (scaledW > PANEL_NATIVE_W) scaledW = PANEL_NATIVE_W;
    outX = (PANEL_NATIVE_W - scaledW) / 2;
  }
  static int16_t colMap[PANEL_NATIVE_W], rowMap[PANEL_NATIVE_H];
  for (int oy = 0; oy < PANEL_NATIVE_H; oy++)
    rowMap[oy] = (int16_t)(DISP_OFFSET_Y + vTop + (int)((long)oy * vH / PANEL_NATIVE_H));
  for (int ox = 0; ox < scaledW; ox++)
    colMap[ox] = (int16_t)(DISP_OFFSET_X + vLeft + (int)((long)ox * vW / scaledW));

  for (int oy = 0; oy < PANEL_NATIVE_H; oy++) {
    const uint16_t *srow = _fb + (size_t)rowMap[oy] * PANEL_NATIVE_W;
    uint16_t *drow = out + (size_t)oy * PANEL_NATIVE_W;
    for (int ox = 0; ox < outX; ox++) drow[ox] = TFT_BLACK;                         // left bar
    for (int ox = 0; ox < scaledW; ox++) drow[outX + ox] = srow[colMap[ox]];        // scaled video
    for (int ox = outX + scaledW; ox < PANEL_NATIVE_W; ox++) drow[ox] = TFT_BLACK;  // right bar
  }
}

// ESP32-P4 flush. The emulator picture is built exactly as it would be WITHOUT the keyboard (1:1 for
// UI / non-fill, else fill-scaled); when the keyboard is open its separate overlay buffer is then
// alpha-blended on top (50/50, except the transparent sentinel pixels), so the game shows through it
// and the picture's format never changes when the keyboard opens.
void DisplayGFX::flushDSI() {
  if (!_fb) return;
  bool osk = (oskActive() || osgActive()) && _oskFb;
  // Fill-scale only when a core declared a video rect this frame (the small/centered emulator video).
  // PC-XT/IIGS draw the whole canvas themselves -> push 1:1. (_uiMode is unreliable: the keyboard
  // overlay leaves it true regardless of the underlying video.)
  bool fill = screenFill && gFrameUsesFill;
  gFrameUsesFill = false;

  static uint16_t *frame = nullptr;
  if (!frame) frame = (uint16_t *)ps_malloc((size_t)PANEL_NATIVE_W * PANEL_NATIVE_H * 2);

  if (!osk) {                                  // no keyboard: original behavior
    if (!fill || !frame) { dsiPanelDrawBitmap(0, 0, PANEL_NATIVE_W, PANEL_NATIVE_H, _fb); return; }
    fillVideoFrame(frame);
    dsiPanelDrawBitmap(0, 0, PANEL_NATIVE_W, PANEL_NATIVE_H, frame);
    return;
  }

  if (!frame) { dsiPanelDrawBitmap(0, 0, PANEL_NATIVE_W, PANEL_NATIVE_H, _fb); return; }
  if (fill) fillVideoFrame(frame);                                                         // filled video
  else      memcpy(frame, _fb, (size_t)PANEL_NATIVE_W * PANEL_NATIVE_H * 2);               // 1:1 video
  size_t n = (size_t)PANEL_NATIVE_W * PANEL_NATIVE_H;
  for (size_t i = 0; i < n; i++) {             // blend the keyboard overlay (50/50) where it's not transparent
    uint16_t o = _oskFb[i];
    if (o != OSK_OVERLAY_TRANSPARENT)
      frame[i] = (uint16_t)(((o >> 1) & 0x7BEF) + ((frame[i] >> 1) & 0x7BEF));
  }
  dsiPanelDrawBitmap(0, 0, PANEL_NATIVE_W, PANEL_NATIVE_H, frame);
}

// Composite + push ONLY the keyboard's band (logical y >= ~100 -> the bottom of the panel) in one DSI
// transfer. Used by the tiny386 render loop when ONLY the keyboard changed (a key press) so it doesn't
// re-flush the whole 1024x600 frame -- the full composite made the on-screen keyboard laggy.
void DisplayGFX::flushOskBand() {
  if (!_fb || !_oskFb) return;
  const int y0 = PANEL_NATIVE_H * 100 / DISP_LOGICAL_H;   // keyboard top (logical ~100) -> panel row
  const int h  = PANEL_NATIVE_H - y0;
  static uint16_t *band = nullptr;
  if (!band) band = (uint16_t *)ps_malloc((size_t)PANEL_NATIVE_W * h * 2);
  if (!band) { flushDSI(); return; }   // fall back to the full flush if the band buffer won't allocate
  for (int r = 0; r < h; r++) {
    const uint16_t *vrow = _fb    + (size_t)(y0 + r) * PANEL_NATIVE_W;
    const uint16_t *orow = _oskFb + (size_t)(y0 + r) * PANEL_NATIVE_W;
    uint16_t *drow = band + (size_t)r * PANEL_NATIVE_W;
    for (int x = 0; x < PANEL_NATIVE_W; x++) {
      uint16_t o = orow[x];
      drow[x] = (o != OSK_OVERLAY_TRANSPARENT)
                ? (uint16_t)(((o >> 1) & 0x7BEF) + ((vrow[x] >> 1) & 0x7BEF))   // 50/50 blend
                : vrow[x];
    }
  }
  dsiPanelDrawBitmap(0, y0, PANEL_NATIVE_W, h, band);
}
#endif // BOARD_PANEL_DSI

// Touch (XPT2046 on its own SPI) is read directly in touchkeyboard.cpp; these stay stubs.
uint16_t DisplayGFX::getTouchRawZ() { return 0; }
void DisplayGFX::getTouchRaw(uint16_t *x, uint16_t *y) { if (x) *x = 0; if (y) *y = 0; }

extern DisplayGFX tft;
void displayFlush() { tft.flush(); }
void displaySetUiMode(bool ui) { tft.setUiMode(ui); }

#endif // BOARD_DISPLAY_GFX
