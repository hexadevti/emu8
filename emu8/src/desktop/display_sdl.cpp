// display_sdl.cpp — SDL2 implementation of the desktop DisplayGFX (320x240 RGB565 framebuffer,
// scaled to the window). Text uses the same 1:1 bitmap fonts and renderer as the PicoCalc
// (src/shared/uitext.h). flush() also pumps SDL input events.
//
// EMU_SIM_PICOCALC (CMake option EMU8_SIM_PICOCALC) turns the framebuffer into the PicoCalc's
// 320x320 panel: emulator video letterboxes into rows 40..279 and the settings menu switches to
// full-panel mode with the PicoCalc layout, so menu changes can be checked without flashing.
#if defined(BOARD_DESKTOP)

#include "display_sdl.h"
#include "ui_imgui.h"
#include "device.h"
#include <SDL.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <algorithm>

#include "../shared/uitext.h"   // menu fonts + renderer, shared with the PicoCalc backend

extern DisplayGFX tft;
extern void desktopPumpInput();   // input_sdl.cpp: drains SDL events (keyboard/gamepad/mouse/quit)
extern bool screenFill;           // Settings: SCREEN FILL (zoom video to fill window) vs ORIG (globals.cpp)
extern bool OptionsWindow;        // settings menu open -> always show ORIG so the menu isn't zoomed
extern bool DebugWindow;

// Emulator framebuffer size. Defaults to 320x240 (like the CYD); PC-XT enlarge it via
// desktopSetEmuResolution() so their authentic PC fonts render at native resolution (not squished).
// W x H is the whole framebuffer; logical drawing lands at row g_offY and is clipped to g_logH rows.
// Without the PicoCalc simulation the two coincide (offset 0, logical height = H).
#if defined(EMU_SIM_PICOCALC)
static int W = DISP_LOGICAL_W, H = SIM_PANEL_H;
static int g_offY = SIM_OFFSET_Y, g_logH = DISP_LOGICAL_H;
#else
static int W = DISP_LOGICAL_W, H = DISP_LOGICAL_H;
static int g_offY = 0, g_logH = DISP_LOGICAL_H;
#endif
#if defined(EMU_SIM_PICOCALC)
static int            g_winW   = 1280, g_winH = 1000;  // default window: the tall device picture + panels
#else
static int            g_winW   = 1024, g_winH = 768;   // default desktop window (4:3, like 320x240)
#endif
static SDL_Window    *g_win    = nullptr;
static SDL_Renderer  *g_ren    = nullptr;
static SDL_Texture   *g_tex    = nullptr;

// Active emulator-video content rect inside the 320x240 framebuffer (set per frame by the cores via
// displaySetVideoRect/Fill). FILL mode zooms this rect to the whole window; defaults = the full fb.
static int g_vidTop = 0, g_vidH = H, g_vidLeft = 0, g_vidW = W;

// Resize the emulator framebuffer. PC-XT setup() call this BEFORE begin(), so begin() just
// allocates _fb / g_tex at the new size. (No live-resize path is needed: desktop platform switching
// re-execs, so the size is chosen once per process — at begin() — for the booted platform.)
void desktopSetEmuResolution(int w, int h) {
  if (w < 64 || h < 48 || w > 4096 || h > 4096) return;
#if defined(EMU_SIM_PICOCALC)
  return;                     // the simulated panel is fixed at 320x320, like the real one
#endif
  W = w; H = h; g_logH = h;
  g_vidTop = 0; g_vidH = H; g_vidLeft = 0; g_vidW = W;
}

void DisplayGFX::begin() {
  if (g_win) return;   // idempotent, like the PicoCalc's (its boot progress screen calls it first)
  // Restore the last session's window size (and view prefs / open panels) before creating the window.
  desktopUiLoadConfig();
  int cw = 0, ch = 0; desktopUiGetWindowSize(&cw, &ch);
  if (cw >= 320) g_winW = cw;
  if (ch >= 240) g_winH = ch;
  // EMU_W/EMU_H still override (used by the offline-capture harness).
  if (const char *s = getenv("EMU_W")) { int v = atoi(s); if (v >= 320) g_winW = v; }
  if (const char *s = getenv("EMU_H")) { int v = atoi(s); if (v >= 240) g_winH = v; }
  _fb = (uint16_t *)calloc((size_t)W * H, sizeof(uint16_t));
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");   // linear filtering (smooth non-integer upscale)
  char title[96];
  snprintf(title, sizeof(title), "emu8 - %s", desktopDeviceInfo().name);
  g_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           g_winW, g_winH, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
  g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, W, H);
  if (!g_win || !g_ren || !g_tex) Serial.printf("DisplayGFX: SDL init failed: %s\n", SDL_GetError());
  desktopUiInit(g_win, g_ren);   // native ImGui shell shares this window/renderer (presents via flush())
}

// Expose the active emulator-video content rect (set per frame by displaySetVideoRect/Fill) so the
// ImGui shell's "Crop borders" view can sample just that sub-rect of the 320x240 framebuffer.
void desktopGetVideoRect(int *l, int *t, int *w, int *h) {
  if (l) *l = g_vidLeft; if (t) *t = g_vidTop; if (w) *w = g_vidW; if (h) *h = g_vidH;
}

// Live SDL window size (for persisting the frame size across sessions).
void desktopGetCurrentWindowSize(int *w, int *h) {
  if (g_win) SDL_GetWindowSize(g_win, w, h);
  else { if (w) *w = g_winW; if (h) *h = g_winH; }
}

// --- the PicoCalc's LCD link ---
// On the device every pixel crosses an SPI link at F_CPU/4 (board.h LCD_SPI_HZ: 62.5MHz on the
// RP2040 at 250MHz, 50MHz on the RP2350 at 200MHz), 16 bits each, and the render task waits for it.
// That wire time is what sets the PicoCalc's frame rates (and why its cores push only changed bands),
// so the simulator charges it too: drawing counts pixels, flush() sleeps until the link would be free.
#if defined(EMU_SIM_PICOCALC)
static std::atomic<uint32_t> g_wirePx{0};
static inline void wire(int64_t n) { if (n > 0) g_wirePx.fetch_add((uint32_t)n, std::memory_order_relaxed); }
static void wirePace() {
  static const double hz = desktopDevice() == DEV_PICOCALC_RP2350 ? 50e6 : 62.5e6;
  static const uint64_t freq = SDL_GetPerformanceFrequency();
  static uint64_t busyUntil = 0;                       // when the link finishes what was sent so far
  const uint32_t n = g_wirePx.exchange(0, std::memory_order_relaxed);
  const uint64_t now = SDL_GetPerformanceCounter();
  if (busyUntil < now) busyUntil = now;
  busyUntil += (uint64_t)(n * 16.0 / hz * (double)freq);
  const uint32_t ms = (uint32_t)((busyUntil - now) * 1000 / freq);
  if (ms) SDL_Delay(ms);
}
#else
static inline void wire(int64_t) {}
#endif

// --- pixel helpers (LOGICAL coords, clipped to the logical area) ---
static inline void px(uint16_t *fb, int x, int y, uint16_t c) {
  // fb may be null if a draw happens before begin() allocates the framebuffer (e.g. the file-scan
  // progress bar runs during <core>Setup, before videoSetup). Match the device (guarded no-op).
  if (fb && (unsigned)x < (unsigned)W && (unsigned)y < (unsigned)g_logH) fb[(y + g_offY) * W + x] = c;
}

void DisplayGFX::fillScreen(uint16_t color) {
  if (!_fb) return;
  for (int i = g_offY * W; i < (g_offY + g_logH) * W; i++) _fb[i] = color;
  wire((int64_t)W * g_logH);
}
void DisplayGFX::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  if (!_fb) return;
  for (int32_t yy = y; yy < y + h; yy++)
    for (int32_t xx = x; xx < x + w; xx++) px(_fb, xx, yy, color);
  const int32_t cw = std::min<int32_t>(x + w, W) - std::max<int32_t>(x, 0);
  const int32_t ch = std::min<int32_t>(y + h, g_logH) - std::max<int32_t>(y, 0);
  if (cw > 0 && ch > 0) wire((int64_t)cw * ch);
}
// Rounded corners: the PicoCalc backend's midpoint circle (display_picocalc.cpp), so the menus
// look the same as on the device. dx[d] = how far the arc reaches sideways d rows from the centre.
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
  int32_t f = 1 - r, ddx = 1, ddy = -2 * r, px = 0, py = r;
  while (px < py) {
    if (f >= 0) { py--; ddy += 2; f += ddy; }
    px++; ddx += 2; f += ddx;
    if (px > dx[py]) dx[py] = (int8_t)px;
    if (py > dx[px]) dx[px] = (int8_t)py;
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
  // The corner pixels outside the arc are left alone: they belong to whatever is behind the box.
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
  fillRect(cl,        y,         cr - cl + 1, 1, color);
  fillRect(cl,        y + h - 1, cr - cl + 1, 1, color);
  fillRect(x,         ct,        1, cb - ct + 1, color);
  fillRect(x + w - 1, ct,        1, cb - ct + 1, color);
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
    wire(w);
  }
}
void DisplayGFX::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) { blit(data, x, y, w, h); }
void DisplayGFX::pushPanelBand(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) { blit(data, x, y, w, h); }

// 8x8 glyph nearest-scaled into a pw x ph cell (ported from the Arduino_GFX backend, clipped to W x H).
void DisplayGFX::drawGlyph8(int px, int py, int pw, int ph, const uint8_t *g, uint16_t fg, uint16_t bg, bool transparentBg) {
  if (!_fb || !g || pw <= 0 || ph <= 0) return;
  for (int oy = 0; oy < ph; oy++) {
    int yy = py + oy;
    if ((unsigned)yy >= (unsigned)g_logH) continue;
    uint8_t bits = g[(oy * 8) / ph];                 // nearest-neighbour source row
    uint16_t *row = _fb + (size_t)(yy + g_offY) * W;
    for (int ox = 0; ox < pw; ox++) {
      int xx = px + ox;
      if ((unsigned)xx >= (unsigned)W) continue;
      bool on = bits & (0x80 >> ((ox * 8) / pw));
      if (on) row[xx] = fg;
      else if (!transparentBg) row[xx] = bg;
    }
    wire(pw);
  }
}

// --- Apple II scanline window --- (kept in RAW framebuffer rows, like the PicoCalc's panel window:
// writes are not clipped to the logical area, only to the framebuffer)
void DisplayGFX::setAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h) {
  setPanelAddrWindow(x, y + g_offY, w, h);
}
void DisplayGFX::setPanelAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h) {
  _winX = x; _winY = y; _winW = w; _winH = h; _curX = x; _curY = y;
}
void DisplayGFX::writeColor(uint16_t color, uint32_t len) {
  if (!_fb) return;
  const int32_t xEnd = _winX + _winW, yEnd = _winY + _winH;
  const bool scale = _hsc && len < 256;               // the device's bulk fill skips the 7:8 doubling
  wire(len);
  while (len--) {
    const int reps = (scale && --_hsc == 0) ? 2 : 1;
    if (reps == 2) wire(1);
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

// SCREEN: SMOOTH, the device's smoothKick() (display_picocalc.cpp) on the framebuffer row just done.
static inline uint16_t avg565(uint16_t a, uint16_t b) {
  return (uint16_t)((((a ^ b) & 0xF7DE) >> 1) + (a & b));
}
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

// --- text: 1:1 bitmap fonts (src/shared/uitext.h), identical to the PicoCalc's ---
int16_t DisplayGFX::textWidth(const char *s, uint8_t font) { return (int16_t)uiTextWidth(s, font); }

int16_t DisplayGFX::drawString(const char *s, int32_t x, int32_t y, uint8_t font) {
  if (!_fb || !s) return 0;
  int32_t left, top;
  uiTextOrigin(s, x, y, _datum, font, left, top);
  const int w = uiTextWidth(s, font);
  if (_textHasBg && w > 0) fillRect(left, top, w, uiTextCellH(font), _textBg);
  uint16_t *fb = _fb; const uint16_t fg = _textFg; int64_t lit = 0;
  uiTextDraw(s, left, top, font, [fb, fg, &lit](int32_t tx, int32_t ty) { px(fb, tx, ty, fg); lit++; });
  wire(lit);
  return (int16_t)w;
}

#if defined(EMU_SIM_PICOCALC)
// --- PicoCalc panel simulation: same contract as display_picocalc.cpp ---
bool DisplayGFX::setFullPanel(bool on) {
  const int off = on ? 0 : SIM_OFFSET_Y;
  if (off == g_offY) return false;
  g_offY = off; g_logH = on ? SIM_PANEL_H : DISP_LOGICAL_H;
  return true;
}
bool    DisplayGFX::fullPanel() const { return g_offY == 0; }
int32_t DisplayGFX::height() const    { return g_logH; }
void DisplayGFX::fillPanelRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  if (!_fb) return;
  for (int32_t yy = y; yy < y + h; yy++)
    for (int32_t xx = x; xx < x + w; xx++)
      if ((unsigned)xx < (unsigned)W && (unsigned)yy < (unsigned)H) _fb[yy * W + xx] = color;
  wire((int64_t)w * h);
}
void DisplayGFX::fillPanelBlack() {
  fillPanelRect(0, 0, W, SIM_OFFSET_Y, TFT_BLACK);
  fillPanelRect(0, SIM_OFFSET_Y + DISP_LOGICAL_H, W, H - SIM_OFFSET_Y - DISP_LOGICAL_H, TFT_BLACK);
}
int16_t DisplayGFX::drawPanelString(const char *s, int32_t x, int32_t y, uint8_t font) {
  if (!_fb || !s) return 0;
  int32_t left, top;
  uiTextOrigin(s, x, y, _datum, font, left, top);
  uint16_t *fb = _fb; const uint16_t fg = _textFg; const int fw = W, fh = H; int64_t lit = 0;
  uiTextDraw(s, left, top, font, [fb, fg, fw, fh, &lit](int32_t tx, int32_t ty) {
    if ((unsigned)tx < (unsigned)fw && (unsigned)ty < (unsigned)fh) { fb[ty * fw + tx] = fg; lit++; }
  });
  wire(lit);
  return (int16_t)uiTextWidth(s, font);
}
#endif

// Headless capture: dump the 320x240 framebuffer to a PPM (EMU_DUMP_DIR + EMU_DUMP_EVERY, from
// EMU_DUMP_FROM) and exit
// after EMU_QUIT_AT frames, so the desktop build can be driven offline for diagnosis.
static void desktopFrameHook(const uint16_t *fb) {
  static long frame = -1; frame++;
  static const char *dir   = getenv("EMU_DUMP_DIR");
  static int   every = []{ const char*s=getenv("EMU_DUMP_EVERY"); return s?atoi(s):0; }();
  static long  quitAt= []{ const char*s=getenv("EMU_QUIT_AT");   return s?atol(s):0L; }();
  static long  from  = []{ const char*s=getenv("EMU_DUMP_FROM"); return s?atol(s):0L; }();
  if (fb && dir && every > 0 && frame >= from && (frame % every) == 0) {
    char path[512]; snprintf(path, sizeof(path), "%s/f%06ld.ppm", dir, frame);
    FILE *f = fopen(path, "wb");
    if (f) {
      fprintf(f, "P6\n%d %d\n255\n", W, H);
      for (int i = 0; i < W * H; i++) { uint16_t c = fb[i];
        fputc((((c >> 11) & 0x1F) << 3), f); fputc((((c >> 5) & 0x3F) << 2), f); fputc(((c & 0x1F) << 3), f); }
      fclose(f);
    }
  }
  if (quitAt > 0 && frame >= quitAt) std::exit(0);
}

// --- present + input pump (runs on the main thread inside renderLoop) ---
void DisplayGFX::flush() {
  desktopPumpInput();                                   // keyboard / gamepad / mouse / SDL_QUIT (before any early-out)
  desktopFrameHook(_fb);                                // headless capture / quit
  if (!g_ren || !g_tex || !_fb) return;
#if defined(EMU_SIM_PICOCALC)
  wirePace();                                           // what was drawn takes the device's SPI time
#endif
  // Upload the freshly-rendered 320x240 framebuffer, then hand off to the native ImGui shell, which
  // draws it (aspect-fit, dockable) plus the menu bar / settings / debug panels and presents. The old
  // raw RenderCopy path (full-window stretch / SCREEN-FILL crop) is now handled inside the shell
  // (aspect-fit + the View > "Crop borders" toggle).
  SDL_UpdateTexture(g_tex, nullptr, _fb, W * (int)sizeof(uint16_t));
  desktopUiFrame(g_tex, W, H);
}

// --- mouse == touch (window pixels -> logical 320x240, via the ImGui shell's image rect) ---
// Only register a touch when the pointer is over the emulator image; a click on the ImGui chrome
// (menu bar / debug panels) must NOT poke the emulated screen.
void DisplayGFX::setMouseState(int winX, int winY, bool down) {
  int fx, fy;
  if (desktopUiMapToEmu(winX, winY, &fx, &fy)) {
    if (fx < 0) fx = 0; else if (fx > W - 1) fx = W - 1;
    if (fy < 0) fy = 0; else if (fy > H - 1) fy = H - 1;
    _mx = fx; _my = fy; _mdown = down;
  } else {
    _mdown = false;
  }
}
uint16_t DisplayGFX::getTouchRawZ() { return _mdown ? 1000 : 0; }
void DisplayGFX::getTouchRaw(uint16_t *x, uint16_t *y) { if (x) *x = (uint16_t)_mx; if (y) *y = (uint16_t)_my; }

// --- free-function shims (declared in proto.h / display_sdl.h) ---
void displayFlush() { tft.flush(); }
void displaySetUiMode(bool ui) { tft.setUiMode(ui); }
bool desktopUiModeActive() { return tft.uiMode() || OptionsWindow; }
// Cores report their active video content rect each frame (the rest of 320x240 is black border);
// The S3/P4 panel presentation (ui_imgui.cpp) zooms this rect in SCREEN FILL, like display_gfx.cpp.
static bool g_vidStretch = false;
void displaySetVideoRect(int topLogical, int hLogical) {
  g_vidTop = topLogical + g_offY; g_vidH = (hLogical > 0) ? hLogical : g_logH;
  g_vidLeft = 0; g_vidW = W;                      // defaults; displaySetVideoFill() overrides after
  g_vidStretch = false;
}
void displaySetVideoFill(int leftLogical, int wLogical, bool stretch) {
  g_vidLeft = leftLogical; g_vidW = (wLogical > 0) ? wLogical : W;
  g_vidStretch = stretch;
}
bool desktopVideoStretch() { return g_vidStretch; }

#endif // BOARD_DESKTOP
