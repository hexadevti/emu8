// dirty_bands.h - send only the parts of an emulated picture that changed since the last push.
//
// On the PicoCalc the panel sits on a 50 MHz SPI link: a full 256x192 picture costs ~16 ms and the
// ZX Spectrum's 320x240 (paper + border) ~25 ms. The Z80 machines only render a new frame once the
// render task has finished pushing the previous one (the frameReady handshake), so a full push per
// frame holds the panel at roughly half the machine's 50/60 Hz. Most frames change a few rows (a
// sprite, a score, a scroller), so the picture is split into bands of 8 source lines, each band is
// checksummed, and only bands whose checksum moved go out -- the same scheme as c64.cpp.
//
// Everything is re-sent when the caller says so (menu closed, palette / mode change), after a gap in
// rendering (something else drew on the panel) and once a second regardless, so an overlay drawn
// over the picture by anyone else never lingers.
//
// One shared state: only one emulator runs at a time, and switching mode keys forces a full push.
#pragma once
#include <stdint.h>

namespace dband {

static const uint32_t SEED = 2166136261u;           // FNV-1a offset basis
static const int      MAX_BANDS = 32;

// FNV-1a over n bytes, word-at-a-time when p is 4-byte aligned and n a multiple of 4.
uint32_t hash(uint32_t h, const uint8_t* p, uint32_t n);
inline uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

// Start a frame. modeKey identifies everything that changes how bands map to pixels (machine,
// FILL, palette hash, ...): a new key forces a full push, as does force. Returns true for a full push.
bool begin(uint32_t modeKey, bool force);
// Band b's checksum is `sum`: true if it must be sent (always on a full push). Records it.
bool changed(int b, uint32_t sum);
// Count one frame shown and the bands it sent; logs "<tag>: panel fps=.. bands/frame=.." every 5 s.
void frameDone(const char* tag, int bandsSent);

// 256x192 indexed framebuffer -> panel, used by MSX, ColecoVision and SMS. fill is the SCREEN mode:
// ORIG 1:1 at (ox, top); ENLARGE the whole 320x240 logical screen scaled 5:4 bilinear
// (writeSmoothBand256); STRETCH the whole raw 320x320 panel, 5:4 across and 5:3 down
// (writeStretchLines256). hideLeft8 paints source pixels 0..7 black (SMS left-column blanking).
// Returns the number of bands sent.
int pushIndexed256(const uint8_t* fb, const uint16_t* pal, uint8_t mask, int ox, int top,
                   uint8_t fill, bool hideLeft8, uint32_t modeKey, bool force);

// ---- SCREEN: ENLARGE / STRETCH: bilinear upscaling in quarter steps ---------------------------
// RGB565 blends. avg565 is the exact per-channel floor average (the low bit of each channel is
// masked off before the shift, so no carry crosses a channel); q3_565 weighs a 3/4, b 1/4;
// blendQ weighs b q/4 (q 0..4).
inline uint16_t avg565(uint16_t a, uint16_t b) { return (uint16_t)((((a ^ b) & 0xF7DE) >> 1) + (a & b)); }
inline uint16_t q3_565(uint16_t a, uint16_t b) { return avg565(a, avg565(a, b)); }
inline uint16_t blendQ(uint16_t a, uint16_t b, int q) {
  return q <= 0 ? a : q == 1 ? q3_565(a, b) : q == 2 ? avg565(a, b) : q == 3 ? q3_565(b, a) : b;
}

// 4 -> 5. Output pixel k samples source 0.8k - 0.1 (pixel centres) = 0, 0.7, 1.5, 2.3, 3.1, rounded
// to quarters:  s0 | s0/4 + 3s1/4 | (s1 + s2)/2 | 3s2/4 + s3/4 | s3.  A group never reads outside its
// 4 source pixels, and vertically outside its 4 source lines -- so an 8-line band (2 groups) maps
// to exactly 10 rows from its own lines only, and dirty-band bookkeeping is unchanged.
inline void smooth5(uint16_t c0, uint16_t c1, uint16_t c2, uint16_t c3, uint16_t* o) {
  o[0] = c0; o[1] = q3_565(c1, c0); o[2] = avg565(c1, c2); o[3] = q3_565(c2, c3); o[4] = c3;
}
// Row k (0..4) of a 5-row group: source line kRowMain[k] of the group's 4, blended with
// kRowOther[k] by kRowW[k] (0 none, 1 = main 3/4, 2 = half each) -- smooth5's table, vertically.
extern const uint8_t kRowMain[5], kRowOther[5], kRowW[5];
// One output row (n source pixels -> n*5/4) from RGB565 lines m and t blended by w (as kRowW).
void writeSmoothRow45(const uint16_t* m, const uint16_t* t, int w, int n);
// 8 lines of a 256-wide indexed picture -> 10 rows of 320, through writeColor().
void writeSmoothBand256(const uint8_t* band, const uint16_t* pal, uint8_t mask, bool hideLeft8);

// SCREEN: STRETCH, any height. Source lines [y0, y0 + n) of a srcH-line picture own output rows
// [stretchRow(y0), stretchRow(y0 + n)) of outH, so consecutive bands tile the output exactly.
inline int stretchRow(int y, int srcH, int outH) { return (y * outH + srcH - 1) / srcH; }
// Those rows of a 256-wide indexed picture (fb = line 0): 5:4 across (smooth5), and down each row
// blends its two nearest source lines in quarters -- clamped to [y0, y0 + n), so a band redraws the
// same whichever bands around it are skipped as unchanged. Through writeColor(), 320 px per row.
void writeStretchLines256(const uint8_t* fb, int y0, int n, int srcH, int outH,
                          const uint16_t* pal, uint8_t mask, bool hideLeft8);

}  // namespace dband
