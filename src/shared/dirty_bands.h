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

// 256x192 indexed framebuffer -> panel, used by MSX, ColecoVision and SMS. 1:1 at (ox, top), or
// with fill the whole 320x240 logical screen scaled 5:4 (each 8-line band becomes 10 rows). hideLeft8
// paints source pixels 0..7 black (SMS left-column blanking). Returns the number of bands sent.
int pushIndexed256(const uint8_t* fb, const uint16_t* pal, uint8_t mask, int ox, int top,
                   bool fill, bool hideLeft8, uint32_t modeKey, bool force);

}  // namespace dband
