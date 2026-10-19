// This MSX translation unit is compiled out on boards without the MSX core or without MSX2 support.
// emu.h must be included FIRST because it is what pulls in board.h and defines the macros.
#include "../../emu.h"
#if BOARD_HAS_MSX_CORE && BOARD_HAS_MSX2

// msx_v9938.cpp - Yamaha V9938 VDP for the MSX2 mode of the MSX core. Ports $98 (VRAM data),
// $99 (address / register / status), $9A (palette), $9B (indirect register).
//
//   * 128 KB VRAM (msx::vram), registers R0-R23 + R32-R46, status S0-S9, 16-entry 9-bit palette.
//   * Screen modes TEXT1/TEXT2 (with blink), MULTICOLOR, GRAPHIC1-7 (SCREEN 0-8), 192 or 212 lines,
//     vertical scroll (R23), VBlank and line (R19) interrupts.
//   * Sprite mode 1 (TMS, 4 per line) and mode 2 (8 per line, per-line colours, CC/IC/EC bits),
//     collision and 5th/9th-sprite status.
//   * The command engine (HMMC/YMMM/HMMM/HMMV/LMMC/LMCM/LMMM/LMMV/LINE/SRCH/PSET/POINT). Commands
//     complete at once (CE never stays up), except the CPU-transfer ones, which advance one pixel /
//     byte per R44 write (S7 read for LMCM).
//
// Rendering is per scanline (v99Line, called by msx_machine.cpp at the start of every line), so
// mid-frame register changes from line interrupts (split screens) land on the right line. Lines go
// into msx::framebuffer at a 512-byte stride: narrow modes fill 256 bytes, the 512-wide ones (TEXT2,
// SCREEN 6/7) 512, and v99LineFlags tells the display which -- and whether the bytes are palette
// indices or SCREEN 8's GRB332. The display side (msx.cpp) scales that to the panel.
//
// Simplifications: VRAM is linear in SCREEN 7/8 (no planar interleave, which only shows when one
// picture is viewed in two different modes), no interlace / even-odd page flipping, no horizontal
// adjust (R18), commands take no time.

#include "msx.h"
#include <string.h>

namespace msx {

uint8_t  v99LineFlags[FB2_H];
int      v99FrameH = 192;
uint16_t v99DispPal[16];
uint16_t V99_PAL332[256];

static const uint32_t VMASK = V99_VRAM_SIZE - 1;

static uint8_t  r[64];          // R0-R46 (the rest unused)
static uint8_t  st[10];         // S0-S9 (S1/S2 composed on read)
static uint32_t vaddr;          // 17-bit VRAM pointer
static bool     latched;
static uint8_t  latch;
static uint8_t  readAhead;
static bool     palLatched;
static uint8_t  palLatch;
static uint16_t pal9[16];       // 0RRR0BBB | 00000GGG << 8
static uint16_t pal565[16];
static bool     fh;             // line-interrupt flag (S1 bit 0)
static bool     inVblank;
static uint32_t lineStart;      // cpu.cycles at the start of the current line (S2 HR)
static int      activeH = 192;
static bool     renderOn;       // this frame is drawn into the framebuffer
static int      blinkCount;
static bool     blinkAlt;       // TEXT2 blink: alternate colours showing

// ---- palette ----------------------------------------------------------------------------------
static const uint8_t DEF_PAL[16][3] = {          // V9938 power-on palette (R, G, B 0-7)
  {0,0,0},{0,0,0},{1,6,1},{3,7,3},{1,1,7},{2,3,7},{5,1,1},{2,6,7},
  {7,1,1},{7,3,3},{6,6,1},{6,6,4},{1,4,1},{6,2,5},{5,5,5},{7,7,7},
};
static inline uint16_t rgb565(int r8, int g8, int b8) {
  return (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
}
static void palUpdate(int i) {
  int R = (pal9[i] >> 4) & 7, B = pal9[i] & 7, G = (pal9[i] >> 8) & 7;
  pal565[i] = rgb565(R * 255 / 7, G * 255 / 7, B * 255 / 7);
}

// SCREEN 8 sprites use their own fixed 16 colours (9-bit GRB), stored here as GRB332 bytes.
static uint8_t G7_SPRITE[16];
static void initTables() {
  for (int c = 0; c < 256; c++) {
    int G = c >> 5, R = (c >> 2) & 7, B = c & 3;
    static const uint8_t B3[4] = {0, 2, 4, 7};
    V99_PAL332[c] = rgb565(R * 255 / 7, G * 255 / 7, B3[B] * 255 / 7);
  }
  static const uint16_t grb[16] = { 0x000, 0x002, 0x030, 0x032, 0x300, 0x302, 0x330, 0x332,
                                    0x472, 0x007, 0x070, 0x077, 0x700, 0x707, 0x770, 0x777 };
  for (int i = 0; i < 16; i++) {
    int G = (grb[i] >> 8) & 7, R = (grb[i] >> 4) & 7, B = grb[i] & 7;
    G7_SPRITE[i] = (uint8_t)((G << 5) | (R << 2) | (B >> 1));
  }
}

// ---- modes ------------------------------------------------------------------------------------
enum { M_G1 = 0x00, M_T1 = 0x01, M_MC = 0x02, M_G2 = 0x04, M_G3 = 0x08, M_T2 = 0x09,
       M_G4 = 0x0C, M_G5 = 0x10, M_G6 = 0x14, M_G7 = 0x1C };
static inline int mode() { return ((r[1] >> 4) & 1) | ((r[1] >> 2) & 2) | ((r[0] << 1) & 0x1C); }
static inline bool bitmapMode(int m) { return m == M_G4 || m == M_G5 || m == M_G6 || m == M_G7; }
static inline bool tmsMode(int m) { return m == M_G1 || m == M_T1 || m == M_MC || m == M_G2; }

void v99Reset() {
  memset(r, 0, sizeof(r));
  memset(st, 0, sizeof(st));
  vaddr = 0; latched = false; latch = 0; readAhead = 0;
  palLatched = false; palLatch = 0;
  fh = false; inVblank = false; blinkCount = 0; blinkAlt = false;
  initTables();
  for (int i = 0; i < 16; i++) {
    pal9[i] = (uint16_t)((DEF_PAL[i][0] << 4) | DEF_PAL[i][2] | (DEF_PAL[i][1] << 8));
    palUpdate(i);
  }
  memcpy(v99DispPal, pal565, sizeof(pal565));
  memset(v99LineFlags, 0, sizeof(v99LineFlags));
}

// ---- command engine ---------------------------------------------------------------------------
// One active CPU-transfer command (LMMC/HMMC/LMCM) at a time; everything else finishes inside the
// R46 write.
static struct {
  int  cmd;                     // 0 = idle
  int  op;
  int  sx, sy, dx, dy, nx, ny;  // working copy
  int  x, y, cnt, rows;         // transfer position
  int  dix, diy;
} ce;

static inline int scrW(int m) { return (m == M_G5 || m == M_G6) ? 512 : 256; }
// Bits per pixel and the pixel -> VRAM address mapping of the bitmap modes. The engine runs in the
// current mode; outside SCREEN 5-8 it behaves as SCREEN 8.
static inline int cmdMode() { int m = mode(); return bitmapMode(m) ? m : M_G7; }
static inline uint32_t pxAddr(int m, int x, int y) {
  switch (m) {
    case M_G4: return ((uint32_t)(y & 1023) << 7) | ((x & 255) >> 1);
    case M_G5: return ((uint32_t)(y & 1023) << 7) | ((x & 511) >> 2);
    case M_G6: return (((uint32_t)(y & 511) << 8) | ((x & 511) >> 1)) & VMASK;
    default:   return (((uint32_t)(y & 511) << 8) | (x & 255)) & VMASK;
  }
}
static inline int pxShift(int m, int x) {
  switch (m) {
    case M_G4: case M_G6: return (x & 1) ? 0 : 4;
    case M_G5: return (3 - (x & 3)) * 2;
    default:   return 0;
  }
}
static inline uint8_t pxMask(int m) { return m == M_G7 ? 0xFF : m == M_G5 ? 0x03 : 0x0F; }
static inline int ppb(int m) { return m == M_G7 ? 1 : m == M_G5 ? 4 : 2; }

static uint8_t point(int m, int x, int y) {
  return (uint8_t)((vram[pxAddr(m, x, y)] >> pxShift(m, x)) & pxMask(m));
}
static void pset(int m, int x, int y, uint8_t sc, int op) {
  const uint8_t mask = pxMask(m);
  sc &= mask;
  if ((op & 8) && sc == 0) return;                  // T-ops: colour 0 leaves the pixel alone
  const uint32_t a = pxAddr(m, x, y);
  const int sh = pxShift(m, x);
  uint8_t d = (uint8_t)((vram[a] >> sh) & mask), c;
  switch (op & 7) {
    case 0:  c = sc; break;                         // IMP
    case 1:  c = sc & d; break;                     // AND
    case 2:  c = sc | d; break;                     // OR
    case 3:  c = sc ^ d; break;                     // EOR
    case 4:  c = (uint8_t)(~sc & mask); break;      // NOT
    default: c = d; break;
  }
  vram[a] = (uint8_t)((vram[a] & ~(mask << sh)) | (c << sh));
}

static inline int reg9(int lo) { return r[lo] | ((r[lo + 1] & 1) << 8); }
static inline int reg10(int lo) { return r[lo] | ((r[lo + 1] & 3) << 8); }
static void putReg10(int lo, int v) { r[lo] = (uint8_t)v; r[lo + 1] = (uint8_t)((v >> 8) & 3); }

static void cmdFinish() { ce.cmd = 0; st[2] &= ~0x01; }

// LMCM: put the pixel at the transfer position into S7.
static void lmcmFetch(int m) { st[7] = point(m, ce.x, ce.y); }

// Advance a CPU-transfer command by one pixel (or byte). Returns false when it ended.
static bool xferStep(int m) {
  const int W = scrW(m);
  const int step = (ce.cmd == 0xF) ? ppb(m) : 1;    // HMMC moves in bytes
  ce.x += ce.dix * step;
  if (--ce.cnt <= 0 || ce.x < 0 || ce.x >= W) {
    ce.x = (ce.cmd == 0xA) ? ce.sx : ce.dx;
    ce.cnt = ce.nx;
    ce.y += ce.diy;
    if (--ce.rows <= 0) {
      if (ce.cmd == 0xA) putReg10(34, ce.y); else putReg10(38, ce.y);
      putReg10(42, 0);
      cmdFinish();
      return false;
    }
  }
  return true;
}

static void cmdStart() {
  const int m = cmdMode();
  const int W = scrW(m);
  const int op = r[46] & 0x0F;
  const int cmd = r[46] >> 4;
  const int arg = r[45];
  const int dix = (arg & 0x04) ? -1 : 1, diy = (arg & 0x08) ? -1 : 1;
  int sx = reg9(32), sy = reg10(34), dx = reg9(36), dy = reg10(38);
  int nx = reg9(40), ny = reg10(42);
  if (nx == 0) nx = 512;
  if (ny == 0) ny = 1024;
  ce.cmd = 0;
  st[2] &= ~0x11;                                   // CE, BD
  switch (cmd) {
    case 0x0: break;                                // STOP
    case 0x4: st[7] = point(m, sx, sy); break;      // POINT
    case 0x5: pset(m, dx, dy, r[44], op); break;    // PSET
    case 0x6: {                                     // SRCH
      const uint8_t cl = r[44] & pxMask(m);
      const bool eq = arg & 0x02;
      int x = sx;
      for (; x >= 0 && x < W; x += dix) {
        if ((point(m, x, sy) == cl) != eq) { st[2] |= 0x10; break; }
      }
      st[8] = (uint8_t)x; st[9] = (uint8_t)(0xFE | ((x >> 8) & 1));
      break;
    }
    case 0x7: {                                     // LINE
      const int maj = reg9(40), mn = reg10(42);     // NX = long side, NY = short side
      const bool yMajor = arg & 0x01;
      int x = dx, y = dy, acc = (maj - 1) >> 1;
      for (int i = 0; i <= maj; i++) {
        if (x >= 0 && x < W) pset(m, x, y, r[44], op);
        if (yMajor) y += diy; else x += dix;
        acc -= mn;
        if (acc < 0) { acc += maj; if (yMajor) x += dix; else y += diy; }
      }
      break;
    }
    case 0x8: case 0x9: case 0xC: case 0xD: {       // LMMV LMMM HMMV HMMM
      const bool high = cmd >= 0xC;
      const int step = high ? ppb(m) : 1;
      int y0 = sy, y1 = dy;
      for (int j = 0; j < ny; j++) {
        int xs = sx, xd = dx;
        for (int i = 0; i < nx; i += step) {
          if (xd < 0 || xd >= W) break;
          if (cmd == 0x8) pset(m, xd, y1, r[44], op);
          else if (cmd == 0x9) { if (xs < 0 || xs >= W) break; pset(m, xd, y1, point(m, xs, y0), op); }
          else if (cmd == 0xC) vram[pxAddr(m, xd, y1)] = r[44];
          else { if (xs < 0 || xs >= W) break; vram[pxAddr(m, xd, y1)] = vram[pxAddr(m, xs, y0)]; }
          xs += dix * step; xd += dix * step;
        }
        y0 += diy; y1 += diy;
      }
      if (cmd == 0x9 || cmd == 0xD) putReg10(34, y0);
      putReg10(38, y1);
      putReg10(42, 0);
      break;
    }
    case 0xE: {                                     // YMMM: DX to the edge, SY -> DY
      const int step = ppb(m);
      int y0 = sy, y1 = dy;
      for (int j = 0; j < ny; j++) {
        for (int x = dx; x >= 0 && x < W; x += dix * step) vram[pxAddr(m, x, y1)] = vram[pxAddr(m, x, y0)];
        y0 += diy; y1 += diy;
      }
      putReg10(34, y0); putReg10(38, y1); putReg10(42, 0);
      break;
    }
    case 0xA: case 0xB: case 0xF: {                 // LMCM LMMC HMMC: CPU transfers
      ce.cmd = cmd; ce.op = op; ce.dix = dix; ce.diy = diy;
      ce.sx = sx; ce.dx = dx; ce.nx = (cmd == 0xF) ? (nx + ppb(m) - 1) / ppb(m) : nx;
      ce.x = (cmd == 0xA) ? sx : dx; ce.y = (cmd == 0xA) ? sy : dy;
      ce.cnt = ce.nx; ce.rows = ny;
      st[2] |= 0x01;                                // CE until the last pixel
      if (cmd == 0xA) lmcmFetch(m);
      else if (cmd == 0xB) { pset(m, ce.x, ce.y, r[44], op); xferStep(m); }
      else { vram[pxAddr(m, ce.x, ce.y)] = r[44]; xferStep(m); }
      break;
    }
    default: break;
  }
}

// ---- register writes ---------------------------------------------------------------------------
static void writeReg(int n, uint8_t v) {
  if (n >= 47) return;
  r[n] = v;
  if (n == 16) palLatched = false;
  else if (n == 44 && (ce.cmd == 0xB || ce.cmd == 0xF)) {   // next pixel / byte of LMMC / HMMC
    const int m = cmdMode();
    if (ce.cmd == 0xB) pset(m, ce.x, ce.y, v, ce.op);
    else vram[pxAddr(m, ce.x, ce.y)] = v;
    xferStep(m);
  } else if (n == 46) cmdStart();
}

void v99WriteCtrl(uint8_t v) {
  if (!latched) { latch = v; latched = true; return; }
  latched = false;
  if (v & 0x80) { writeReg(v & 0x3F, latch); return; }
  vaddr = ((uint32_t)(r[14] & 7) << 14) | ((uint32_t)(v & 0x3F) << 8) | latch;
  if (!(v & 0x40)) {                                // read setup: prefetch
    readAhead = vram[vaddr];
    vaddr = (vaddr + 1) & VMASK;
  }
}

static inline void incAddr() {
  // In the TMS9918 modes the pointer wraps inside 16K; elsewhere the carry goes into R14.
  uint32_t lo = (vaddr + 1) & 0x3FFF;
  if (lo == 0 && !tmsMode(mode())) r[14] = (uint8_t)((r[14] + 1) & 7);
  vaddr = ((uint32_t)(r[14] & 7) << 14) | lo;
}
void v99WriteData(uint8_t v) {
  latched = false;
  vram[vaddr] = v; readAhead = v;
  incAddr();
}
uint8_t v99ReadData() {
  latched = false;
  uint8_t x = readAhead;
  readAhead = vram[vaddr];
  incAddr();
  return x;
}

void v99WritePalette(uint8_t v) {
  if (!palLatched) { palLatch = v; palLatched = true; return; }
  palLatched = false;
  const int i = r[16] & 15;
  pal9[i] = (uint16_t)((palLatch & 0x77) | ((v & 7) << 8));
  palUpdate(i);
  r[16] = (uint8_t)((i + 1) & 15);
}

void v99WriteIndirect(uint8_t v) {
  const int n = r[17] & 0x3F;
  if (n != 17) writeReg(n, v);
  if (!(r[17] & 0x80)) r[17] = (uint8_t)((r[17] & 0xC0) | ((n + 1) & 0x3F));
}

uint8_t v99ReadStatus() {
  latched = false;
  switch (r[15] & 0x0F) {
    case 0: { uint8_t s = st[0]; st[0] &= 0x1F; return s; }            // clears F, 5S, C
    case 1: { uint8_t s = fh ? 1 : 0; fh = false; return s; }          // V9938 ID 0 + FH
    case 2: {
      const uint32_t pos = cpu.cycles - lineStart;
      uint8_t s = 0x80 | 0x0C | (st[2] & 0x11);                        // TR (always ready), CE, BD
      if (inVblank) s |= 0x40;
      if (pos >= 182) s |= 0x20;                                       // HR: the right border onwards
      return s;
    }
    case 7: {                                                          // LMCM pixel
      uint8_t s = st[7];
      if (ce.cmd == 0xA) {
        const int m = cmdMode();
        if (xferStep(m)) lmcmFetch(m);
      }
      return s;
    }
    case 3: case 4: case 5: case 6: case 8: case 9: return st[r[15] & 0x0F];
    default: return 0xFF;
  }
}

bool v99IrqActive() { return ((st[0] & 0x80) && (r[1] & 0x20)) || (fh && (r[0] & 0x10)); }

// ---- rendering ---------------------------------------------------------------------------------
static inline uint8_t backdrop() { return r[7] & 0x0F; }
static inline uint8_t opaque(uint8_t c) { return (c || (r[8] & 0x20)) ? c : backdrop(); }

static void lineText1(uint8_t* p, int vy) {          // 40x24, 6 px chars, 8 px borders
  const uint32_t nameB = (uint32_t)(r[2] & 0x7F) << 10, patB = (uint32_t)(r[4] & 0x3F) << 11;
  const uint8_t fg = opaque(r[7] >> 4), bg = backdrop();
  const int row = vy >> 3, line = vy & 7;
  memset(p, bg, 8); memset(p + 248, bg, 8);
  for (int c = 0; c < 40; c++) {
    uint8_t ch = vram[(nameB + row * 40 + c) & VMASK];
    uint8_t pat = vram[(patB + ch * 8 + line) & VMASK];
    uint8_t* q = p + 8 + c * 6;
    for (int b = 0; b < 6; b++) q[b] = (pat & (0x80 >> b)) ? fg : bg;
  }
}

static void lineText2(uint8_t* p, int vy) {          // 80x24(26), 6 px chars in 512, 16 px borders
  const uint32_t nameB = (uint32_t)(r[2] & 0x7C) << 10, patB = (uint32_t)(r[4] & 0x3F) << 11;
  const uint32_t colB = ((uint32_t)(r[10] & 7) << 14) | ((uint32_t)(r[3] & 0xF8) << 6);
  const uint8_t fg = opaque(r[7] >> 4), bg = backdrop();
  const uint8_t bfg = opaque(r[12] >> 4), bbg = opaque(r[12] & 0x0F);
  const int row = vy >> 3, line = vy & 7;
  memset(p, bg, 16); memset(p + 496, bg, 16);
  for (int c = 0; c < 80; c++) {
    uint8_t ch = vram[(nameB + row * 80 + c) & VMASK];
    uint8_t pat = vram[(patB + ch * 8 + line) & VMASK];
    uint8_t f = fg, b = bg;
    if (blinkAlt && (vram[(colB + row * 10 + (c >> 3)) & VMASK] & (0x80 >> (c & 7)))) { f = bfg; b = bbg; }
    uint8_t* q = p + 16 + c * 6;
    for (int k = 0; k < 6; k++) q[k] = (pat & (0x80 >> k)) ? f : b;
  }
}

static void lineGraphic1(uint8_t* p, int vy) {
  const uint32_t nameB = (uint32_t)(r[2] & 0x7F) << 10, patB = (uint32_t)(r[4] & 0x3F) << 11;
  const uint32_t colB = ((uint32_t)(r[10] & 7) << 14) | ((uint32_t)r[3] << 6);
  const int row = vy >> 3, line = vy & 7;
  for (int c = 0; c < 32; c++) {
    uint8_t ch = vram[(nameB + row * 32 + c) & VMASK];
    uint8_t pat = vram[(patB + ch * 8 + line) & VMASK];
    uint8_t clr = vram[(colB + (ch >> 3)) & VMASK];
    uint8_t f = opaque(clr >> 4), b = opaque(clr & 0x0F);
    uint8_t* q = p + c * 8;
    for (int k = 0; k < 8; k++) q[k] = (pat & (0x80 >> k)) ? f : b;
  }
}

static void lineGraphic23(uint8_t* p, int vy) {      // SCREEN 2 / 4 background (masked tables)
  const uint32_t nameB = (uint32_t)(r[2] & 0x7F) << 10;
  const uint32_t patB = (uint32_t)(r[4] & 0x3C) << 11, patM = ((uint32_t)(r[4] & 3) << 11) | 0x7FF;
  const uint32_t colB = ((uint32_t)(r[10] & 7) << 14) | ((uint32_t)(r[3] & 0x80) << 6);
  const uint32_t colM = ((uint32_t)(r[3] & 0x7F) << 6) | 0x3F;
  const int row = (vy >> 3) % 32, line = vy & 7, third = (row >> 3) & 3;
  for (int c = 0; c < 32; c++) {
    uint8_t name = vram[(nameB + row * 32 + c) & VMASK];
    uint32_t idx = (uint32_t)((third * 256 + name) * 8 + line);
    uint8_t pat = vram[(patB | (idx & patM)) & VMASK];
    uint8_t clr = vram[(colB | (idx & colM)) & VMASK];
    uint8_t f = opaque(clr >> 4), b = opaque(clr & 0x0F);
    uint8_t* q = p + c * 8;
    for (int k = 0; k < 8; k++) q[k] = (pat & (0x80 >> k)) ? f : b;
  }
}

static void lineMulticolor(uint8_t* p, int vy) {
  const uint32_t nameB = (uint32_t)(r[2] & 0x7F) << 10, patB = (uint32_t)(r[4] & 0x3F) << 11;
  const int row = vy >> 3, line = vy & 7;
  for (int c = 0; c < 32; c++) {
    uint8_t name = vram[(nameB + row * 32 + c) & VMASK];
    uint8_t clr = vram[(patB + name * 8 + (row & 3) * 2 + (line >> 2)) & VMASK];
    memset(p + c * 8, opaque(clr >> 4), 4);
    memset(p + c * 8 + 4, opaque(clr & 0x0F), 4);
  }
}

static void lineBitmap(uint8_t* p, int vy, int m) {
  const int y = vy & 255;
  switch (m) {
    case M_G4: {                                    // 256 px, 4 bpp
      const uint8_t* s = vram + (((uint32_t)(r[2] & 0x60) << 10) | (y << 7));
      for (int x = 0; x < 128; x++) { p[2 * x] = opaque(s[x] >> 4); p[2 * x + 1] = opaque(s[x] & 15); }
      break;
    }
    case M_G5: {                                    // 512 px, 2 bpp; colour 0 shows R7 split per pixel
      const uint8_t* s = vram + (((uint32_t)(r[2] & 0x60) << 10) | (y << 7));
      const bool tp = r[8] & 0x20;
      const uint8_t be = (r[7] >> 2) & 3, bo = r[7] & 3;
      for (int x = 0; x < 128; x++) {
        uint8_t b = s[x];
        for (int k = 0; k < 4; k++) {
          uint8_t c = (b >> (6 - 2 * k)) & 3;
          p[4 * x + k] = (c || tp) ? c : ((k & 1) ? bo : be);
        }
      }
      break;
    }
    case M_G6: {                                    // 512 px, 4 bpp
      const uint8_t* s = vram + (((uint32_t)(r[2] & 0x20) << 11) | (y << 8));
      for (int x = 0; x < 256; x++) { p[2 * x] = opaque(s[x] >> 4); p[2 * x + 1] = opaque(s[x] & 15); }
      break;
    }
    default: {                                      // M_G7: 256 px, GRB332 bytes
      const uint8_t* s = vram + (((uint32_t)(r[2] & 0x20) << 11) | (y << 8));
      memcpy(p, s, 256);
      break;
    }
  }
}

// Sprites on one line. Mode 1 (TMS) in G1/G2/MC, mode 2 in G3-G7. The pixels are composed into
// spr[] (colour + 1, 0 = none) at 256-px resolution; the status bits are updated whether or not
// the frame is drawn.
static void lineSprites(uint8_t* p, int vy, int m, bool draw) {
  if (r[8] & 0x02) return;                          // SPD: sprites disabled
  const bool mode2 = !(m == M_G1 || m == M_G2 || m == M_MC);
  const int size = (r[1] & 0x02) ? 16 : 8, mag = (r[1] & 0x01) ? 2 : 1, dsize = size * mag;
  uint32_t sat;
  if (mode2) sat = ((uint32_t)(r[11] & 3) << 15) | ((uint32_t)(r[5] & 0xFC) << 7);
  else       sat = ((uint32_t)(r[11] & 3) << 15) | ((uint32_t)r[5] << 7);
  const uint32_t ctab = sat - 0x200;
  const uint32_t patB = (uint32_t)(r[6] & 0x3F) << 11;
  const int limit = mode2 ? 8 : 4;
  const uint8_t term = mode2 ? 216 : 208;
  const bool tp = r[8] & 0x20;

  int sel[8], row[8], nsel = 0;
  for (int i = 0; i < 32; i++) {
    const uint8_t yb = vram[(sat + i * 4) & VMASK];
    if (yb == term) break;
    const int d = (vy - yb - 1) & 0xFF;
    if (d >= dsize) continue;
    if (nsel >= limit) {
      if (!(st[0] & 0x40)) st[0] = (uint8_t)((st[0] & 0xA0) | 0x40 | i);
      break;
    }
    sel[nsel] = i; row[nsel] = d / mag; nsel++;
  }
  if (!nsel) return;

  uint8_t spr[256 + 32];                            // colour + 1 per pixel, 0 = no sprite
  uint8_t own[256 + 32];                            // pixel set by a sprite that collides (IC = 0)
  memset(spr, 0, 256); memset(own, 0, 256);
  bool anchor = false;                              // a CC = 0 sprite has been drawn on this line
  for (int s = 0; s < nsel; s++) {                  // highest priority (lowest number) first
    const int i = sel[s], rr = row[s];
    int x = vram[(sat + i * 4 + 1) & VMASK];
    uint8_t pat = vram[(sat + i * 4 + 2) & VMASK];
    uint8_t cb = mode2 ? vram[(ctab + i * 16 + rr) & VMASK] : vram[(sat + i * 4 + 3) & VMASK];
    const bool cc = mode2 && (cb & 0x40), ic = mode2 && (cb & 0x20);
    const uint8_t color = cb & 0x0F;
    if (cb & 0x80) x -= 32;
    if (size == 16) pat &= 0xFC;
    if (cc && !anchor) continue;                    // CC without a sprite before it: not shown
    if (!cc) anchor = true;
    if (color == 0 && !tp && !mode2) continue;      // TMS: colour 0 sprite is invisible
    const uint32_t pa = patB + pat * 8 + (rr & 7) + ((rr >= 8) ? 8 : 0);
    const uint16_t bits = (uint16_t)((vram[pa & VMASK] << 8) | (size == 16 ? vram[(pa + 16) & VMASK] : 0));
    for (int c = 0; c < size; c++) {
      if (!(bits & (0x8000 >> c))) continue;
      for (int k = 0; k < mag; k++) {
        const int px = x + c * mag + k;
        if (px < 0 || px >= 256) continue;
        if (cc) {
          if (spr[px]) spr[px] = (uint8_t)(((spr[px] - 1) | color) + 1);
          else         spr[px] = (uint8_t)(color + 1);
        } else {
          if (own[px] && !ic) {
            if (!(st[0] & 0x20)) { st[0] |= 0x20; st[3] = (uint8_t)(px + 12); st[5] = (uint8_t)(vy + 8); }
          }
          if (!spr[px]) spr[px] = (uint8_t)(color + 1);
          if (!ic) own[px] = 1;
        }
      }
    }
  }
  if (!draw) return;
  for (int x = 0; x < 256; x++) {
    if (!spr[x]) continue;
    const uint8_t c = spr[x] - 1;
    if (c == 0 && !tp) continue;                    // transparent
    switch (m) {
      case M_G5: p[2 * x] = (c >> 2) & 3; p[2 * x + 1] = c & 3; break;
      case M_G6: p[2 * x] = p[2 * x + 1] = c; break;
      case M_G7: p[x] = G7_SPRITE[c]; break;
      default:   p[x] = c; break;
    }
  }
}

int v99FrameStart(bool render) {
  renderOn = render && framebuffer;
  activeH = (r[9] & 0x80) ? 212 : 192;
  inVblank = false;
  // TEXT2 blink: R13 = on time (high nibble) / off time (low), in units of 10 frames.
  const int on = r[13] >> 4, off = r[13] & 0x0F;
  if (!on) blinkAlt = false;
  else if (!off) blinkAlt = true;
  else if (++blinkCount >= (blinkAlt ? on : off) * 10) { blinkCount = 0; blinkAlt = !blinkAlt; }
  return (r[9] & 0x02) ? 313 : 262;
}

uint32_t frameMicros() { return (r[9] & 0x02) ? 19968 : 16687; }

void v99Line(int line, uint32_t lineStartCycle) {
  lineStart = lineStartCycle;
  if (line == activeH) {                            // end of the active area: VBlank
    inVblank = true;
    st[0] |= 0x80;
    if (renderOn) {
      v99FrameH = activeH;
      memcpy(v99DispPal, pal565, sizeof(pal565));
      frameReady = true;
      renderOn = false;
    }
    return;
  }
  if (line > activeH) return;
  if (((line + r[23]) & 0xFF) == r[19]) fh = true;  // line interrupt (R19 against the scrolled line)

  const int vy = (line + r[23]) & 0xFF;
  const int m = mode();
  uint8_t* p = renderOn ? framebuffer + line * FB2_W : nullptr;
  const bool wide = (m == M_T2 || m == M_G5 || m == M_G6);
  if (p) v99LineFlags[line] = (uint8_t)((wide ? V99_LINE_WIDE : 0) | (m == M_G7 ? V99_LINE_G7 : 0));

  if (!(r[1] & 0x40)) {                             // blanked: border colour only
    if (p) memset(p, m == M_G7 ? r[7] : backdrop(), wide ? 512 : 256);
    return;
  }
  if (p) {
    switch (m) {
      case M_T1: lineText1(p, vy); break;
      case M_T2: lineText2(p, vy); break;
      case M_MC: lineMulticolor(p, vy); break;
      case M_G2: case M_G3: lineGraphic23(p, vy); break;
      case M_G4: case M_G5: case M_G6: case M_G7: lineBitmap(p, vy, m); break;
      default:   lineGraphic1(p, vy); break;
    }
  }
  if (m != M_T1 && m != M_T2) lineSprites(p, vy, m, p != nullptr);
}

} // namespace msx

#endif  // BOARD_HAS_MSX_CORE && BOARD_HAS_MSX2
