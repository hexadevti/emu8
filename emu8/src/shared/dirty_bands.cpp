// dirty_bands.cpp - see dirty_bands.h.
#include "../../emu.h"
#if UI_PICOCALC   // the device and the desktop PicoCalc simulator

#include "dirty_bands.h"
#include <stdio.h>

namespace dband {

static uint32_t sums[MAX_BANDS];
static uint32_t lastKey = 0;
static uint32_t lastCallMs = 0, lastFullMs = 0;
static bool     full = true;

static uint32_t statMs = 0, statFrames = 0, statBands = 0;

uint32_t hash(uint32_t h, const uint8_t* p, uint32_t n) {
  if ((((uintptr_t)p | n) & 3) == 0) {               // the M0+ faults on unaligned word loads
    const uint32_t* w = (const uint32_t*)p;
    for (uint32_t i = 0; i < n / 4; i++) h = (h ^ w[i]) * 16777619u;
    return h;
  }
  for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

bool begin(uint32_t modeKey, bool force) {
  const uint32_t now = millis();
  full = force || modeKey != lastKey || (now - lastCallMs > 150) || (now - lastFullMs > 1000);
  lastCallMs = now;
  lastKey = modeKey;
  if (full) lastFullMs = now;
  return full;
}

bool changed(int b, uint32_t sum) {
  if (b < 0 || b >= MAX_BANDS) return true;
  if (!full && sums[b] == sum) return false;
  sums[b] = sum;
  return true;
}

void frameDone(const char* tag, int bandsSent) {
  const uint32_t now = millis();
  if (statFrames == 0 && statMs == 0) statMs = now;
  statFrames++; statBands += (uint32_t)bandsSent;
  fpsFrameShown();
  if (now - statMs >= 5000) {
    char line[80];                                   // not the global buf: the Z80 core logs too
    snprintf(line, sizeof(line), "%s: panel fps=%u.%u bands/frame=%u", tag,
             (unsigned)(statFrames * 1000 / (now - statMs)),
             (unsigned)(statFrames * 10000 / (now - statMs) % 10),
             (unsigned)(statFrames ? statBands / statFrames : 0));
    printLog(line);
    statMs = now; statFrames = 0; statBands = 0;
  }
}

const uint8_t kRowMain[5] = {0, 1, 1, 2, 3}, kRowOther[5] = {0, 0, 2, 3, 3}, kRowW[5] = {0, 1, 2, 1, 0};

// One output row of the 4 -> 5 picture from RGB565 lines: line m, blended with line t when w is 1
// (m 3/4, t 1/4) or 2 (half each); w 0 = line m alone. n source pixels (a multiple of 4).
void writeSmoothRow45(const uint16_t* m, const uint16_t* t, int w, int n) {
  uint16_t o[5], p[5];
  for (int x = 0; x < n; x += 4) {
    smooth5(m[x], m[x + 1], m[x + 2], m[x + 3], o);
    if (w) {
      smooth5(t[x], t[x + 1], t[x + 2], t[x + 3], p);
      for (int i = 0; i < 5; i++) o[i] = (w == 2) ? avg565(o[i], p[i]) : q3_565(o[i], p[i]);
    }
    for (int i = 0; i < 5; i++) tft.writeColor(o[i], 1);
  }
}

// Same from indexed lines (256 pixels; hideLeft8 blanks source pixels 0..7 = output 0..9).
static void smoothRowIdx(const uint8_t* m, const uint8_t* t, int w, const uint16_t* pal, uint8_t mask,
                         bool hideLeft8) {
  int x = 0;
  if (hideLeft8) { tft.writeColor(0x0000, 10); x = 8; }
  uint16_t o[5], p[5];
  for (; x < 256; x += 4) {
    smooth5(pal[m[x] & mask], pal[m[x + 1] & mask], pal[m[x + 2] & mask], pal[m[x + 3] & mask], o);
    if (w) {
      smooth5(pal[t[x] & mask], pal[t[x + 1] & mask], pal[t[x + 2] & mask], pal[t[x + 3] & mask], p);
      for (int i = 0; i < 5; i++) o[i] = (w == 2) ? avg565(o[i], p[i]) : q3_565(o[i], p[i]);
    }
    for (int i = 0; i < 5; i++) tft.writeColor(o[i], 1);
  }
}

void writeSmoothBand256(const uint8_t* band, const uint16_t* pal, uint8_t mask, bool hideLeft8) {
  for (int r = 0; r < 10; r++) {
    const uint8_t* s0 = band + (r / 5) * 4 * 256;   // 5-row group covers 4 source lines
    const int k = r % 5;
    smoothRowIdx(s0 + kRowMain[k] * 256, s0 + kRowOther[k] * 256, kRowW[k], pal, mask, hideLeft8);
  }
}

// One window per run of consecutive dirty bands, so the pixels of a run stream through
// writeColor()'s ping-pong DMA staging without a drain between bands.
int pushIndexed256(const uint8_t* fb, const uint16_t* pal, uint8_t mask, int ox, int top,
                   bool fill, bool hideLeft8, uint32_t modeKey, bool force) {
  const int W = 256, BANDS = 192 / 8;
  uint32_t ph = SEED;
  for (int i = 0; i <= mask; i++) ph = mix(ph, pal[i]);
  begin(mix(mix(mix(modeKey, ph), fill), hideLeft8), force);

  bool dirty[BANDS];
  for (int b = 0; b < BANDS; b++) dirty[b] = changed(b, hash(SEED, fb + b * 8 * W, 8 * W));

  int sent = 0;
  for (int b = 0; b < BANDS; ) {
    if (!dirty[b]) { b++; continue; }
    int e = b;
    while (e < BANDS && dirty[e]) e++;               // bands b..e-1
    sent += e - b;
    if (fill) {
      // 256x192 -> 320x240 bilinear: each 8-line band becomes 10 rows.
      tft.setAddrWindow(0, b * 10, 320, (e - b) * 10);
      tft.startWrite();
      for (int k = b; k < e; k++) writeSmoothBand256(fb + k * 8 * W, pal, mask, hideLeft8);
    } else {
      tft.setAddrWindow(ox, top + b * 8, W, (e - b) * 8);
      tft.startWrite();
      for (int sy = b * 8; sy < e * 8; sy++) {
        const uint8_t* src = fb + sy * W;
        int x = 0;
        if (hideLeft8) { tft.writeColor(0x0000, 8); x = 8; }
        for (; x < W; x++) tft.writeColor(pal[src[x] & mask], 1);
      }
    }
    tft.endWrite();
    b = e;
  }
  return sent;
}

}  // namespace dband

#endif  // UI_PICOCALC
