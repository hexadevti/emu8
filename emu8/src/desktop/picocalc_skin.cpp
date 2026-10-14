// picocalc_skin.cpp — the PicoCalc drawn as a picture of the device (see picocalc_sim.h).
//
// docs/picocalc.png is linked into emu8-picocalc.exe (picocalc_skin_png.S) and
// decoded once with zlib. The emulator's 320x320 panel is drawn into the picture's screen glass and
// every key in the picture is clickable. Click Shift / Ctrl / Alt to latch them for the next key
// (Ctrl, then Shift, then F1 = the system menu), like sticky keys; click a latched one again to drop it.
// The PicoCalc has no touch panel, so clicks on the screen do nothing, as on the device.
#if defined(BOARD_DESKTOP) && defined(EMU_SIM_PICOCALC)

#include "picocalc_sim.h"
#include "imgui.h"
#include <SDL.h>
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// --- the embedded picture (picocalc_skin_png.S) ---------------------------------------------------
extern "C" const unsigned char skin_png[]     asm("emu8_skin_png");
extern "C" const unsigned char skin_png_end[] asm("emu8_skin_png_end");

// Geometry of docs/picocalc.png, in its pixels.
static const int   kSkinW = 955, kSkinH = 1580;
static const int   kGlassX0 = 151, kGlassY0 = 146, kGlassX1 = 793, kGlassY1 = 776;   // the screen
static const float kPadX0 = 100, kPadY0 = 848, kPadX1 = 282, kPadY1 = 1027;           // the D-pad
static const float kPadCX = 191, kPadCY = 938;

// --- minimal PNG decoder: 8-bit RGB / RGBA, non-interlaced (all docs/picocalc.png needs) ----------
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static bool decodePng(const unsigned char *png, size_t len, std::vector<uint8_t> &rgba, int &w, int &h)
{
  static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (len < 8 || memcmp(png, sig, 8) != 0) return false;
  int depth = 0, ctype = 0, interlace = 0;
  std::vector<uint8_t> idat;
  w = h = 0;
  for (size_t p = 8; p + 12 <= len; ) {
    uint32_t n = be32(png + p);
    const unsigned char *type = png + p + 4, *data = png + p + 8;
    if (p + 12 + n > len) return false;
    if (!memcmp(type, "IHDR", 4) && n >= 13) {
      w = (int)be32(data); h = (int)be32(data + 4);
      depth = data[8]; ctype = data[9]; interlace = data[12];
    } else if (!memcmp(type, "IDAT", 4)) {
      idat.insert(idat.end(), data, data + n);
    } else if (!memcmp(type, "IEND", 4)) {
      break;
    }
    p += 12 + n;
  }
  if (w <= 0 || h <= 0 || depth != 8 || interlace != 0 || (ctype != 2 && ctype != 6)) return false;
  const int bpp = (ctype == 6) ? 4 : 3;
  const size_t stride = (size_t)w * bpp;
  std::vector<uint8_t> raw((stride + 1) * h);
  uLongf rawLen = (uLongf)raw.size();
  if (uncompress(raw.data(), &rawLen, idat.data(), (uLong)idat.size()) != Z_OK || rawLen != raw.size()) return false;

  std::vector<uint8_t> img(stride * h);
  for (int y = 0; y < h; y++) {
    const uint8_t filter = raw[y * (stride + 1)];
    const uint8_t *src = &raw[y * (stride + 1) + 1];
    uint8_t *cur = &img[y * stride];
    const uint8_t *prev = y ? &img[(y - 1) * stride] : nullptr;
    for (size_t i = 0; i < stride; i++) {
      int a = i >= (size_t)bpp ? cur[i - bpp] : 0;
      int b = prev ? prev[i] : 0;
      int c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;
      int v = src[i];
      switch (filter) {
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) >> 1; break;
        case 4: {
          int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c);
          v += (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
          break;
        }
        default: break;
      }
      cur[i] = (uint8_t)v;
    }
  }
  rgba.resize((size_t)w * h * 4);
  for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
    rgba[i * 4 + 0] = img[i * bpp + 0];
    rgba[i * 4 + 1] = img[i * bpp + 1];
    rgba[i * 4 + 2] = img[i * bpp + 2];
    rgba[i * 4 + 3] = (bpp == 4) ? img[i * bpp + 3] : 255;
  }
  return true;
}

// The picture is nearly always shown smaller than it is. A GPU bilinear shrink samples only 2x2
// source pixels, so the keys' 3-pixel black outlines come out 1 pixel on some keys and 2 on others.
// So for a shrink it is resampled here instead (area average, alpha-weighted) to the exact size it
// is drawn at, and drawn 1:1. Larger than the picture, the GPU's bilinear stretch is fine.
static std::vector<uint8_t> g_skinRgba;
static bool         g_skinFailed = false;
static SDL_Texture *g_skinTex = nullptr;
static int          g_texW = 0, g_texH = 0;

// For each destination pixel, the source pixels it covers and how much of each.
struct Tap { int src; float w; };
static std::vector<std::vector<Tap>> areaTaps(int src, int dst)
{
  std::vector<std::vector<Tap>> taps(dst);
  const double f = (double)src / dst;
  for (int i = 0; i < dst; i++) {
    double a = i * f, b = (i + 1) * f;
    for (int s = (int)a; s < b && s < src; s++) {
      double cover = std::min(b, (double)s + 1) - std::max(a, (double)s);
      if (cover > 1e-6) taps[i].push_back({s, (float)(cover / f)});
    }
  }
  return taps;
}

static std::vector<uint8_t> shrinkSkin(int dw, int dh)
{
  const int sw = kSkinW, sh = kSkinH;
  std::vector<float> pre((size_t)sw * sh * 4);              // premultiplied, so edges don't darken
  for (size_t i = 0, n = (size_t)sw * sh; i < n; i++) {
    float a = g_skinRgba[i * 4 + 3] / 255.0f;
    pre[i * 4 + 0] = g_skinRgba[i * 4 + 0] * a;
    pre[i * 4 + 1] = g_skinRgba[i * 4 + 1] * a;
    pre[i * 4 + 2] = g_skinRgba[i * 4 + 2] * a;
    pre[i * 4 + 3] = g_skinRgba[i * 4 + 3];
  }
  const auto tx = areaTaps(sw, dw), ty = areaTaps(sh, dh);
  std::vector<float> rows((size_t)dw * sh * 4);             // horizontal pass
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < dw; x++) {
      float acc[4] = {0, 0, 0, 0};
      for (const Tap &t : tx[x]) {
        const float *p = &pre[((size_t)y * sw + t.src) * 4];
        for (int c = 0; c < 4; c++) acc[c] += p[c] * t.w;
      }
      memcpy(&rows[((size_t)y * dw + x) * 4], acc, sizeof acc);
    }
  std::vector<uint8_t> out((size_t)dw * dh * 4);            // vertical pass, un-premultiply
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      float acc[4] = {0, 0, 0, 0};
      for (const Tap &t : ty[y]) {
        const float *p = &rows[((size_t)t.src * dw + x) * 4];
        for (int c = 0; c < 4; c++) acc[c] += p[c] * t.w;
      }
      uint8_t *o = &out[((size_t)y * dw + x) * 4];
      float a = acc[3];
      for (int c = 0; c < 3; c++) o[c] = (uint8_t)std::min(255.0f, a > 0.5f ? acc[c] * 255.0f / a + 0.5f : 0.0f);
      o[3] = (uint8_t)std::min(255.0f, a + 0.5f);
    }
  return out;
}

// The skin texture for drawing at dw x dh pixels (or null if the picture could not be decoded).
static SDL_Texture *skinTexture(SDL_Renderer *ren, int dw, int dh)
{
  if (g_skinFailed) return nullptr;
  if (g_skinRgba.empty()) {
    int w, h;
    if (!decodePng(skin_png, (size_t)(skin_png_end - skin_png), g_skinRgba, w, h) || w != kSkinW || h != kSkinH) {
      fprintf(stderr, "picocalc: could not decode the embedded skin (%dx%d)\n", w, h);
      g_skinFailed = true;
      return nullptr;
    }
  }
  if (dw >= kSkinW || dh >= kSkinH) { dw = kSkinW; dh = kSkinH; }   // enlarged: the full picture
  if (g_skinTex && dw == g_texW && dh == g_texH) return g_skinTex;
  // while the window is being resized, stretch the old copy; redo it once the size holds still
  static int lastW = 0, lastH = 0, still = 0;
  if (dw != lastW || dh != lastH) { lastW = dw; lastH = dh; still = 0; }
  if (g_skinTex && ++still < 10) return g_skinTex;

  std::vector<uint8_t> shrunk;
  const uint8_t *px = g_skinRgba.data();
  if (dw != kSkinW || dh != kSkinH) { shrunk = shrinkSkin(dw, dh); px = shrunk.data(); }
  if (g_skinTex) SDL_DestroyTexture(g_skinTex);
  g_skinTex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, dw, dh);   // bytes R,G,B,A
  if (!g_skinTex) { g_skinFailed = true; return nullptr; }
  SDL_UpdateTexture(g_skinTex, nullptr, px, dw * 4);
  SDL_SetTextureBlendMode(g_skinTex, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(g_skinTex, SDL_ScaleModeLinear);
  g_texW = dw; g_texH = dh;
  return g_skinTex;
}

// --- the keys, in picture pixels ------------------------------------------------------------------
struct SkinKey { short x0, y0, x1, y1; uint8_t key; };
static std::vector<SkinKey> g_keys;

static void buildKeys()
{
  if (!g_keys.empty()) return;
  auto add = [](int x0, int y0, int x1, int y1, uint8_t k) {
    g_keys.push_back({(short)x0, (short)y0, (short)x1, (short)y1, k});
  };
  static const short fx[5][2] = {{326, 418}, {437, 528}, {547, 639}, {657, 749}, {768, 859}};
  static const uint8_t fRow[5]   = {PCK_F1, PCK_F2, PCK_F3, PCK_F4, PCK_F5};
  static const uint8_t row2[5]   = {PCK_ESC, PCK_TAB, PCK_CAPS_LOCK, PCK_DEL, PCK_BACKSPACE};
  for (int i = 0; i < 5; i++) {
    add(fx[i][0], 867, fx[i][1], 910, fRow[i]);
    add(fx[i][0], 931, fx[i][1], 973, row2[i]);
  }
  static const short col[10] = {90, 169, 247, 326, 405, 484, 563, 641, 720, 799};   // 61 wide
  static const char  sym[] = "`/\\-=[]";                                          // columns 3..9
  for (int i = 0; i < 7; i++) add(col[i + 3], 994, col[i + 3] + 61, 1055, (uint8_t)sym[i]);
  static const char *rows[4] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm,."};
  static const short rowY[4] = {1076, 1157, 1239, 1320};
  for (int r = 0; r < 4; r++)
    for (int i = 0; rows[r][i]; i++) add(col[i], rowY[r], col[i] + 61, rowY[r] + 61, (uint8_t)rows[r][i]);
  add(799, 1239, 860, 1381, PCK_ENTER);
  add(108, 1402, 210, 1463, PCK_MOD_SHL);
  add(228, 1402, 289, 1463, PCK_MOD_CTRL);
  add(307, 1402, 367, 1463, PCK_MOD_ALT);
  add(385, 1402, 564, 1463, ' ');
  add(582, 1402, 643, 1463, ';');
  add(661, 1402, 722, 1463, '\'');
  add(740, 1402, 841, 1463, PCK_MOD_SHR);
}

// Which key is at picture point (x, y)? 0 = none. Keys get a few pixels of slack so the gaps
// between them still hit the nearest one.
static uint8_t keyAt(float x, float y, SkinKey *rect)
{
  for (const SkinKey &k : g_keys) {
    if (x >= k.x0 - 8 && x < k.x1 + 8 && y >= k.y0 - 8 && y < k.y1 + 8) { if (rect) *rect = k; return k.key; }
  }
  if (x >= kPadX0 && x < kPadX1 && y >= kPadY0 && y < kPadY1) {
    float dx = x - kPadCX, dy = y - kPadCY;
    if (dx * dx + dy * dy < 15 * 15) return 0;                      // the D-pad's centre
    uint8_t k = (fabsf(dx) > fabsf(dy)) ? (dx < 0 ? PCK_LEFT : PCK_RIGHT) : (dy < 0 ? PCK_UP : PCK_DOWN);
    if (rect) {
      const float t = 60;                                           // highlight the pressed arm
      switch (k) {
        case PCK_LEFT:  *rect = {(short)kPadX0, (short)(kPadCY - t / 2), (short)(kPadCX - 15), (short)(kPadCY + t / 2), k}; break;
        case PCK_RIGHT: *rect = {(short)(kPadCX + 15), (short)(kPadCY - t / 2), (short)kPadX1, (short)(kPadCY + t / 2), k}; break;
        case PCK_UP:    *rect = {(short)(kPadCX - t / 2), (short)kPadY0, (short)(kPadCX + t / 2), (short)(kPadCY - 15), k}; break;
        default:        *rect = {(short)(kPadCX - t / 2), (short)(kPadCY + 15), (short)(kPadCX + t / 2), (short)kPadY1, k}; break;
      }
    }
    return k;
  }
  return 0;
}

static bool isModifier(uint8_t k)
{
  return k == PCK_MOD_SHL || k == PCK_MOD_SHR || k == PCK_MOD_CTRL || k == PCK_MOD_ALT;
}

// --- mouse state --------------------------------------------------------------------------------
static uint8_t g_held = 0;              // key held by the mouse button
static SkinKey g_heldRect;
static bool    g_latched[256];          // modifiers latched by a click

static void releaseLatched()
{
  for (int k = 0; k < 256; k++)
    if (g_latched[k]) { g_latched[k] = false; pcSimPhysKey((uint8_t)k, false, false); }
}

bool pcSimSkinDraw(SDL_Renderer *ren, SDL_Texture *emuTex, bool linear, int *sx, int *sy, int *sw, int *sh)
{
  if (g_skinFailed) return false;
  buildKeys();

  // aspect-fit the picture into the window, centred, on whole pixels (so the shrunk copy is 1:1)
  ImVec2 avail = ImGui::GetContentRegionAvail();
  if (avail.x < 8 || avail.y < 8) return true;
  float fit = avail.x / kSkinW;
  if (kSkinH * fit > avail.y) fit = avail.y / kSkinH;
  const int dw = std::max(1, (int)(kSkinW * fit)), dh = std::max(1, (int)(kSkinH * fit));
  SDL_Texture *skin = skinTexture(ren, dw, dh);
  if (!skin) return false;
  const float scale = (float)dw / kSkinW;
  ImVec2 cur = ImGui::GetCursorScreenPos();
  ImVec2 o(floorf(cur.x + (avail.x - dw) * 0.5f), floorf(cur.y + (avail.y - dh) * 0.5f));
  auto at = [&](float x, float y) { return ImVec2(o.x + x * scale, o.y + y * scale); };

  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddImage((ImTextureID)(intptr_t)skin, o, ImVec2(o.x + dw, o.y + dh));
  SDL_SetTextureScaleMode(emuTex, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
  ImVec2 g0 = at(kGlassX0, kGlassY0), g1 = at(kGlassX1, kGlassY1);
  dl->AddImage((ImTextureID)(intptr_t)emuTex, g0, g1);
  if (sx) *sx = (int)g0.x;
  if (sy) *sy = (int)g0.y;
  if (sw) *sw = (int)(g1.x - g0.x);
  if (sh) *sh = (int)(g1.y - g0.y);

  // one invisible button over the whole picture: it owns the mouse (no window drag) and reports
  // press / release; the key under the pointer is looked up by hand
  ImGui::SetCursorScreenPos(o);
  ImGui::InvisibleButton("##picocalc", ImVec2(kSkinW * scale, kSkinH * scale));
  ImVec2 m = ImGui::GetIO().MousePos;
  const float px = (m.x - o.x) / scale, py = (m.y - o.y) / scale;

  if (ImGui::IsItemActivated()) {
    SkinKey r;
    uint8_t k = keyAt(px, py, &r);
    if (isModifier(k)) {
      g_latched[k] = !g_latched[k];
      pcSimPhysKey(k, g_latched[k], false);
    } else if (k) {
      g_held = k; g_heldRect = r;
      pcSimPhysKey(k, true, false);
    }
  }
  if (g_held && !ImGui::IsItemActive()) {        // button released (or focus lost)
    pcSimPhysKey(g_held, false, false);
    g_held = 0;
    if (g_heldRect.key != PCK_CAPS_LOCK) releaseLatched();   // one-shot modifiers
  }

  // highlights: held key, latched modifiers, caps lock, hovered key
  auto mark = [&](const SkinKey &k, ImU32 col) {
    dl->AddRectFilled(at(k.x0, k.y0), at(k.x1, k.y1), col, 6.0f * scale);
  };
  for (const SkinKey &k : g_keys) {
    if (g_latched[k.key]) mark(k, IM_COL32(80, 160, 255, 110));
    if (k.key == PCK_CAPS_LOCK && pcSimCapsLock()) mark(k, IM_COL32(255, 190, 40, 90));
  }
  if (g_held) mark(g_heldRect, IM_COL32(255, 255, 255, 90));
  else if (ImGui::IsItemHovered()) {
    SkinKey r;
    if (keyAt(px, py, &r)) { mark(r, IM_COL32(255, 255, 255, 35)); ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
  }
  return true;
}

#endif // BOARD_DESKTOP && EMU_SIM_PICOCALC
