// display_web.cpp — browser (Emscripten) implementation of the desktop DisplayGFX.
//
// Same drawing code as src/desktop/display_sdl.cpp (320x240 RGB565 framebuffer, 1:1 bitmap fonts from
// src/shared/uitext.h), minus SDL/ImGui. flush() converts the framebuffer to RGBA into one of two
// "front" buffers and publishes it; the page's requestAnimationFrame reads the latest one straight
// out of the (shared) wasm memory and paints it on a <canvas>. Touch arrives already mapped to
// framebuffer coordinates (input_web.cpp -> setMouseState()).
#if defined(BOARD_DESKTOP) && defined(EMU_WEB)

#include "src/desktop/display_sdl.h"
#include <emscripten.h>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "src/shared/uitext.h"

extern DisplayGFX tft;
extern void desktopPumpInput();    // input_web.cpp
extern uint8_t screenFill;         // SCREEN_ORIG / SCREEN_ENLARGE / SCREEN_STRETCH
extern bool OptionsWindow;

static int W = DISP_LOGICAL_W, H = DISP_LOGICAL_H;
static const int g_offY = 0;
static int g_logH = DISP_LOGICAL_H;

static int g_vidTop = 0, g_vidH = DISP_LOGICAL_H, g_vidLeft = 0, g_vidW = DISP_LOGICAL_W;
static bool g_vidStretch = false;

// Two RGBA front buffers; g_front = index of the last complete one, g_frameNo bumps per flush.
static uint8_t *g_rgba[2] = {nullptr, nullptr};
static std::atomic<int> g_front{0};
static std::atomic<uint32_t> g_frameNo{0};
static int g_rgbaW = 0, g_rgbaH = 0;

void desktopSetEmuResolution(int w, int h) {
  if (w < 64 || h < 48 || w > 4096 || h > 4096) return;
  W = w; H = h; g_logH = h;
  g_vidTop = 0; g_vidH = H; g_vidLeft = 0; g_vidW = W;
}

void DisplayGFX::begin() {
  if (_fb) return;
  _fb = (uint16_t *)calloc((size_t)W * H, sizeof(uint16_t));
  g_rgba[0] = (uint8_t *)calloc((size_t)W * H, 4);
  g_rgba[1] = (uint8_t *)calloc((size_t)W * H, 4);
  g_rgbaW = W; g_rgbaH = H;
}

static inline void wire(int64_t) {}

static inline void px(uint16_t *fb, int x, int y, uint16_t c) {
  if (fb && (unsigned)x < (unsigned)W && (unsigned)y < (unsigned)g_logH) fb[(y + g_offY) * W + x] = c;
}

void DisplayGFX::fillScreen(uint16_t color) {
  if (!_fb) return;
  for (int i = g_offY * W; i < (g_offY + g_logH) * W; i++) _fb[i] = color;
}
void DisplayGFX::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  if (!_fb) return;
  const int32_t x0 = std::max<int32_t>(x, 0), x1 = std::min<int32_t>(x + w, W);
  const int32_t y0 = std::max<int32_t>(y, 0), y1 = std::min<int32_t>(y + h, g_logH);
  for (int32_t yy = y0; yy < y1; yy++) {
    uint16_t *row = _fb + (size_t)(yy + g_offY) * W;
    for (int32_t xx = x0; xx < x1; xx++) row[xx] = color;
  }
}

static const int32_t kMaxCornerR = 31;
static int32_t roundRectRadius(int32_t w, int32_t h, int32_t r) {
  int32_t m = (w < h ? w : h) / 2;
  if (r > m) r = m;
  if (r > kMaxCornerR) r = kMaxCornerR;
  return r < 0 ? 0 : r;
}
static void cornerReach(int32_t r, int8_t *dx) {
  for (int32_t i = 0; i <= r; i++) dx[i] = 0;
  dx[0] = (int8_t)r;
  int32_t f = 1 - r, ddx = 1, ddy = -2 * r, px_ = 0, py = r;
  while (px_ < py) {
    if (f >= 0) { py--; ddy += 2; f += ddy; }
    px_++; ddx += 2; f += ddx;
    if (px_ > dx[py]) dx[py] = (int8_t)px_;
    if (py > dx[px_]) dx[px_] = (int8_t)py;
  }
}
void DisplayGFX::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  r = roundRectRadius(w, h, r);
  if (r == 0) { fillRect(x, y, w, h, color); return; }
  int8_t dx[kMaxCornerR + 1];
  cornerReach(r, dx);
  const int32_t cl = x + r, cr = x + w - 1 - r;
  const int32_t ct = y + r, cb = y + h - 1 - r;
  for (int32_t d = r; d >= 1; d--)
    fillRect(cl - dx[d], ct - d, (cr + dx[d]) - (cl - dx[d]) + 1, 1, color);
  if (cb >= ct) fillRect(x, ct, w, cb - ct + 1, color);
  for (int32_t d = 1; d <= r; d++)
    fillRect(cl - dx[d], cb + d, (cr + dx[d]) - (cl - dx[d]) + 1, 1, color);
}
void DisplayGFX::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  r = roundRectRadius(w, h, r);
  const int32_t cl = x + r, cr = x + w - 1 - r;
  const int32_t ct = y + r, cb = y + h - 1 - r;
  fillRect(cl, y, cr - cl + 1, 1, color);
  fillRect(cl, y + h - 1, cr - cl + 1, 1, color);
  fillRect(x, ct, 1, cb - ct + 1, color);
  fillRect(x + w - 1, ct, 1, cb - ct + 1, color);
  if (r == 0) return;
  int8_t dx[kMaxCornerR + 1];
  cornerReach(r, dx);
  for (int32_t d = 1; d <= r; d++) {
    int32_t hi = dx[d];
    int32_t lo = (d < r) ? dx[d + 1] + 1 : 0;
    if (lo > hi) lo = hi;
    int32_t n = hi - lo + 1;
    fillRect(cl - hi, ct - d, n, 1, color);
    fillRect(cr + lo, ct - d, n, 1, color);
    fillRect(cl - hi, cb + d, n, 1, color);
    fillRect(cr + lo, cb + d, n, 1, color);
  }
}

void DisplayGFX::blit(const uint16_t *data, int32_t x, int32_t y, int32_t w, int32_t h) {
  if (!_fb || !data) return;
  for (int32_t row = 0; row < h; row++) {
    int32_t cy = y + row;
    if ((unsigned)cy >= (unsigned)g_logH) continue;
    const uint16_t *src = data + row * w;
    for (int32_t col = 0; col < w; col++) px(_fb, x + col, cy, src[col]);
  }
}
void DisplayGFX::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) { blit(data, x, y, w, h); }
void DisplayGFX::pushPanelBand(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) { blit(data, x, y, w, h); }

void DisplayGFX::drawGlyph8(int px_, int py, int pw, int ph, const uint8_t *g, uint16_t fg, uint16_t bg, bool transparentBg) {
  if (!_fb || !g || pw <= 0 || ph <= 0) return;
  for (int oy = 0; oy < ph; oy++) {
    int yy = py + oy;
    if ((unsigned)yy >= (unsigned)g_logH) continue;
    uint8_t bits = g[(oy * 8) / ph];
    uint16_t *row = _fb + (size_t)(yy + g_offY) * W;
    for (int ox = 0; ox < pw; ox++) {
      int xx = px_ + ox;
      if ((unsigned)xx >= (unsigned)W) continue;
      bool on = bits & (0x80 >> ((ox * 8) / pw));
      if (on) row[xx] = fg;
      else if (!transparentBg) row[xx] = bg;
    }
  }
}

void DisplayGFX::setAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h) { setPanelAddrWindow(x, y + g_offY, w, h); }
void DisplayGFX::setPanelAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h) {
  _winX = x; _winY = y; _winW = w; _winH = h; _curX = x; _curY = y;
}
void DisplayGFX::writeColor(uint16_t color, uint32_t len) {
  if (!_fb) return;
  const int32_t xEnd = _winX + _winW, yEnd = _winY + _winH;
  const bool scale = _hsc && len < 256;
  while (len--) {
    const int reps = (scale && --_hsc == 0) ? 2 : 1;
    for (int rep = reps; rep; rep--) {
      if ((unsigned)_curX < (unsigned)W && (unsigned)_curY < (unsigned)H) _fb[_curY * W + _curX] = color;
      if (++_curX >= xEnd) {
        if (_smoothOn) smoothRow(_curY);
        _curX = _winX; if (++_curY >= yEnd) _curY = _winY;
      }
    }
    if (scale && _hsc == 0) _hsc = 7;
  }
}

static inline uint16_t avg565(uint16_t a, uint16_t b) { return (uint16_t)((((a ^ b) & 0xF7DE) >> 1) + (a & b)); }
void DisplayGFX::smoothRow(int32_t y) {
  const uint8_t mode = _rowBlend;
  _rowBlend = 0;
  if ((unsigned)y >= (unsigned)H || _winX < 0 || _winX + _winW > W) { _prevValid = false; return; }
  uint16_t *r = _fb + y * W + _winX;
  if (_hscale)
    for (int32_t i = 7; i < _winW - 1; i += 8) r[i] = avg565(r[i - 1], r[i + 1]);
  if (_heldY >= 0) {
    uint16_t *h = _fb + _heldY * W + _winX;
    for (int32_t i = 0; i < _winW; i++) h[i] = avg565(h[i], r[i]);
    _heldY = -1;
  } else if (mode == 1 && _prevValid && y > 0) {
    const uint16_t *u = r - W;
    for (int32_t i = 0; i < _winW; i++) r[i] = avg565(r[i], u[i]);
  } else if (mode == 2) {
    _heldY = y;
  }
  _prevValid = true;
}

int16_t DisplayGFX::textWidth(const char *s, uint8_t font) { return (int16_t)uiTextWidth(s, font); }
int16_t DisplayGFX::drawString(const char *s, int32_t x, int32_t y, uint8_t font) {
  if (!_fb || !s) return 0;
  int32_t left, top;
  uiTextOrigin(s, x, y, _datum, font, left, top);
  const int w = uiTextWidth(s, font);
  if (_textHasBg && w > 0) fillRect(left, top, w, uiTextCellH(font), _textBg);
  uint16_t *fb = _fb; const uint16_t fg = _textFg;
  uiTextDraw(s, left, top, font, [fb, fg](int32_t tx, int32_t ty) { px(fb, tx, ty, fg); });
  return (int16_t)w;
}

// --- present: RGB565 -> RGBA into the back buffer, then publish it ---
static uint8_t g_lut5[32], g_lut6[64];
void webHiddenWait();
void DisplayGFX::flush() {
  webHiddenWait();                             // page hidden: no frames (the cores are parked too)
  desktopPumpInput();
  if (!_fb) return;
  static bool lutInit = false;
  if (!lutInit) {
    for (int i = 0; i < 32; i++) g_lut5[i] = (uint8_t)((i << 3) | (i >> 2));
    for (int i = 0; i < 64; i++) g_lut6[i] = (uint8_t)((i << 2) | (i >> 4));
    lutInit = true;
  }
  if (g_rgbaW != W || g_rgbaH != H) {          // PC-XT resized before begin(): realloc to match
    for (int i = 0; i < 2; i++) { free(g_rgba[i]); g_rgba[i] = (uint8_t *)calloc((size_t)W * H, 4); }
    g_rgbaW = W; g_rgbaH = H;
  }
  const int back = 1 - g_front.load(std::memory_order_acquire);
  uint8_t *d = g_rgba[back];
  const int n = W * H;
  for (int i = 0; i < n; i++) {
    uint16_t c = _fb[i];
    d[0] = g_lut5[c >> 11]; d[1] = g_lut6[(c >> 5) & 0x3F]; d[2] = g_lut5[c & 0x1F]; d[3] = 255;
    d += 4;
  }
  g_front.store(back, std::memory_order_release);
  g_frameNo.fetch_add(1, std::memory_order_release);
}

void DisplayGFX::setMouseState(int fx, int fy, bool down) {
  if (fx < 0 || fy < 0) { _mdown = false; return; }
  if (fx > W - 1) fx = W - 1;
  if (fy > H - 1) fy = H - 1;
  _mx = fx; _my = fy; _mdown = down;
}
uint16_t DisplayGFX::getTouchRawZ() { return _mdown ? 1000 : 0; }
void DisplayGFX::getTouchRaw(uint16_t *x, uint16_t *y) { if (x) *x = (uint16_t)_mx; if (y) *y = (uint16_t)_my; }

void displayFlush() { tft.flush(); }
void displaySetUiMode(bool ui) { tft.setUiMode(ui); }
bool displayReadVideo(int x, int y, int w, int h, uint16_t *buf) { return tft.readVideo(x, y, w, h, buf); }
bool DisplayGFX::readVideo(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t *buf) {
  if (!_fb || !buf || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > W || y + h > H) return false;
  for (int r = 0; r < h; r++) memcpy(buf + (size_t)r * w, _fb + (size_t)(y + r) * W + x, (size_t)w * 2);
  return true;
}
bool desktopUiModeActive() { return tft.uiMode() || OptionsWindow; }
void displaySetVideoRect(int topLogical, int hLogical) {
  g_vidTop = topLogical + g_offY; g_vidH = (hLogical > 0) ? hLogical : g_logH;
  g_vidLeft = 0; g_vidW = W;
  g_vidStretch = false;
}
void displaySetVideoFill(int leftLogical, int wLogical, bool stretch) {
  g_vidLeft = leftLogical; g_vidW = (wLogical > 0) ? wLogical : W;
  g_vidStretch = stretch;
}
bool desktopVideoStretch() { return g_vidStretch; }

// ===================== exports read by the page (main browser thread) =====================
extern "C" {
EMSCRIPTEN_KEEPALIVE uint32_t web_frame_no() { return g_frameNo.load(std::memory_order_acquire); }
EMSCRIPTEN_KEEPALIVE uint8_t *web_frame_ptr() { return g_rgba[g_front.load(std::memory_order_acquire)]; }
EMSCRIPTEN_KEEPALIVE int web_fb_w() { return g_rgbaW; }
EMSCRIPTEN_KEEPALIVE int web_fb_h() { return g_rgbaH; }
// The part of the framebuffer worth showing: out[0..3] = x, y, w, h; out[4] = 1 to stretch it to the
// full width (SCREEN: STRETCH), 0 to keep its aspect. Menus / splash always show the whole frame.
EMSCRIPTEN_KEEPALIVE void web_view_rect(int *out) {
  out[0] = 0; out[1] = 0; out[2] = g_rgbaW; out[3] = g_rgbaH; out[4] = 0;
  if (desktopUiModeActive()) return;
  int l = g_vidLeft, t = g_vidTop, w = g_vidW, h = g_vidH;
  if (screenFill && w > 0 && h > 0 && l >= 0 && t >= 0 && l + w <= g_rgbaW && t + h <= g_rgbaH) {
    out[0] = l; out[1] = t; out[2] = w; out[3] = h;
    out[4] = (screenFill == SCREEN_STRETCH && g_vidStretch) ? 1 : 0;
  }
}
}

#endif // BOARD_DESKTOP && EMU_WEB
