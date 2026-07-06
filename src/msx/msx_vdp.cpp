// msx_vdp.cpp - TMS9918A VDP for the MSX1 core. Ports $98 (data) / $99 (address+register / status).
// Implements the address-latch state machine, the read-ahead buffer, the VBlank interrupt, and
// background rendering for Text Mode 1 (40x24), Graphic Mode 1 (32x24) and Graphic Mode 2 (256x192).
// Sprites + multicolor + collision/5th-sprite status land in M3 (game support).
//
// Renders into the 256x192 8-bit indexed framebuffer; the platform render task converts it to
// RGB565 8-line bands using MSX_PALETTE (see msx.cpp / video.cpp), like the NES/Atari paths.

#include "msx.h"
#include <string.h>

namespace msx {

uint16_t MSX_PALETTE[16];

// VDP registers + access state
static uint8_t  vreg[8];
static uint8_t  vstatus;
static uint16_t vaddr;          // 14-bit VRAM pointer
static bool     vlatched;       // control-port first/second byte
static uint8_t  vlatch;         // first control byte
static uint8_t  vreadAhead;     // $98 read prefetch buffer

static const uint8_t TMS_RGB[16][3] = {
  {  0,  0,  0}, {  0,  0,  0}, { 33,200, 66}, { 94,220,120},
  { 84, 85,237}, {125,118,252}, {212, 82, 77}, { 66,235,245},
  {252, 85, 84}, {255,121,120}, {212,193, 84}, {230,206,128},
  { 33,176, 59}, {201, 91,186}, {204,204,204}, {255,255,255},
};
static void initPalette() {
  for (int i = 0; i < 16; i++) {
    uint8_t r = TMS_RGB[i][0], g = TMS_RGB[i][1], b = TMS_RGB[i][2];
    MSX_PALETTE[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  }
}

void vdpReset() {
  memset(vreg, 0, sizeof(vreg));
  vstatus = 0; vaddr = 0; vlatched = false; vlatch = 0; vreadAhead = 0;
  initPalette();
}

uint8_t vdpRegister(int r) { return vreg[r & 7]; }

void vdpWriteCtrl(uint8_t v) {
  if (!vlatched) { vlatch = v; vlatched = true; return; }
  vlatched = false;
  if (v & 0x80) {                                   // register write
    vreg[v & 0x07] = vlatch;
  } else {                                          // set VRAM address
    vaddr = (uint16_t)(((v & 0x3F) << 8) | vlatch);
    if (!(v & 0x40)) {                              // read setup: prefetch
      vreadAhead = vram[vaddr & 0x3FFF];
      vaddr = (vaddr + 1) & 0x3FFF;
    }
  }
}
uint8_t vdpReadStatus() {
  uint8_t s = vstatus;
  vstatus &= ~0x80;        // reading status clears the VBlank/INT flag
  vlatched = false;        // ...and resets the control latch
  return s;
}
void vdpWriteData(uint8_t v) {
  vram[vaddr & 0x3FFF] = v; vreadAhead = v; vaddr = (vaddr + 1) & 0x3FFF; vlatched = false;
}
uint8_t vdpReadData() {
  uint8_t r = vreadAhead; vreadAhead = vram[vaddr & 0x3FFF]; vaddr = (vaddr + 1) & 0x3FFF; vlatched = false; return r;
}

void vdpEndFrame()     { vstatus |= 0x80; }
bool vdpIrqActive()    { return (vstatus & 0x80) && (vreg[1] & 0x20); }

// ---- rendering ---------------------------------------------------------------------------------
static inline void fillRow(uint8_t* fb, int y, uint8_t color) { memset(fb + y * VDP_W, color, VDP_W); }

static void renderText1(uint8_t* fb) {     // 40x24, 6px-wide chars, 2 colors from R7
  uint16_t nameBase = (uint16_t)((vreg[2] & 0x0F) << 10);
  uint16_t patBase  = (uint16_t)((vreg[4] & 0x07) << 11);
  uint8_t fg = vreg[7] >> 4, bg = vreg[7] & 0x0F;
  const int OX = (VDP_W - 240) / 2;          // 8px border each side
  for (int row = 0; row < 24; row++) {
    for (int line = 0; line < 8; line++) {
      int y = row * 8 + line;
      uint8_t* p = fb + y * VDP_W;
      for (int i = 0; i < OX; i++) p[i] = bg;
      for (int col = 0; col < 40; col++) {
        uint8_t ch = vram[(nameBase + row * 40 + col) & 0x3FFF];
        uint8_t pat = vram[(patBase + ch * 8 + line) & 0x3FFF];
        int x = OX + col * 6;
        for (int px = 0; px < 6; px++) p[x + px] = (pat & (0x80 >> px)) ? fg : bg;
      }
      for (int i = OX + 240; i < VDP_W; i++) p[i] = bg;
    }
  }
}

static void renderGraphic1(uint8_t* fb) {  // 32x24 tiles, color per group of 8 patterns
  uint16_t nameBase = (uint16_t)((vreg[2] & 0x0F) << 10);
  uint16_t patBase  = (uint16_t)((vreg[4] & 0x07) << 11);
  uint16_t colBase  = (uint16_t)(vreg[3] << 6);
  for (int row = 0; row < 24; row++) {
    for (int line = 0; line < 8; line++) {
      int y = row * 8 + line;
      uint8_t* p = fb + y * VDP_W;
      for (int col = 0; col < 32; col++) {
        uint8_t ch  = vram[(nameBase + row * 32 + col) & 0x3FFF];
        uint8_t pat = vram[(patBase + ch * 8 + line) & 0x3FFF];
        uint8_t clr = vram[(colBase + (ch >> 3)) & 0x3FFF];
        uint8_t fg = clr >> 4, bg = clr & 0x0F;
        int x = col * 8;
        for (int px = 0; px < 8; px++) p[x + px] = (pat & (0x80 >> px)) ? fg : bg;
      }
    }
  }
}

static void renderGraphic2(uint8_t* fb) {  // 256x192 bitmap (3 banks of 8 rows)
  uint16_t nameBase = (uint16_t)((vreg[2] & 0x0F) << 10);
  uint16_t patBase  = (vreg[4] & 0x04) ? 0x2000 : 0x0000;
  uint16_t colBase  = (vreg[3] & 0x80) ? 0x2000 : 0x0000;
  for (int row = 0; row < 24; row++) {
    int third = row >> 3;
    for (int line = 0; line < 8; line++) {
      int y = row * 8 + line;
      uint8_t* p = fb + y * VDP_W;
      for (int col = 0; col < 32; col++) {
        uint8_t name = vram[(nameBase + row * 32 + col) & 0x3FFF];
        int idx = (third * 256 + name) * 8 + line;
        uint8_t pat = vram[(patBase + idx) & 0x3FFF];
        uint8_t clr = vram[(colBase + idx) & 0x3FFF];
        uint8_t fg = clr >> 4, bg = clr & 0x0F;
        int x = col * 8;
        for (int px = 0; px < 8; px++) p[x + px] = (pat & (0x80 >> px)) ? fg : bg;
      }
    }
  }
}

// Sprites (modes 1/2/3): 32 entries, 8x8 or 16x16, optional 2x magnify. Honors the 4-per-line
// limit (sets the 5th-sprite status), color-0 transparency, early-clock (-32px), and a per-line
// collision check (status bit 5). Sprite 0 is highest priority (drawn last so it wins).
static void renderSprites(uint8_t* fb) {
  int size = (vreg[1] & 0x02) ? 16 : 8;
  int mag  = (vreg[1] & 0x01) ? 2 : 1;
  int dsize = size * mag;
  uint16_t attr = (uint16_t)((vreg[5] & 0x7F) << 7);
  uint16_t patb = (uint16_t)((vreg[6] & 0x07) << 11);
  vstatus &= ~(0x40 | 0x20);                 // clear 5th-sprite + collision (recomputed per frame)

  // collect the active sprite list (stop at y == 0xD0)
  int order[32], nsp = 0;
  for (int i = 0; i < 32; i++) { if (vram[(attr + i * 4) & 0x3FFF] == 0xD0) break; order[nsp++] = i; }

  static uint8_t lineMark[VDP_W];
  for (int py = 0; py < VDP_H; py++) {
    // Pass 1: pick the (up to 4) lowest-numbered sprites that cover this scanline; the 5th sets the
    // 5th-sprite status. The 4-per-line limit keeps the LOWEST indices (highest priority).
    int sel[4], nsel = 0;
    for (int k = 0; k < nsp; k++) {
      int i = order[k];
      int yb = vram[(attr + i * 4) & 0x3FFF];
      int sy = yb + 1; if (sy > 225) sy -= 256;          // y > 225 wraps partially above the top
      if (py < sy || py >= sy + dsize) continue;          // sprite doesn't cover this scanline
      if (nsel >= 4) { vstatus = (uint8_t)((vstatus & ~0x1F) | (i & 0x1F) | 0x40); break; }  // 5th-sprite
      sel[nsel++] = i;
    }
    // Pass 2: draw HIGH index first so the lowest-numbered sprite ends up on top (MSX priority).
    memset(lineMark, 0, sizeof(lineMark));
    for (int s = nsel - 1; s >= 0; s--) {
      int i = sel[s];
      int yb = vram[(attr + i * 4) & 0x3FFF];
      int sy = yb + 1; if (sy > 225) sy -= 256;
      int x = vram[(attr + i * 4 + 1) & 0x3FFF];
      uint8_t pat = vram[(attr + i * 4 + 2) & 0x3FFF];
      uint8_t cb = vram[(attr + i * 4 + 3) & 0x3FFF];
      int color = cb & 0x0F;
      if (color == 0) continue;                            // sprite color 0 = fully transparent
      if (cb & 0x80) x -= 32;                              // early clock
      if (size == 16) pat &= 0xFC;
      int r = (py - sy) / mag;                             // pattern row 0..size-1
      for (int c = 0; c < size; c++) {
        int patNum = pat + ((size == 16) ? (((c >= 8) ? 2 : 0) + ((r >= 8) ? 1 : 0)) : 0);
        uint8_t pb = vram[(patb + patNum * 8 + (r & 7)) & 0x3FFF];
        if (!(pb & (0x80 >> (c & 7)))) continue;           // transparent (pattern bit 0)
        for (int mx = 0; mx < mag; mx++) {
          int px = x + c * mag + mx;
          if (px < 0 || px >= VDP_W) continue;
          if (lineMark[px]) vstatus |= 0x20;               // collision
          lineMark[px] = 1;
          fb[py * VDP_W + px] = (uint8_t)color;
        }
      }
    }
  }
}

void vdpRender() {
  uint8_t* fb = framebuffer;
  if (!fb) return;
  bool m1 = (vreg[1] & 0x10) != 0;     // Text Mode 1
  bool m2 = (vreg[0] & 0x02) != 0;     // Graphic Mode 2
  bool m3 = (vreg[1] & 0x08) != 0;     // Multicolor
  if (m1) { renderText1(fb); return; }                     // text mode has no sprites
  if (m2)      renderGraphic2(fb);
  else if (m3) { uint8_t bg = vreg[7] & 0x0F; for (int y = 0; y < VDP_H; y++) fillRow(fb, y, bg); }  // multicolor: M3+
  else         renderGraphic1(fb);
  renderSprites(fb);
}

} // namespace msx
