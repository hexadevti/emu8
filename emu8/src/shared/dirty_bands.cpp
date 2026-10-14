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
      // 256x192 -> 320x240, nearest neighbour: each 8 source lines become 10 (sy = oy*4/5) and
      // each 4 source pixels become 5 (the first one doubled).
      tft.setAddrWindow(0, b * 10, 320, (e - b) * 10);
      tft.startWrite();
      for (int oy = b * 10; oy < e * 10; oy++) {
        const uint8_t* src = fb + (oy * 4 / 5) * W;
        int x = 0;
        if (hideLeft8) { tft.writeColor(0x0000, 10); x = 8; }
        for (; x < W; x += 4) {
          const uint16_t c0 = pal[src[x] & mask];
          tft.writeColor(c0, 1);
          tft.writeColor(c0, 1);
          tft.writeColor(pal[src[x + 1] & mask], 1);
          tft.writeColor(pal[src[x + 2] & mask], 1);
          tft.writeColor(pal[src[x + 3] & mask], 1);
        }
      }
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
