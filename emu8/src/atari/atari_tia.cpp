#include "../../emu.h"
#include "atari.h"

// TIA video — the 2600 has no framebuffer in hardware: the program writes TIA registers in real
// time as the electron beam scans ("racing the beam"). We emulate the beam at colour-clock (dot)
// resolution and composite each visible pixel directly into a 160xATARI_FB_H indexed framebuffer.
//
// One scanline = 228 dots (68 HBLANK + 160 visible). The CPU runs at 1/3 the dot rate, so the CPU
// loop calls tiaStep(cpuCycles) to advance 3*cpuCycles dots after each instruction (instruction-
// level timing, like the NES PPU). Object position strobes (RESP/RESM/RESBL) and HMOVE are
// captured against the live beam position, so most games place sprites correctly; fine sub-
// instruction timing (the exact dot inside an instruction) is approximated.
//
// Audio register writes ($15-$1A) are forwarded to atari_audio.cpp.

namespace atari {

int colorClock = 0;                 // current dot 0..227 (exposed in atari.h for the CPU loop)
static int scanline = 0;
static int outRow   = 0;            // current framebuffer row being drawn (0..ATARI_FB_H-1)
static bool vsync   = false;
static bool vblank  = false;

// ---- registers (colours stored as palette indices = (COLUxx >> 1) & 0x7F) ----
static uint8_t colup0 = 0, colup1 = 0, colupf = 0, colubk = 0;
static uint8_t ctrlpf = 0;          // bit0 reflect, bit1 score, bit2 PF priority, bits4-5 ball size
static uint8_t pf0 = 0, pf1 = 0, pf2 = 0;
static uint32_t pf20 = 0;           // 20-bit playfield pattern, bit0 = leftmost

// players
static uint8_t grp0New = 0, grp0Old = 0, grp1New = 0, grp1Old = 0;
static int     playerX[2] = {0, 0};
static uint8_t nusiz0 = 0, nusiz1 = 0;
static bool    refp0 = false, refp1 = false;
static bool    vdelp0 = false, vdelp1 = false;
// missiles
static int     missileX[2] = {0, 0};
static bool    enam0 = false, enam1 = false;
static bool    resmp0 = false, resmp1 = false;
// ball
static int     ballX = 0;
static bool    enablNew = false, enablOld = false, vdelbl = false;
// horizontal motion (signed -8..+7)
static int8_t  hmp0 = 0, hmp1 = 0, hmm0 = 0, hmm1 = 0, hmbl = 0;
// collision latches (read as CXM0P..CXPPMM)
static uint8_t collision[8] = {0};

// NUSIZ copy layout: number of copies + their dot offsets (modes 5/7 are single double/quad width).
static const int8_t copyCount[8] = {1, 2, 2, 3, 2, 1, 3, 1};
static const int    copyOff[8][3] = {
  {0, 0, 0}, {0, 16, 0}, {0, 32, 0}, {0, 16, 32},
  {0, 64, 0}, {0, 0, 0}, {0, 32, 64}, {0, 0, 0} };

static inline __attribute__((always_inline)) int mod160(int v) { v %= 160; return v < 0 ? v + 160 : v; }

// Object position from a RESP/RESM/RESBL strobe. The object's first pixel is NOT where the beam is
// when the strobe latches: empirically (Bumbershoot's hardware positioning test) the visible pixel
// is 3*N-55 for a missile / 3*N-54 for a player, where N = CPU cycles from WSYNC to the write. Our
// instruction-level CPU samples the beam at the END of the store (colorClock = 3*N), so the object
// lands at colorClock-55 (missile/ball) or colorClock-54 (player) = a constant +13/+14 vs the naive
// colorClock-68. The position counter runs continuously and wraps at 160 (an early-HBLANK strobe
// wraps to the right edge — real behaviour), so no left-edge clamp. RESP_DELAY is the player value.
static const int RESP_DELAY = 5;
static inline int respPos(int delay) {
  return mod160(colorClock - 68 + delay);
}

static void rebuildPf() {
  uint32_t p = 0;
  for (int i = 0; i < 4; i++) if (pf0 & (1 << (4 + i))) p |= (1u << i);          // PF0 bits 4..7
  for (int i = 0; i < 8; i++) if (pf1 & (1 << (7 - i))) p |= (1u << (4 + i));    // PF1 bits 7..0
  for (int i = 0; i < 8; i++) if (pf2 & (1 << i))       p |= (1u << (12 + i));   // PF2 bits 0..7
  pf20 = p;
}

// Object-coverage bitmap for the current scanline, built ONE SPAN at a time. Each flushSpan rebuilds
// exactly its own [spanX,toPx) slice (markLo..markHi) from the registers live while the beam crosses
// it — so mid-line RESP/HMOVE/GRP moves stay pixel-accurate AND each line's coverage is built only
// once (no redundant whole-tail rebuild on every register write, the old hot spot). pfLine caches the
// playfield row (PF rarely changes mid-line), copied into the slice; objects OR over it, clipped.
enum { O_P0 = 1, O_P1 = 2, O_M0 = 4, O_M1 = 8, O_BL = 16, O_PF = 32 };
static uint8_t objLine[160];
static uint8_t pfLine[160];   // cached playfield-only coverage; rebuilt only on PF/CTRLPF change
static bool pfDirty = true;
static int spanX = 0;   // next pixel of the current line not yet rendered (span-flush renderer)
static int markLo = 0, markHi = 160;   // clip window for the slice currently being built

static inline void markPlayer(int p, uint8_t g, uint8_t bit) {
  int nusiz = p ? nusiz1 : nusiz0, mode = nusiz & 7;
  int scale = (mode == 5) ? 2 : (mode == 7) ? 4 : 1;
  bool refl = p ? refp1 : refp0;
  for (int c = 0; c < copyCount[mode]; c++) {
    int base = playerX[p] + copyOff[mode][c];
    for (int b = 0; b < 8; b++) {
      if (!(refl ? ((g >> b) & 1) : ((g >> (7 - b)) & 1))) continue;
      int x0 = base + b * scale;
      for (int s = 0; s < scale; s++) { int x = x0 + s; if (x >= markLo && x < markHi) objLine[x] |= bit; }
    }
  }
}
static inline void markSpan(int x0, int w, uint8_t bit) {
  int a = x0 < markLo ? markLo : x0, b = x0 + w; if (b > markHi) b = markHi;
  for (int x = a; x < b; x++) objLine[x] |= bit;
}

// Build objLine over [lo,hi) (the span about to be rendered) from the live registers.
static void buildCoverage(int lo, int hi) {
  if (pfDirty) {   // rebuild the cached playfield row only when PF0/1/2 or the CTRLPF reflect changed
    for (int cell = 0; cell < 40; cell++) {
      int pfidx = (cell < 20) ? cell : ((ctrlpf & 0x01) ? (39 - cell) : (cell - 20));
      uint8_t v = ((pf20 >> pfidx) & 1) ? O_PF : 0;
      uint8_t *p = pfLine + cell * 4;
      p[0] = v; p[1] = v; p[2] = v; p[3] = v;
    }
    pfDirty = false;
  }
  markLo = lo; markHi = hi;
  memcpy(objLine + lo, pfLine + lo, hi - lo);            // playfield base for this slice
  if (vdelbl ? enablOld : enablNew) markSpan(ballX, 1 << ((ctrlpf >> 4) & 3), O_BL);
  uint8_t dg0 = vdelp0 ? grp0Old : grp0New;
  uint8_t dg1 = vdelp1 ? grp1Old : grp1New;
  if (dg0) markPlayer(0, dg0, O_P0);
  if (dg1) markPlayer(1, dg1, O_P1);
  for (int m = 0; m < 2; m++) {
    bool en = m ? (enam1 && !resmp1) : (enam0 && !resmp0);
    if (!en) continue;
    int nusiz = m ? nusiz1 : nusiz0, mode = nusiz & 7, w = 1 << ((nusiz >> 4) & 3);
    uint8_t bit = m ? O_M1 : O_M0;
    for (int c = 0; c < copyCount[mode]; c++) markSpan(missileX[m] + copyOff[mode][c], w, bit);
  }
}

// Composite one visible pixel from the cached coverage. Colours are read live (per-dot). `line` is
// the framebuffer row base (hoisted out of the per-pixel loop). flushSpan bulk-fills the all-common
// background / playfield-only runs and only calls this for pixels that actually carry an object.
static inline __attribute__((always_inline)) void compositePixel(uint8_t *line, int px) {
  uint8_t o = objLine[px];
  uint8_t *fbp = line + px;
  // flushSpan only calls this for pixels carrying an object (o != 0 and o != O_PF — those runs are
  // memset there). Fast-path a lone object over background: a single coverage bit means no overlap
  // (no collision) and no playfield (no priority), so the colour is immediate.
  switch (o) {
    case O_P0: case O_M0: *fbp = colup0; return;   // player/missile 0 share COLUP0
    case O_P1: case O_M1: *fbp = colup1; return;   // player/missile 1 share COLUP1
    case O_BL:            *fbp = colupf; return;    // ball uses COLUPF
  }
  bool score = ctrlpf & 0x02;
  bool p0 = o & O_P0, p1 = o & O_P1, m0 = o & O_M0, m1 = o & O_M1, bl = o & O_BL, pf = o & O_PF;

  if (o & (o - 1)) {                 // 2+ objects overlap -> collision latches
    if (m0 && p1) collision[0] |= 0x80;
    if (m0 && p0) collision[0] |= 0x40;
    if (m1 && p0) collision[1] |= 0x80;
    if (m1 && p1) collision[1] |= 0x40;
    if (p0 && pf) collision[2] |= 0x80;
    if (p0 && bl) collision[2] |= 0x40;
    if (p1 && pf) collision[3] |= 0x80;
    if (p1 && bl) collision[3] |= 0x40;
    if (m0 && pf) collision[4] |= 0x80;
    if (m0 && bl) collision[4] |= 0x40;
    if (m1 && pf) collision[5] |= 0x80;
    if (m1 && bl) collision[5] |= 0x40;
    if (bl && pf) collision[6] |= 0x80;
    if (p0 && p1) collision[7] |= 0x80;
    if (m0 && m1) collision[7] |= 0x40;
  }

  uint8_t pfColour = score ? (px < 80 ? colup0 : colup1) : colupf;   // ball always uses COLUPF
  bool obj0 = p0 || m0, obj1 = p1 || m1, pfbl = pf || bl;
  uint8_t c;
  if (ctrlpf & 0x04) {
    if (pfbl)      c = bl ? colupf : pfColour;
    else if (obj0) c = colup0;
    else if (obj1) c = colup1;
    else           c = colubk;
  } else {
    if (obj0)      c = colup0;
    else if (obj1) c = colup1;
    else if (pfbl) c = bl ? colupf : pfColour;
    else           c = colubk;
  }
  *fbp = c;
}

// Render pending pixels [spanX, toPx) of the current visible line in one batch, rebuilding coverage
// first if a register changed. Called on each coverage-register write (up to the current beam) and
// at line end — so each span is composited with the registers valid while the beam crossed it
// (per-dot accurate) but in tight batches (per-scanline fast).
static void flushSpan(int toPx) {
  if (toPx <= spanX) return;
  if (!vblank && !vsync && outRow < ATARI_FB_H) {
    buildCoverage(spanX, toPx);   // build exactly this span's coverage (clipped) from live registers
    uint8_t *line = framebuffer + outRow * 160;
    bool score = ctrlpf & 0x02;
    int px = spanX;
    while (px < toPx) {
      uint8_t o = objLine[px];
      if (o == 0) {                                  // background run -> one memset (the common case)
        int s = px; do { px++; } while (px < toPx && objLine[px] == 0);
        memset(line + s, colubk, px - s);
      } else if (o == O_PF) {                         // solid-playfield run (e.g. the ground)
        int s = px; do { px++; } while (px < toPx && objLine[px] == O_PF);
        if (!score) memset(line + s, colupf, px - s);
        else for (int i = s; i < px; i++) line[i] = (i < 80) ? colup0 : colup1;
      } else {                                        // a pixel with object coverage -> full composite
        compositePixel(line, px); px++;
      }
    }
  }
  spanX = toPx;
}

// Finish the current scanline and start the next — called when the beam crosses dot 228. Exported
// (not static) so the CPU loop can inline the common no-wrap step via tiaStepInline() in atari.h.
void tiaLineWrap() {
  if (!vblank && !vsync && outRow < ATARI_FB_H) { flushSpan(160); outRow++; }   // finish the line
  spanX = 0;
  scanline++;
}

// Advance the beam in ONE add per instruction instead of a per-dot loop: rendering is deferred to
// flushSpan, so the step only needs the counter + the (at most one — an instruction is <=21 dots,
// far under the 228/line) line wrap. tiaStepInline (atari.h) inlines the no-wrap case into cpuLoop.
void tiaStep(int cpuCycles) {
  colorClock += cpuCycles * 3;
  if (colorClock >= 228) { colorClock -= 228; tiaLineWrap(); }
}

int tiaTickToLineEnd() {                 // WSYNC: finish this line, advance to the next line start
  if (colorClock == 0) return 0;         // already at a line start (matches the old loop's no-op)
  int dots = 228 - colorClock;
  tiaLineWrap();
  colorClock = 0;
  return dots / 3;
}

void tiaWrite(uint8_t reg, uint8_t val) {
  // A coverage-changing write (anything but the live-read colour $06-$09 / audio $15-$1A regs):
  // first render the span the beam has already crossed using the OLD coverage, THEN apply the change —
  // so mid-line RESP/HMOVE repositioning only affects pixels drawn after it. The next flushSpan rebuilds
  // its slice from the new registers (each span is built fresh, so there's no dirty flag to set).
  bool cov = !((reg >= 0x06 && reg <= 0x09) || (reg >= 0x15 && reg <= 0x1A) ||
               (reg >= 0x20 && reg <= 0x24) || reg == 0x2B || reg == 0x2C);  // colour/audio/HM-motion
  if (cov) { int px = colorClock - 68; if (px < 0) px = 0; else if (px > 160) px = 160; flushSpan(px); }

  switch (reg) {
    case 0x00: {                         // VSYNC
      bool n = val & 0x02;
      if (n && !vsync) {                    // entering vsync: the field just finished
        atariFrameCount++;
        // Blank the rows this field did not reach, so a shorter field never shows stale lines
        // from a longer one below it.
        if (outRow < ATARI_FB_H) memset(framebuffer + outRow * 160, 0, (ATARI_FB_H - outRow) * 160);
        if (!frontBuf) frontRows = outRow;  // live-buffer fallback: still centre on the field height
        // Hand the finished field to the render task. It pushes to the panel far slower than the
        // beam redraws (esp. the PicoCalc's SPI LCD), so reading the live buffer tore scrolling
        // games (River Raid) into rows from two fields. Busy renderer -> this field is dropped.
        if (frontBuf && frontState == 0) {
          memcpy(frontBuf, framebuffer, 160 * ATARI_FB_H);
          frontRows = outRow;
          __sync_synchronize();
          frontState = 1;
        }
      }
      if (!n && vsync) outRow = 0;          // leaving vsync: top of the next field
      vsync = n;
      break;
    }
    case 0x01: vblank = val & 0x02; break;  // VBLANK
    case 0x02: wsyncStall = true; break;    // WSYNC (CPU halts until line end)
    case 0x03: colorClock = 0; break;       // RSYNC (reset horizontal counter)
    case 0x04: nusiz0 = val; break;
    case 0x05: nusiz1 = val; break;
    case 0x06: colup0 = (val >> 1) & 0x7F; break;
    case 0x07: colup1 = (val >> 1) & 0x7F; break;
    case 0x08: colupf = (val >> 1) & 0x7F; break;
    case 0x09: colubk = (val >> 1) & 0x7F; break;
    case 0x0A: ctrlpf = val; pfDirty = true; break;   // reflect bit affects the cached playfield row
    case 0x0B: refp0 = val & 0x08; break;
    case 0x0C: refp1 = val & 0x08; break;
    case 0x0D: pf0 = val; rebuildPf(); pfDirty = true; break;
    case 0x0E: pf1 = val; rebuildPf(); pfDirty = true; break;
    case 0x0F: pf2 = val; rebuildPf(); pfDirty = true; break;
    case 0x10: playerX[0]  = respPos(RESP_DELAY);     break;   // RESP0
    case 0x11: playerX[1]  = respPos(RESP_DELAY);     break;   // RESP1
    case 0x12: missileX[0] = respPos(RESP_DELAY - 1); break;   // RESM0 (missile/ball start 1 px earlier)
    case 0x13: missileX[1] = respPos(RESP_DELAY - 1); break;   // RESM1
    case 0x14: ballX       = respPos(RESP_DELAY - 1); break;   // RESBL
    case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: audioWrite(reg, val); break;                // AUDx
    case 0x1B: grp0New = val; grp1Old = grp1New; break;                          // GRP0 (+VDELP1 latch)
    case 0x1C: grp1New = val; grp0Old = grp0New; enablOld = enablNew; break;     // GRP1 (+VDELP0/BL latch)
    case 0x1D: enam0 = val & 0x02; break;
    case 0x1E: enam1 = val & 0x02; break;
    case 0x1F: enablNew = val & 0x02; break;
    case 0x20: hmp0 = (int8_t)(val & 0xF0) >> 4; break;
    case 0x21: hmp1 = (int8_t)(val & 0xF0) >> 4; break;
    case 0x22: hmm0 = (int8_t)(val & 0xF0) >> 4; break;
    case 0x23: hmm1 = (int8_t)(val & 0xF0) >> 4; break;
    case 0x24: hmbl = (int8_t)(val & 0xF0) >> 4; break;
    case 0x25: vdelp0 = val & 0x01; break;
    case 0x26: vdelp1 = val & 0x01; break;
    case 0x27: vdelbl = val & 0x01; break;
    case 0x28: resmp0 = val & 0x02; if (resmp0) missileX[0] = mod160(playerX[0] + 3); break;
    case 0x29: resmp1 = val & 0x02; if (resmp1) missileX[1] = mod160(playerX[1] + 3); break;
    case 0x2A:                           // HMOVE: apply the signed motion to every object
      playerX[0]  = mod160(playerX[0]  - hmp0);
      playerX[1]  = mod160(playerX[1]  - hmp1);
      missileX[0] = mod160(missileX[0] - hmm0);
      missileX[1] = mod160(missileX[1] - hmm1);
      ballX       = mod160(ballX       - hmbl);
      break;
    case 0x2B: hmp0 = hmp1 = hmm0 = hmm1 = hmbl = 0; break;                       // HMCLR
    case 0x2C: for (int i = 0; i < 8; i++) collision[i] = 0; break;               // CXCLR
    default: break;
  }
}

uint8_t tiaRead(uint8_t reg) {
  if (reg < 8) return collision[reg];          // CXM0P..CXPPMM (bits 6/7)
  switch (reg) {
    case 0x08: case 0x09: case 0x0A: case 0x0B: return 0x00;   // INPT0-3 paddles (not connected)
    case 0x0C: return inpt4;                                   // P0 fire
    case 0x0D: return inpt5;                                   // P1 fire
  }
  return 0;
}

void tiaReset() {
  colorClock = 0; scanline = 0; outRow = 0;
  vsync = vblank = false;
  colup0 = colup1 = colupf = colubk = 0;
  ctrlpf = pf0 = pf1 = pf2 = 0; pf20 = 0;
  grp0New = grp0Old = grp1New = grp1Old = 0;
  playerX[0] = playerX[1] = 0; nusiz0 = nusiz1 = 0;
  refp0 = refp1 = vdelp0 = vdelp1 = false;
  missileX[0] = missileX[1] = 0; enam0 = enam1 = resmp0 = resmp1 = false;
  ballX = 0; enablNew = enablOld = vdelbl = false;
  hmp0 = hmp1 = hmm0 = hmm1 = hmbl = 0;
  for (int i = 0; i < 8; i++) collision[i] = 0;
  wsyncStall = false;
  pfDirty = true; spanX = 0;
  if (framebuffer) memset(framebuffer, 0, 160 * ATARI_FB_H);
}

} // namespace atari
