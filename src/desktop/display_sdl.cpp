// display_sdl.cpp — SDL2 implementation of the desktop DisplayGFX (320x240 RGB565 framebuffer,
// scaled to the window). Text is rendered with the bundled FreeSans9pt7b GFX font for every size
// (the device uses a 6x8 built-in for font 1; on desktop the proportional font is used throughout —
// a cosmetic difference only, not core logic). flush() also pumps SDL input events.
#if defined(BOARD_DESKTOP)

#include "display_sdl.h"
#include "ui_imgui.h"
#include <SDL.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>

// --- GFX font structs (Adafruit layout) so the bundled font header compiles without Arduino_GFX ---
typedef struct {
  uint16_t bitmapOffset;
  uint8_t  width, height, xAdvance;
  int8_t   xOffset, yOffset;
} GFXglyph;
typedef struct {
  uint8_t  *bitmap;
  GFXglyph *glyph;
  uint16_t  first, last;
  uint8_t   yAdvance;
} GFXfont;
#include "../shared/fonts/FreeSans9pt7b.h"
static const GFXfont *UIFONT = &FreeSans9pt7b;

extern DisplayGFX tft;
extern void desktopPumpInput();   // input_sdl.cpp: drains SDL events (keyboard/gamepad/mouse/quit)
extern bool screenFill;           // Settings: SCREEN FILL (zoom video to fill window) vs ORIG (globals.cpp)
extern bool OptionsWindow;        // settings menu open -> always show ORIG so the menu isn't zoomed
extern bool DebugWindow;

// Emulator framebuffer size. Defaults to 320x240 (like the CYD); PC-XT / tiny386 enlarge it via
// desktopSetEmuResolution() so their authentic PC fonts render at native resolution (not squished).
static int W = DISP_LOGICAL_W, H = DISP_LOGICAL_H;
static int            g_winW   = 1024, g_winH = 768;   // default desktop window (4:3, like 320x240)
static SDL_Window    *g_win    = nullptr;
static SDL_Renderer  *g_ren    = nullptr;
static SDL_Texture   *g_tex    = nullptr;

// Active emulator-video content rect inside the 320x240 framebuffer (set per frame by the cores via
// displaySetVideoRect/Fill). FILL mode zooms this rect to the whole window; defaults = the full fb.
static int g_vidTop = 0, g_vidH = H, g_vidLeft = 0, g_vidW = W;

// Resize the emulator framebuffer. PC-XT / tiny386 setup() call this BEFORE begin(), so begin() just
// allocates _fb / g_tex at the new size. (No live-resize path is needed: desktop platform switching
// re-execs, so the size is chosen once per process — at begin() — for the booted platform.)
void desktopSetEmuResolution(int w, int h) {
  if (w < 64 || h < 48 || w > 4096 || h > 4096) return;
  W = w; H = h;
  g_vidTop = 0; g_vidH = H; g_vidLeft = 0; g_vidW = W;
}

void DisplayGFX::begin() {
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
  g_win = SDL_CreateWindow("emu8 (desktop)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
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

// --- pixel helpers (clipped to 320x240) ---
static inline void px(uint16_t *fb, int x, int y, uint16_t c) {
  // fb may be null if a draw happens before begin() allocates the framebuffer (e.g. the file-scan
  // progress bar runs during <core>Setup, before videoSetup). Match the device (guarded no-op).
  if (fb && (unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) fb[y * W + x] = c;
}

void DisplayGFX::fillScreen(uint16_t color) {
  if (!_fb) return;
  for (int i = 0; i < W * H; i++) _fb[i] = color;
}
void DisplayGFX::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  if (!_fb) return;
  for (int32_t yy = y; yy < y + h; yy++)
    for (int32_t xx = x; xx < x + w; xx++) px(_fb, xx, yy, color);
}
void DisplayGFX::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  // good-enough rounded rect: a full rect minus the four corner pixels (debug UI doesn't need AA).
  fillRect(x, y, w, h, color);
  if (r > 0) {
    for (int i = 0; i < r; i++) for (int j = 0; j < r; j++) {
      if (i * i + j * j > r * r) {
        px(_fb, x + (r - 1 - i), y + (r - 1 - j), TFT_BLACK);
        px(_fb, x + w - r + i,   y + (r - 1 - j), TFT_BLACK);
        px(_fb, x + (r - 1 - i), y + h - r + j,   TFT_BLACK);
        px(_fb, x + w - r + i,   y + h - r + j,   TFT_BLACK);
      }
    }
  }
}
void DisplayGFX::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  for (int32_t xx = x + r; xx < x + w - r; xx++) { px(_fb, xx, y, color); px(_fb, xx, y + h - 1, color); }
  for (int32_t yy = y + r; yy < y + h - r; yy++) { px(_fb, x, yy, color); px(_fb, x + w - 1, yy, color); }
}

void DisplayGFX::blit(const uint16_t *data, int32_t x, int32_t y, int32_t w, int32_t h) {
  if (!_fb || !data) return;
  for (int32_t row = 0; row < h; row++) {
    int32_t cy = y + row;
    if ((unsigned)cy >= (unsigned)H) continue;
    const uint16_t *src = data + row * w;
    for (int32_t col = 0; col < w; col++) px(_fb, x + col, cy, src[col]);
  }
}
void DisplayGFX::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) { blit(data, x, y, w, h); }
void DisplayGFX::pushPanelBand(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) { blit(data, x, y, w, h); }

// 8x8 glyph nearest-scaled into a pw x ph cell (ported from the Arduino_GFX backend, clipped to W x H).
void DisplayGFX::drawGlyph8(int px, int py, int pw, int ph, const uint8_t *g, uint16_t fg, uint16_t bg, bool transparentBg) {
  if (!_fb || !g || pw <= 0 || ph <= 0) return;
  for (int oy = 0; oy < ph; oy++) {
    int yy = py + oy;
    if ((unsigned)yy >= (unsigned)H) continue;
    uint8_t bits = g[(oy * 8) / ph];                 // nearest-neighbour source row
    uint16_t *row = _fb + (size_t)yy * W;
    for (int ox = 0; ox < pw; ox++) {
      int xx = px + ox;
      if ((unsigned)xx >= (unsigned)W) continue;
      bool on = bits & (0x80 >> ((ox * 8) / pw));
      if (on) row[xx] = fg;
      else if (!transparentBg) row[xx] = bg;
    }
  }
}

// --- Apple II scanline window ---
void DisplayGFX::setAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h) {
  _winX = x; _winY = y; _winW = w; _winH = h; _curX = x; _curY = y;
}
void DisplayGFX::writeColor(uint16_t color, uint32_t len) {
  if (!_fb) return;
  const int32_t xEnd = _winX + _winW, yEnd = _winY + _winH;
  while (len--) {
    px(_fb, _curX, _curY, color);
    if (++_curX >= xEnd) { _curX = _winX; if (++_curY >= yEnd) _curY = _winY; }
  }
}

// --- text (FreeSans9pt7b for all sizes). Font >= 2 (titles/buttons) renders 1:1; font < 2 (the
//     dense option/label/file rows that use the device's 6x8) is scaled down ~2/3 so it stays small,
//     using OR-downsampling so thin strokes survive the shrink. SC_NUM/SC_DEN = the small-font scale.
#define SC_NUM 2
#define SC_DEN 3
static inline int scN(int v, int num, int den) { return (v * num) / den; }

// Is the glyph's pixel (sx,sy) set? (Adafruit GFX bitmaps stream row-major, MSB first.)
static inline bool glyphBit(const uint8_t *bmp, int gw, int sx, int sy) {
  int idx = sy * gw + sx;
  return (bmp[idx >> 3] & (0x80 >> (idx & 7))) != 0;
}

static void strMetrics(const char *s, int num, int den, int &bw, int &minYO, int &maxYExt) {
  bw = 0; minYO = 0; maxYExt = 0; bool any = false;
  for (const char *p = s; *p; p++) {
    uint8_t c = (uint8_t)*p;
    if (c < UIFONT->first || c > UIFONT->last) { bw += scN(UIFONT->yAdvance / 2, num, den); continue; }
    const GFXglyph *g = &UIFONT->glyph[c - UIFONT->first];
    bw += scN(g->xAdvance, num, den);
    int yo = scN(g->yOffset, num, den), ext = scN(g->yOffset + g->height, num, den);
    if (!any || yo < minYO) minYO = yo;
    if (!any || ext > maxYExt) maxYExt = ext;
    any = true;
  }
  if (!any) { minYO = scN(-(int)UIFONT->yAdvance + 4, num, den); maxYExt = 0; }
}

int16_t DisplayGFX::drawString(const char *s, int32_t x, int32_t y, uint8_t font) {
  if (!_fb || !s) return 0;
  const int num = (font >= 2) ? 1 : SC_NUM;     // titles/buttons full size; rows shrunk
  const int den = (font >= 2) ? 1 : SC_DEN;

  int bw, minYO, maxYExt;
  strMetrics(s, num, den, bw, minYO, maxYExt);
  int bh = maxYExt - minYO;
  const uint8_t horiz = _datum % 3, vert = _datum / 3;
  int boxLeft = x, boxTop = y;
  if (horiz == 1) boxLeft = x - bw / 2; else if (horiz == 2) boxLeft = x - bw;
  if (vert == 1)  boxTop  = y - bh / 2; else if (vert == 2)  boxTop = y - bh;
  if (_textHasBg) fillRect(boxLeft, boxTop, bw, bh > 0 ? bh : 1, _textBg);

  int baseline = boxTop - minYO;   // output rows = baseline + scaled(yOffset) + outRow
  int cursorX = boxLeft;
  for (const char *p = s; *p; p++) {
    uint8_t c = (uint8_t)*p;
    if (c < UIFONT->first || c > UIFONT->last) { cursorX += scN(UIFONT->yAdvance / 2, num, den); continue; }
    const GFXglyph *g = &UIFONT->glyph[c - UIFONT->first];
    const uint8_t *bmp = UIFONT->bitmap + g->bitmapOffset;
    int ow = (g->width  * num + den - 1) / den;        // scaled glyph box (round up so nothing is lost)
    int oh = (g->height * num + den - 1) / den;
    int ox0 = cursorX + scN(g->xOffset, num, den);
    int oy0 = baseline + scN(g->yOffset, num, den);
    for (int oy = 0; oy < oh; oy++) {
      int sy0 = (oy * den) / num, sy1 = ((oy + 1) * den + num - 1) / num;
      if (sy1 <= sy0) sy1 = sy0 + 1; if (sy1 > g->height) sy1 = g->height;
      for (int ox = 0; ox < ow; ox++) {
        int sx0 = (ox * den) / num, sx1 = ((ox + 1) * den + num - 1) / num;
        if (sx1 <= sx0) sx1 = sx0 + 1; if (sx1 > g->width) sx1 = g->width;
        bool on = false;                               // OR-downsample: keep the stroke if any covered src pixel is set
        for (int sy = sy0; sy < sy1 && !on; sy++)
          for (int sx = sx0; sx < sx1; sx++)
            if (glyphBit(bmp, g->width, sx, sy)) { on = true; break; }
        if (on) px(_fb, ox0 + ox, oy0 + oy, _textFg);
      }
    }
    cursorX += scN(g->xAdvance, num, den);
  }
  return (int16_t)bw;
}

// Headless capture: dump the 320x240 framebuffer to a PPM (EMU_DUMP_DIR + EMU_DUMP_EVERY) and exit
// after EMU_QUIT_AT frames, so the desktop build can be driven offline for diagnosis.
static void desktopFrameHook(const uint16_t *fb) {
  static long frame = -1; frame++;
  static const char *dir   = getenv("EMU_DUMP_DIR");
  static int   every = []{ const char*s=getenv("EMU_DUMP_EVERY"); return s?atoi(s):0; }();
  static long  quitAt= []{ const char*s=getenv("EMU_QUIT_AT");   return s?atol(s):0L; }();
  if (fb && dir && every > 0 && (frame % every) == 0) {
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
// Cores report their active video content rect each frame (the rest of 320x240 is black border);
// SCREEN FILL zooms this rect to the whole window. (stretch flag ignored — FILL always fills.)
void displaySetVideoRect(int topLogical, int hLogical) {
  g_vidTop = topLogical; g_vidH = (hLogical > 0) ? hLogical : H;
  g_vidLeft = 0; g_vidW = W;                      // defaults; displaySetVideoFill() overrides after
}
void displaySetVideoFill(int leftLogical, int wLogical, bool /*stretch*/) {
  g_vidLeft = leftLogical; g_vidW = (wLogical > 0) ? wLogical : W;
}

#endif // BOARD_DESKTOP
