#include "../../emu.h"
#if defined(BOARD_DESKTOP)
#include "../desktop/debug_bridge.h"   // dbgVicMarkFrame: tag VIC DMA reads in the heat map (no-op on device)
#endif
#include "c64.h"
#include "../shared/snapshot.h"

// VIC-II renderer ported from C64Esp32 VIC.ino: renders each scanline into the RGB565
// `bitmap` framebuffer. All char/bitmap modes + sprites + collisions. Wrapped in
// namespace c64 so it doesn't collide with the Apple II core.

namespace c64 {

uint8_t startbyte;
bool badlinecond;
bool vertborder;
uint8_t lineC64map;

volatile uint32_t c64CollLines = 0;   // lines vicCollideLine checked for the collision registers

// Set by read8 the first time the program reads a collision register ($D01E/$D01F); cleared on
// reset. Until then, and while the collision IRQs ($D01A bits 1-2) are off, nothing can observe a
// collision, so the emulator core does not have to work them out.
bool vicCollPolled = false;

// The emulator core draws nothing (see vicCollideLine for the collision registers); only the render
// core includes the renderer (c64_vic_draw.h, below).
#define VIC_SCREENMAP (ram + screenmemstart)
#define VIC_SPRPTR(nr) ram[screenmemstart + 1016 + (nr)]


void initVarsAndRegs() {
  for (uint8_t i = 0; i < 0x40; i++) {
    vicreg[i] = 0;
  }
  vicreg[0x11] = 0x1b;
  vicreg[0x16] = 0xc8;
  vicreg[0x18] = 0x15;
  vicreg[0x19] = 0x71;
  vicreg[0x1a] = 0xf0;

  cntRefreshs = 0;
  syncd020 = 0;
  vicmem = 0;
  bitmapstart = 0x2000;
  screenmemstart = 1024;
  cntRefreshs = 0;
  rasterline = 0;
  vicCollPolled = false;   // a new program has not polled the collision registers yet
  charset = chrom;
  vertborder = true;
}

void vicSetup(uint8_t *ramUnused, const uint8_t *charrom) {
  (void)ramUnused;
  static bool done = false;
  if (done) return;                     // init only once
  done = true;

  chrom = charrom;
  // The line records live in the shared static 64K buffer (also the Apple main RAM; platforms are
  // mutually exclusive), after the render task's strips. Static -> always available.
  vicRec = (VicLineRec *)(sharedBigBuf + VIC_REC_OFFSET);
  memset(vicRec, 0, 400 * sizeof(VicLineRec));
  vicSnap = sharedBigBuf + VIC_SNAP_OFFSET;
  bitmap = vicLineBuf;                   // non-null = graphics on

  if (!colormap) colormap = (uint8_t *)malloc(1024);   // color RAM ($D800)
  initVarsAndRegs();
}

void checkFrameColor() {
  uint8_t framecol = vicreg[0x20] & 15;
  if (framecol != syncd020) {
    syncd020 = framecol;
    drawFrame(framecol);   // 8-bit colour index, as the framebuffer holds
  }
}

// VIC IRQ line: asserted (bit7 of $D019) when an ENABLED raster/sprite IRQ is latched
// (nextRasterline / the sprite-collision code set it). Level-triggered and masked by the
// CPU I-flag, exactly like the CIA1 IRQ. Games/cartridges drive their music + raster effects
// from this; the CPU loop must deliver it or all raster-IRQ audio stays silent. Acked by a
// write to $D019 (which clears vicreg[0x19]).
bool vicIRQPending() { return (vicreg[0x19] & 0x80) != 0; }


C64_HOT uint8_t nextRasterline() {
  bool rsel = vicreg[0x11] & 8;
  rasterline++;
  if (rasterline > 311) {
    rasterline = 0;
#if defined(BOARD_DESKTOP)
    dbgVicMarkFrame();   // once per frame: tag the VIC's DMA-read regions in the memory heat map
#endif
  } else if (rasterline == 49) {
    if (vicreg[0x11] & 0x10) {
      badlinecond = true;
    } else {
      badlinecond = false;
    }
    badlinecond |= badlinecond0;
  } else if (rasterline == 51) {
    // The vertical border only opens if the display was enabled (DEN, latched as badlinecond at
    // line 49). With DEN clear the whole screen stays border colour -- games blank it this way
    // while they load or rebuild it, and it used to show whatever was in screen RAM.
    if (rsel && badlinecond) {
      vertborder = false;
    }
    lineC64map = 0;
  } else if ((rasterline == 55) && (!rsel) && badlinecond) {
    vertborder = false;
  } else if ((rasterline == 247) && (!rsel)) {
    vertborder = true;
  } else if (rasterline == 251) {
    if (vicreg[0x11] & 0x10) {
      screenblank = false;
    } else {
      screenblank = true;
    }
    if (rsel) {
      vertborder = true;
    }
  }
  uint8_t raster8 = (rasterline >= 256) ? 0x80 : 0;
  uint8_t raster7 = (rasterline & 0xff);
  vicreg[0x12] = raster7;
  if ((latchd012 == raster7) && ((latchd011 & 0x80) == raster8)) {
    if (vicreg[0x1a] & 1) {
      vicreg[0x19] |= 0x81;
    } else {
      vicreg[0x19] |= 0x01;
    }
  }
  // calculate cycles used by VIC
  uint8_t viccycles = 0;
  // badline? Only on lines $30-$F7: the 8-bit raster7 wraps past 255, so test the full line number
  // (lines 304-311 were stealing cycles in the vertical blank).
  if (((vicreg[0x11] & 7) == (raster7 & 7)) && badlinecond &&
      (rasterline >= 0x30) && (rasterline <= 0xf7)) {
    viccycles = 40;
  }
  // active sprites?
  uint8_t numofsprites = 0;
  uint8_t spritesenabled = vicreg[0x15];
  uint8_t spritesdoubley = vicreg[0x17];
  uint8_t bitval = 128;
  for (int8_t nr = 7; nr >= 0; nr--) {
    if (spritesenabled & bitval) {
      uint8_t facysize = (spritesdoubley & bitval) ? 2 : 1;
      uint16_t y = vicreg[0x01 + nr * 2];
      if ((rasterline >= y) && (rasterline < (y + 21 * facysize))) {
        numofsprites++;
      }
    }
    bitval >>= 1;
  }
  if (numofsprites > 0) {
    viccycles += numofsprites * 2;
  }
  return viccycles;
}

// ---- Video: the emulator core records, the render core draws ----
// The emulator core draws no pixels. For each visible line (raster 51..250) it records the VIC
// state the renderer needs -- registers $D000-$D02F, the vertical border, the character-row
// counter and the memory pointers -- in vicRec[]. The render task on the other core draws every
// line from those records just behind the beam, reading the C64 RAM live, and streams it to the
// panel (c64.cpp). Every frame is drawn, at the full 50 Hz, and drawing costs the 6510 nothing.
// Before this the emulator core drew one frame in three: a smooth scroller moved in uneven
// 3-pixel jumps, and sprites a game shows on alternate frames blinked or vanished.
// A record carries the number of the frame it belongs to, 0 while it is being written, so the
// reader can tell when a record it copied was overwritten by the next frame.
// While the program watches collisions, the emulator core works out the collision registers
// ($D01E/$D01F) for each line a sprite is on, without drawing it (vicCollideLine).
static_assert(VIC_SNAP_OFFSET + 2 * 25 * 80 <= 65536, "strips + records + rows exceed sharedBigBuf");
VicLineRec *vicRec = nullptr;
uint8_t *vicSnap = nullptr;
static uint8_t vicSnapRow = 0xFF;      // the character row last copied into vicSnap this frame
volatile uint8_t vicRecFrame = 0;
uint8_t vicLineBuf[320 + 16];          // unused by the emulator core now: `bitmap` pointing here = graphics on

#if defined(BOARD_PICOCALC)
#define VIC_DMB() __asm volatile("dmb" ::: "memory")
#else
#define VIC_DMB() __sync_synchronize()
#endif

static void vicRecordLine(uint8_t dline) {
  VicLineRec *r = vicRecFor(vicRecFrame, dline);
  r->frame = 0;
  VIC_DMB();
  memcpy(r->reg, vicreg, sizeof(r->reg));
  r->lineMap = lineC64map;
  r->vertborder = vertborder;
  r->screenmem = screenmemstart;
  r->bitmapstart = bitmapstart;
  r->vicmem = vicmem;
  r->charset = charset;
  memcpy(r->sprPtr, ram + ((screenmemstart + 1016) & 0xFFFF), 8);
  // Copy the character row when the beam enters it, as the VIC fetches it on the bad line: the
  // render core trails the beam and must not see what the program writes there afterwards.
  uint8_t snap = 0xFF;
  if (!vertborder) {
    const uint8_t corr = (uint8_t)(lineC64map - (int8_t)((vicreg[0x11] & 7) - 3));
    if ((corr >> 3) < 25) {
      snap = corr >> 3;
      if (snap != vicSnapRow) {
        uint8_t *d = vicSnapFor(vicRecFrame, snap);
        memcpy(d, ram + ((screenmemstart + snap * 40) & 0xFFFF), 40);
        memcpy(d + 40, colormap + snap * 40, 40);
        vicSnapRow = snap;
      }
    }
  }
  r->snapRow = snap;
  VIC_DMB();
  r->frame = vicRecFrame;
}

// Is background pixel q (0 = the first pixel of the first character column, before the X scroll)
// foreground on this line? Multicolour 00 and 01 are background, as on the chip.
static C64_HOT bool vicBgFg(uint16_t q, uint8_t row, uint16_t base) {
  const uint8_t d011 = vicreg[0x11], d016 = vicreg[0x16];
  const uint16_t c = base + (q >> 3);
  const uint8_t bit = q & 7;
  uint8_t data;
  bool mc;
  if (d011 & 0x20) {                                   // bitmap
    data = ram[(bitmapstart + c * 8 + row) & 0xFFFF];
    mc = d016 & 0x10;
  } else {
    const uint8_t ch = VIC_SCREENMAP[c];
    data = charset[((d011 & 0x40) ? (ch & 0x3f) : ch) * 8 + row];
    mc = (d016 & 0x10) && (colormap[c] & 8);
  }
  if (mc) return (data >> (6 - (bit & 6))) & 2;
  return (data >> (7 - bit)) & 1;
}

// The collision registers for raster line `rasterline`, without drawing it: what the chip sets in
// $D01E (sprite-sprite) and $D01F (sprite-background), and the IRQ the first hit raises. The
// emulator core used to render the whole line (background and sprites, into a scratch buffer) for
// this, on every line a sprite is on while the program polls collisions -- dozens of lines a frame
// in a sprite game, taken from the 6510's time.
static C64_HOT void vicCollideLine() {
  if (vertborder) return;                              // border line: no sprite is drawn
  const uint16_t line = rasterline - 1;                // the renderer's sprite-line numbering
  int16_t sx[8];
  uint64_t sm[8];                                      // bit k = sprite pixel sx + k is set
  uint8_t sbit[8], n = 0;
  const uint8_t en = vicreg[0x15];
  for (uint8_t nr = 0; nr < 8; nr++) {
    const uint8_t b = 1 << nr;
    if (!(en & b)) continue;
    const uint16_t y = vicreg[0x01 + nr * 2];
    const uint8_t fac = (vicreg[0x17] & b) ? 2 : 1;
    if (line < y || line >= y + 21 * fac) continue;
    const uint8_t *p = ram + ((vicmem + VIC_SPRPTR(nr) * 64 + ((line - y) / fac) * 3) & 0xFFFF);
    uint32_t d = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    if (!d) continue;
    if (vicreg[0x1c] & b) d |= ((d & 0xAAAAAA) >> 1) | ((d & 0x555555) << 1);   // a pair is 2 pixels
    const bool wide = vicreg[0x1d] & b;
    uint64_t m = 0;
    for (uint8_t k = 0; k < 24; k++)
      if (d & (0x800000u >> k)) m |= wide ? (3ull << (2 * k)) : (1ull << k);
    sx[n] = vicreg[nr * 2] + ((vicreg[0x10] & b) ? 256 : 0) - 24;
    sm[n] = m;
    sbit[n++] = b;
  }
  if (!n) return;
  c64CollLines++;

  const uint8_t coll1e = vicreg[0x1e], coll1f = vicreg[0x1f];
  for (uint8_t i = 0; i < n; i++)
    for (uint8_t j = i + 1; j < n; j++) {
      const int16_t dx = sx[j] - sx[i];
      if (dx >= 48 || dx <= -48) continue;
      if (dx >= 0 ? (sm[i] & (sm[j] << dx)) : ((sm[i] << -dx) & sm[j])) vicreg[0x1e] |= sbit[i] | sbit[j];
    }

  // Sprite-background: the line's graphics, at the sprites' pixels only. Lines the Y scroll leaves
  // empty and the invalid modes (ECM with BMM or MCM, drawn black) have no foreground.
  const uint8_t d011 = vicreg[0x11];
  const int8_t dy = (int8_t)((d011 & 7) - 3);
  const int16_t dline = rasterline - 51;
  const bool invalid = (d011 & 0x40) && ((d011 & 0x20) || (vicreg[0x16] & 0x10));
  if (!invalid && dline >= dy && dy >= dline - 199) {
    const uint8_t corr = (uint8_t)(lineC64map - dy);
    const uint8_t row = corr & 7;
    const uint16_t base = (corr >> 3) * 40;
    const uint8_t xs = vicreg[0x16] & 7;
    for (uint8_t i = 0; i < n; i++) {
      if (vicreg[0x1f] & sbit[i]) continue;
      for (uint64_t m = sm[i]; m; m &= m - 1) {
        const int16_t q = sx[i] + __builtin_ctzll(m) - xs;
        if (q < 0) continue;
        if (q >= 320 - xs) break;
        if (vicBgFg(q, row, base)) { vicreg[0x1f] |= sbit[i]; break; }
      }
    }
  }

  if (vicreg[0x1f] != 0 && coll1f == 0) vicreg[0x19] |= (vicreg[0x1a] & 2) ? 0x82 : 0x02;
  if (vicreg[0x1e] != 0 && coll1e == 0) vicreg[0x19] |= (vicreg[0x1a] & 4) ? 0x84 : 0x04;
}

void drawRasterline() {
  if ((rasterline >= 51) && (rasterline < 251)) {
    if (rasterline == 51) {
      // 1..254, then 1 again: 0 marks a record being written, and 254 -> 1 keeps the banks
      // (frame & 1) alternating.
      vicRecFrame = vicRecFrame >= 254 ? 1 : vicRecFrame + 1;
      vicSnapRow = 0xFF;
    }
    vicRecordLine(rasterline - 51);
    if ((vicCollPolled || (vicreg[0x1a] & 0x06)) && vicreg[0x15]) vicCollideLine();
    lineC64map++;
  }
}

// ---- The render core's copy of the renderer ----
namespace rend {
uint8_t vicreg[0x40];
uint8_t *bitmap = nullptr;
uint8_t spritespritecoll[320 + 16];
bool spritedatacoll[320 + 16];
static bool vicColl = false;
const uint8_t *charset = nullptr;
uint16_t rasterline = 0, screenmemstart = 0, bitmapstart = 0, vicmem = 0;
uint8_t lineC64map = 0;
bool vertborder = true;
uint8_t *colormap = nullptr;           // the line's copied colour row, or the colour RAM
uint8_t *screenMap = nullptr;          // the line's copied screen row, or the screen matrix
const uint8_t *sprPtr = nullptr;       // the line's recorded sprite pointers
#undef VIC_SCREENMAP
#undef VIC_SPRPTR
#define VIC_SCREENMAP screenMap
#define VIC_SPRPTR(nr) sprPtr[nr]
#undef VIC_DRAW_FN
#define VIC_DRAW_FN C64_HOT   // the render core draws every line: from SRAM, clear of the XIP cache
#include "c64_vic_draw.h"
}  // namespace rend


// Draw one line from its record into `out` (320 colour indices) and convert it through `pal` into
// `dst` (320 RGB565 pixels). From SRAM: the render core does this 10,000 times a second.
C64_HOT void vicRenderRecord(const VicLineRec &r, uint8_t dline, uint8_t *out, uint16_t *dst,
                             const uint16_t *pal) {
  memcpy(rend::vicreg, r.reg, sizeof(r.reg));
  rend::lineC64map = r.lineMap;
  rend::vertborder = r.vertborder;
  rend::screenmemstart = r.screenmem;
  rend::bitmapstart = r.bitmapstart;
  rend::vicmem = r.vicmem;
  rend::charset = r.charset;
  rend::sprPtr = r.sprPtr;
  if (r.snapRow < 25) {                // indexed by row * 40 + column: point row * 40 before the copy
    uint8_t *snap = vicSnapFor(r.frame, r.snapRow) - r.snapRow * 40;
    rend::screenMap = snap;
    rend::colormap = snap + 40;
  } else {
    rend::screenMap = ram + r.screenmem;
    rend::colormap = colormap;
  }
  rend::rasterline = dline + 51;
  rend::bitmap = out;
  rend::vicColl = rend::vicSpriteOnLine(rend::rasterline - 1);
  rend::vicDrawLineBody();
  if (dst) vicLineToRgb(out, dst, pal);
}

C64_HOT void vicLineToRgb(const uint8_t *out, uint16_t *dst, const uint16_t *pal) {
  for (int x = 0; x < 320; x += 4) {
    dst[x] = pal[out[x] & 15];
    dst[x + 1] = pal[out[x + 1] & 15];
    dst[x + 2] = pal[out[x + 2] & 15];
    dst[x + 3] = pal[out[x + 3] & 15];
  }
}

// Snapshot (c64_snapshot.cpp): the registers and the raster/border/badline state the next lines
// depend on, so a frame saved mid-way carries on where it was. The base addresses and the charset
// pointer are rebuilt from $D018 + the bank, as a register write does. On load the line records
// are dropped: the frame being recorded started on the old machine, so the render core skips it
// and shows the next whole one.
void vicSnapshot(SnapIO &io) {
  io.arr(vicreg);
  io.val(latchd011); io.val(latchd012);
  io.val(vicmem); io.val(rasterline);
  io.val(syncd020); io.val(cntRefreshs);
  io.flag(screenblank); io.flag(badlinecond0); io.flag(badlinecond);
  io.flag(vertborder); io.flag(vicCollPolled);
  io.val(lineC64map); io.val(startbyte);
  if (!io.loading || !io.ok) return;
  if (rasterline > 311) rasterline = 0;
  adaptVICBaseAddrs(false);
  if (vicRec)
    for (int i = 0; i < 400; i++) vicRec[i].frame = 0;
  vicSnapRow = 0xFF;
}

// End marker of the C64's SRAM code on the PicoCalc: the Apple II borrows the idle run of it up to
// here (a2LendC64Code, src/apple2/memory.cpp). Never called; keep it the last C64_HOT in this file.
C64_HOT void vicHotEnd() {}

} // namespace c64
