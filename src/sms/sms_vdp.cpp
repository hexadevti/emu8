// sms_vdp.cpp - Sega 315-5124 VDP (Master System) for the SMS core. Ports $BE (data) / $BF (control +
// status). It is a superset of the TMS9918: the control-port command carries a 2-bit CODE field
// (0=VRAM read, 1=VRAM write, 2=register write, 3=CRAM write), there is a 32-byte colour RAM (two
// 16-colour palettes, 6-bit --BBGGRR), 16 registers, hardware horizontal/vertical scrolling, line
// interrupts, and Mode 4 tile/sprite graphics.
//
// Renders into the 256x192 8-bit indexed framebuffer (values 0..31 index CRAM). The platform render
// path (sms.cpp / sms_host.cpp) converts CRAM to a 32-entry RGB565 LUT each frame.

#include "sms.h"
#include <string.h>

namespace sms {

// ---- register + access state ----
static uint8_t  vreg[16];
static uint8_t  cram[32];
static uint8_t  vstatus;          // b7 VBlank, b6 sprite overflow, b5 sprite collision
static uint16_t vaddr;            // 14-bit VRAM pointer
static uint8_t  vcode;            // 2-bit command code from the 2nd control byte
static bool     vlatched;         // control-port first/second byte handshake
static uint8_t  vlatch;           // first control byte
static uint8_t  vreadAhead;       // $BE read prefetch buffer
static bool     lineIntPending;   // line interrupt latched (cleared by status read)
static int      lineCounter;      // R10 down-counter
static int      curLine;          // current scanline (for V counter)
static uint8_t  vsLatch;          // vertical scroll latched at the start of the active display
static uint8_t  lineHS[VDP_H];    // per-scanline horizontal scroll snapshot (split-screen)

void vdpReset() {
  memset(vreg, 0, sizeof(vreg));
  memset(cram, 0, sizeof(cram));
  vstatus = 0; vaddr = 0; vcode = 0; vlatched = false; vlatch = 0; vreadAhead = 0;
  lineIntPending = false; lineCounter = 0; curLine = 0; vsLatch = 0;
  memset(lineHS, 0, sizeof(lineHS));
  vreg[2] = 0x0E;   // sensible name-table base (0x3800) before the game programs it
  vreg[10] = 0xFF;  // line counter inert until programmed
}

uint8_t vdpRegister(int r) { return vreg[r & 15]; }
uint8_t vdpCram(int i)     { return cram[i & 31]; }

// ---- port $BF control / status ----
void vdpWriteCtrl(uint8_t v) {
  if (!vlatched) { vlatch = v; vlatched = true; return; }
  vlatched = false;
  vcode = (v >> 6) & 3;
  vaddr = (uint16_t)(((v & 0x3F) << 8) | vlatch);
  if (vcode == 0) {                                 // VRAM read setup: prefetch
    vreadAhead = vram[vaddr & 0x3FFF];
    vaddr = (vaddr + 1) & 0x3FFF;
  } else if (vcode == 2) {                          // register write (reg = low nibble of 2nd byte)
    vreg[v & 0x0F] = vlatch;
  }
}
uint8_t vdpReadStatus() {
  uint8_t s = vstatus;
  vstatus &= ~0xE0;        // reading clears VBlank + overflow + collision
  vlatched = false;        // ...and the control latch
  lineIntPending = false;  // ...and acknowledges the line interrupt
  return s;
}

// ---- port $BE data ----
void vdpWriteData(uint8_t v) {
  vlatched = false;
  if (vcode == 3) cram[vaddr & 0x1F] = v;           // CRAM write
  else            vram[vaddr & 0x3FFF] = v;          // VRAM write (codes 0/1/2)
  vreadAhead = v;
  vaddr = (vaddr + 1) & 0x3FFF;
}
uint8_t vdpReadData() {
  vlatched = false;
  uint8_t r = vreadAhead;
  vreadAhead = vram[vaddr & 0x3FFF];
  vaddr = (vaddr + 1) & 0x3FFF;
  return r;
}

// ---- counters ----
uint8_t vdpVCounter() {
  // NTSC 192-line mapping: 0x00-0xDA then 0xD5-0xFF (the V counter "jumps back" during VBlank).
  int l = curLine;
  if (l > 0xDA) l -= 6;
  return (uint8_t)(l & 0xFF);
}
uint8_t vdpHCounter() { return 0; }                  // not raster-accurate; few games need it

// ---- per-scanline timing ----
void vdpLineTick(int line) {
  curLine = line;
  if (line == 0) vsLatch = vreg[9];                  // vscroll is latched once per frame
  if (line <= VDP_H) {                               // line counter runs over active + first VBlank line
    if (lineCounter == 0) { lineCounter = vreg[10]; lineIntPending = true; }
    else lineCounter--;
  } else {
    lineCounter = vreg[10];                          // reloaded each VBlank line
  }
  if (line == VDP_H) vstatus |= 0x80;                // frame (VBlank) interrupt flag at line 192
}
void vdpSnapshotLine(int line) { if (line >= 0 && line < VDP_H) lineHS[line] = vreg[8]; }

bool vdpIrqActive() {
  return ((vstatus & 0x80) && (vreg[1] & 0x20)) ||   // frame interrupt (R1 bit5)
         (lineIntPending && (vreg[0] & 0x10));        // line interrupt  (R0 bit4)
}

// ---- palette ----
// CRAM colour is 6-bit --BBGGRR; expand each 2-bit channel to 8-bit (x*85) then pack RGB565.
static inline uint16_t cramToRgb565(uint8_t c) {
  uint8_t r = (c & 3) * 85, g = ((c >> 2) & 3) * 85, b = ((c >> 4) & 3) * 85;
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
void vdpBuildPalette(uint16_t* lut32) { for (int i = 0; i < 32; i++) lut32[i] = cramToRgb565(cram[i]); }

// ---- Mode 4 rendering -------------------------------------------------------------------------
// Background: 32x28 tile name table (2 bytes/entry), 8x8 4-bitplane tiles (32 bytes each). Horizontal
// scroll (R8) and vertical scroll (R9, latched) with the top-rows / right-cols scroll locks (R0
// bits 6/7) and the hide-left-8 column (R0 bit5). Background pixels carry a priority bit that, when
// set on a non-zero pixel, keeps the tile in front of sprites.
static uint8_t  bgPrio[VDP_W];     // per-pixel: background-over-sprite flag for the current scanline

static void renderBgLine(uint8_t* fb, int y) {
  uint16_t nameBase = (uint16_t)((vreg[2] & 0x0E) << 10);
  uint8_t  backdrop = (uint8_t)(16 + (vreg[7] & 0x0F));   // border/backdrop uses the sprite palette
  bool lockTop   = (vreg[0] & 0x40) && (y < 16);          // top 2 rows ignore hscroll
  bool hideLeft8 = (vreg[0] & 0x20) != 0;
  uint8_t hs = lockTop ? 0 : lineHS[y];
  uint8_t* row = fb + y * VDP_W;

  for (int x = 0; x < VDP_W; x++) {
    bool lockRight = (vreg[0] & 0x80) && (x >= 192);       // right 8 columns ignore vscroll
    uint8_t vs = lockRight ? 0 : vsLatch;
    int tmY = (y + vs) % 224;                              // vertical wrap is modulo 224 (28 rows)
    int tmX = (x - hs) & 0xFF;                             // horizontal wrap is modulo 256
    int tileCol = tmX >> 3, tileRow = tmY >> 3;
    uint16_t e = (uint16_t)(nameBase + (tileRow * 32 + tileCol) * 2);
    uint8_t lo = vram[e & 0x3FFF], hi = vram[(e + 1) & 0x3FFF];
    int tile = lo | ((hi & 1) << 8);
    bool hflip = hi & 0x02, vflip = hi & 0x04;
    int  pal   = (hi & 0x08) ? 16 : 0;
    bool prio  = hi & 0x10;
    int fy = tmY & 7; if (vflip) fy = 7 - fy;
    int fx = tmX & 7; if (hflip) fx = 7 - fx;
    uint16_t base = (uint16_t)(tile * 32 + fy * 4);
    int sh = 7 - fx;
    int color = ((vram[base & 0x3FFF] >> sh) & 1)
              | (((vram[(base + 1) & 0x3FFF] >> sh) & 1) << 1)
              | (((vram[(base + 2) & 0x3FFF] >> sh) & 1) << 2)
              | (((vram[(base + 3) & 0x3FFF] >> sh) & 1) << 3);
    row[x]   = (uint8_t)(pal + color);
    bgPrio[x] = (prio && color != 0) ? 1 : 0;
  }
  if (hideLeft8) for (int x = 0; x < 8; x++) row[x] = backdrop;
}

// Sprites: 64-entry attribute table, 8x8 or 8x16 (R1 bit1), optional 2x magnify (R1 bit0). Always use
// the sprite palette (CRAM 16-31); colour 0 is transparent. Max 8 per line -> overflow (status bit6);
// overlapping opaque pixels set the collision flag (status bit5).
static void renderSpriteLine(uint8_t* fb, int y) {
  uint16_t attr = (uint16_t)((vreg[5] & 0x7E) << 7);
  uint16_t patb = (uint16_t)((vreg[6] & 0x04) << 11);
  int h = (vreg[1] & 0x02) ? 16 : 8;
  int mag = (vreg[1] & 0x01) ? 2 : 1;
  int dh = h * mag;
  int shiftX = (vreg[0] & 0x08) ? 8 : 0;                  // R0 bit3: shift sprites left by 8
  uint8_t* row = fb + y * VDP_W;
  static uint8_t mark[VDP_W];
  memset(mark, 0, sizeof(mark));

  int drawn = 0;
  for (int i = 0; i < 64; i++) {
    int sy = vram[(attr + i) & 0x3FFF];
    if (sy == 0xD0) break;                                // terminator (192-line mode)
    int top = (sy + 1) & 0xFF; if (top > 224) top -= 256;
    if (y < top || y >= top + dh) continue;               // sprite not on this scanline
    if (drawn >= 8) { vstatus |= 0x40; break; }           // 9th sprite on the line -> overflow
    drawn++;
    int sx = vram[(attr + 0x80 + i * 2) & 0x3FFF] - shiftX;
    int tile = vram[(attr + 0x80 + i * 2 + 1) & 0x3FFF];
    if (h == 16) tile &= 0xFE;                             // 8x16: even tile + next
    int r = (y - top) / mag;                               // 0..h-1
    uint16_t base = (uint16_t)(patb + tile * 32 + r * 4);
    for (int c = 0; c < 8; c++) {
      int sh = 7 - c;
      int color = ((vram[base & 0x3FFF] >> sh) & 1)
                | (((vram[(base + 1) & 0x3FFF] >> sh) & 1) << 1)
                | (((vram[(base + 2) & 0x3FFF] >> sh) & 1) << 2)
                | (((vram[(base + 3) & 0x3FFF] >> sh) & 1) << 3);
      if (color == 0) continue;                            // transparent
      for (int m = 0; m < mag; m++) {
        int px = sx + c * mag + m;
        if (px < 0 || px >= VDP_W) continue;
        if (mark[px]) { vstatus |= 0x20; continue; }       // collision; lower-index sprite wins
        mark[px] = 1;
        if (!bgPrio[px]) row[px] = (uint8_t)(16 + color);  // behind high-priority background
      }
    }
  }
}

void vdpRender() {
  uint8_t* fb = framebuffer;
  if (!fb) return;
  vstatus &= ~(0x40 | 0x20);                               // recompute overflow + collision each frame
  bool mode4    = (vreg[0] & 0x04) != 0;
  bool blanked  = (vreg[1] & 0x40) == 0;                   // R1 bit6: display enable
  if (!mode4 || blanked) {                                 // legacy TMS modes / blanked -> backdrop fill
    uint8_t backdrop = (uint8_t)(16 + (vreg[7] & 0x0F));
    memset(fb, backdrop, FB_SIZE);
    return;
  }
  for (int y = 0; y < VDP_H; y++) {
    renderBgLine(fb, y);
    renderSpriteLine(fb, y);
  }
}

} // namespace sms
