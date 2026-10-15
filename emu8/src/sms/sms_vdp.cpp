// This SMS translation unit is compiled out entirely on boards that clear
// BOARD_HAS_SMS_CORE (currently none -- see board.h; it is on for every board).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro;
// the guard below then empties the file, keeping the Z80 core's static RAM out of the image.
#include "../../emu.h"
#if BOARD_HAS_SMS_CORE

// sms_vdp.cpp - Sega 315-5124 VDP (Master System) for the SMS core. Ports $BE (data) / $BF (control +
// status). It is a superset of the TMS9918: the control-port command carries a 2-bit CODE field
// (0=VRAM read, 1=VRAM write, 2=register write, 3=CRAM write), there is a 32-byte colour RAM (two
// 16-colour palettes, 6-bit --BBGGRR), 16 registers, hardware horizontal/vertical scrolling, line
// interrupts, and Mode 4 tile/sprite graphics.
//
// Renders into the 256x192 8-bit indexed framebuffer (values 0..31 index CRAM). The platform render
// path (sms.cpp / sms_host.cpp) converts CRAM to a 32-entry RGB565 LUT each frame.

#include "sms.h"
#include "../z80/z80_snapshot.h"
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
// per-scanline horizontal scroll snapshot (split-screen); sharedBigBuf tail, see emu.h
static uint8_t (&lineHS)[VDP_H] = SHARED_TAIL_ARRAY(SMS_LINEHS_OFF, VDP_H);

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
// Render source: the live VDP state (vdpRender) or the captured copy (vdpRenderCaptured), below.
static const uint8_t *rVram, *rReg, *rHS;
static uint8_t rVs, rStat;

// Background: 32x28 tile name table (2 bytes/entry), 8x8 4-bitplane tiles (32 bytes each). Horizontal
// scroll (R8) and vertical scroll (R9, latched) with the top-rows / right-cols scroll locks (R0
// bits 6/7) and the hide-left-8 column (R0 bit5). Background pixels carry a priority bit that, when
// set on a non-zero pixel, keeps the tile in front of sprites.
// per-pixel: background-over-sprite flag for the current scanline; sharedBigBuf tail, see emu.h
static uint8_t (&bgPrio)[VDP_W] = SHARED_TAIL_ARRAY(SMS_BGPRIO_OFF, VDP_W);

// One line of the background is decoded tile by tile into a line buffer laid on the tile grid (33
// tiles = 264 pixels), then copied out at the fine-scroll offset. Each tile's 4 bitplanes become 8
// pixel bytes with word operations -- a nibble of a plane spread to the low bit of 4 bytes by table,
// 4 pixels per store -- instead of the old per-pixel shifting: on the M0+ that loop was most of the
// ~28 ms a picture cost. Both buffers are in sharedBigBuf's tail (emu.h).
#define linePx (reinterpret_cast<uint32_t*>(sharedBigBuf + SMS_LINEPX_OFF))
#define linePr (reinterpret_cast<uint32_t*>(sharedBigBuf + SMS_LINEPR_OFF))
static_assert((SMS_LINEPX_OFF & 3) == 0 && (SMS_LINEPR_OFF & 3) == 0, "SMS line buffers must be word aligned");

// Nibble -> its 4 bits in the low bit of 4 bytes (little-endian: byte 0 is the leftmost pixel).
// FWD: bit 3 leftmost (a plane's high bit is its leftmost pixel); REV: bit 0 leftmost (h-flip).
#define S4(b0, b1, b2, b3) ((uint32_t)(b0) | ((uint32_t)(b1) << 8) | ((uint32_t)(b2) << 16) | ((uint32_t)(b3) << 24))
#define FWD(n) S4(((n) >> 3) & 1, ((n) >> 2) & 1, ((n) >> 1) & 1, (n) & 1)
#define REV(n) S4((n) & 1, ((n) >> 1) & 1, ((n) >> 2) & 1, ((n) >> 3) & 1)
static const uint32_t spreadFwd[16] = { FWD(0), FWD(1), FWD(2), FWD(3), FWD(4), FWD(5), FWD(6), FWD(7),
                                        FWD(8), FWD(9), FWD(10), FWD(11), FWD(12), FWD(13), FWD(14), FWD(15) };
static const uint32_t spreadRev[16] = { REV(0), REV(1), REV(2), REV(3), REV(4), REV(5), REV(6), REV(7),
                                        REV(8), REV(9), REV(10), REV(11), REV(12), REV(13), REV(14), REV(15) };
#undef FWD
#undef REV
#undef S4

// Decode name-table column tileCol of map row tmY into dst[0..1] (8 pixel bytes, palette applied)
// and pr[0..1] (1 where a high-priority tile has a non-zero pixel). Returns whether pr is non-zero.
static inline bool decodeTile(uint16_t nameBase, int tileCol, int tmY, uint32_t* dst, uint32_t* pr) {
  uint16_t e = (uint16_t)(nameBase + ((tmY >> 3) * 32 + tileCol) * 2);
  uint8_t lo = rVram[e & 0x3FFF], hi = rVram[(e + 1) & 0x3FFF];
  int tile = lo | ((hi & 1) << 8);
  int fy = tmY & 7; if (hi & 0x04) fy = 7 - fy;            // vflip
  const uint8_t* t = rVram + ((tile * 32 + fy * 4) & 0x3FFF); // 4 planes; tile*32+28 < 0x4000: no wrap
  const uint8_t p0 = t[0], p1 = t[1], p2 = t[2], p3 = t[3];
  uint32_t l, r;
  if (hi & 0x02) {                                         // hflip: bit 0 is the leftmost pixel
    l = spreadRev[p0 & 15] | (spreadRev[p1 & 15] << 1) | (spreadRev[p2 & 15] << 2) | (spreadRev[p3 & 15] << 3);
    r = spreadRev[p0 >> 4] | (spreadRev[p1 >> 4] << 1) | (spreadRev[p2 >> 4] << 2) | (spreadRev[p3 >> 4] << 3);
  } else {
    l = spreadFwd[p0 >> 4] | (spreadFwd[p1 >> 4] << 1) | (spreadFwd[p2 >> 4] << 2) | (spreadFwd[p3 >> 4] << 3);
    r = spreadFwd[p0 & 15] | (spreadFwd[p1 & 15] << 1) | (spreadFwd[p2 & 15] << 2) | (spreadFwd[p3 & 15] << 3);
  }
  bool prio = false;
  if (hi & 0x10) {                                         // in front of sprites where the pixel is opaque
    pr[0] = (l | (l >> 1) | (l >> 2) | (l >> 3)) & 0x01010101u;
    pr[1] = (r | (r >> 1) | (r >> 2) | (r >> 3)) & 0x01010101u;
    prio = (pr[0] | pr[1]) != 0;
  } else {
    pr[0] = pr[1] = 0;
  }
  const uint32_t pal = (hi & 0x08) ? 0x10101010u : 0;     // sprite palette (CRAM 16-31)
  dst[0] = l | pal;
  dst[1] = r | pal;
  return prio;
}

static void renderBgLine(uint8_t* fb, int y) {
  uint16_t nameBase = (uint16_t)((rReg[2] & 0x0E) << 10);
  uint8_t  backdrop = (uint8_t)(16 + (rReg[7] & 0x0F));   // border/backdrop uses the sprite palette
  bool lockTop   = (rReg[0] & 0x40) && (y < 16);          // top 2 rows ignore hscroll
  bool hideLeft8 = (rReg[0] & 0x20) != 0;
  uint8_t hs = lockTop ? 0 : rHS[y];
  uint8_t* row = fb + y * VDP_W;
  const bool lockRightOn = (rReg[0] & 0x80) != 0;         // right 8 columns ignore vscroll
  const int  tmYScroll   = (y + rVs) % 224;                // vertical wrap is modulo 224 (28 rows)

  // Screen x shows map column (x - hs) & 0xFF: x = 0 is pixel `fine` of tile column c0.
  const int start = (256 - hs) & 0xFF;
  const int c0 = start >> 3, fine = start & 7;
  const uint8_t* px = reinterpret_cast<const uint8_t*>(linePx);
  const uint8_t* pr = reinterpret_cast<const uint8_t*>(linePr);
  bool anyPrio = false;
  for (int k = 0; k < 33; k++)
    anyPrio |= decodeTile(nameBase, (c0 + k) & 31, tmYScroll, linePx + 2 * k, linePr + 2 * k);
  const int split = lockRightOn ? 192 : VDP_W;
  memcpy(row, px + fine, split);
  if (anyPrio) memcpy(bgPrio, pr + fine, split); else memset(bgPrio, 0, split);
  if (lockRightOn) {                                       // x >= 192: the unscrolled map row y
    anyPrio = false;
    for (int k = 24; k < 33; k++)                          // tiles under x = 192..255
      anyPrio |= decodeTile(nameBase, (c0 + k) & 31, y, linePx + 2 * k, linePr + 2 * k);
    memcpy(row + 192, px + fine + 192, VDP_W - 192);
    if (anyPrio) memcpy(bgPrio + 192, pr + fine + 192, VDP_W - 192);
    else memset(bgPrio + 192, 0, VDP_W - 192);
  }
  if (hideLeft8) for (int x = 0; x < 8; x++) row[x] = backdrop;
}

// Sprites: 64-entry attribute table, 8x8 or 8x16 (R1 bit1), optional 2x magnify (R1 bit0). Always use
// the sprite palette (CRAM 16-31); colour 0 is transparent. Max 8 per line -> overflow (status bit6);
// overlapping opaque pixels set the collision flag (status bit5).
static void renderSpriteLine(uint8_t* fb, int y) {
  uint16_t attr = (uint16_t)((rReg[5] & 0x7E) << 7);
  uint16_t patb = (uint16_t)((rReg[6] & 0x04) << 11);
  int h = (rReg[1] & 0x02) ? 16 : 8;
  int mag = (rReg[1] & 0x01) ? 2 : 1;
  int dh = h * mag;
  int shiftX = (rReg[0] & 0x08) ? 8 : 0;                  // R0 bit3: shift sprites left by 8
  uint8_t* row = fb + y * VDP_W;
  uint8_t (&mark)[VDP_W] = SHARED_TAIL_ARRAY(SMS_MARK_OFF, VDP_W);           // sharedBigBuf tail, see emu.h
  memset(mark, 0, sizeof(mark));

  int drawn = 0;
  for (int i = 0; i < 64; i++) {
    int sy = rVram[(attr + i) & 0x3FFF];
    if (sy == 0xD0) break;                                // terminator (192-line mode)
    int top = (sy + 1) & 0xFF; if (top > 224) top -= 256;
    if (y < top || y >= top + dh) continue;               // sprite not on this scanline
    if (drawn >= 8) { rStat |= 0x40; break; }           // 9th sprite on the line -> overflow
    drawn++;
    int sx = rVram[(attr + 0x80 + i * 2) & 0x3FFF] - shiftX;
    int tile = rVram[(attr + 0x80 + i * 2 + 1) & 0x3FFF];
    if (h == 16) tile &= 0xFE;                             // 8x16: even tile + next
    int r = (y - top) / mag;                               // 0..h-1
    uint16_t base = (uint16_t)(patb + tile * 32 + r * 4);
    for (int c = 0; c < 8; c++) {
      int sh = 7 - c;
      int color = ((rVram[base & 0x3FFF] >> sh) & 1)
                | (((rVram[(base + 1) & 0x3FFF] >> sh) & 1) << 1)
                | (((rVram[(base + 2) & 0x3FFF] >> sh) & 1) << 2)
                | (((rVram[(base + 3) & 0x3FFF] >> sh) & 1) << 3);
      if (color == 0) continue;                            // transparent
      for (int m = 0; m < mag; m++) {
        int px = sx + c * mag + m;
        if (px < 0 || px >= VDP_W) continue;
        if (mark[px]) { rStat |= 0x20; continue; }       // collision; lower-index sprite wins
        mark[px] = 1;
        if (!bgPrio[px]) row[px] = (uint8_t)(16 + color);  // behind high-priority background
      }
    }
  }
}

static void renderFrame() {
  uint8_t* fb = framebuffer;
  if (!fb) return;
  rStat = 0;                                               // overflow + collision, recomputed each frame
  bool mode4    = (rReg[0] & 0x04) != 0;
  bool blanked  = (rReg[1] & 0x40) == 0;                   // R1 bit6: display enable
  if (!mode4 || blanked) {                                 // legacy TMS modes / blanked -> backdrop fill
    uint8_t backdrop = (uint8_t)(16 + (rReg[7] & 0x0F));
    memset(fb, backdrop, FB_SIZE);
    return;
  }
  for (int y = 0; y < VDP_H; y++) {
    renderBgLine(fb, y);
    renderSpriteLine(fb, y);
  }
}

// Render the live VDP state (the Z80's core, at the frame boundary).
void vdpRender() {
  rVram = vram; rReg = vreg; rHS = lineHS; rVs = vsLatch;
  renderFrame();
  vstatus = (uint8_t)((vstatus & ~(0x40 | 0x20)) | rStat);
}

// ---- render on the display core -----------------------------------------------------------------
// Drawing the picture is most of a frame's cost on the PicoCalc's M0+, and the Z80's core paid it on
// top of the emulation -- so only 1 frame in 4 could be drawn. Instead the Z80's core copies what the
// renderer reads (VRAM + registers + CRAM + the per-line scroll, ~16.6K, about 0.1 ms) and the
// display core draws from that copy while the Z80 runs on. Heap, allocated by smsSetup(): nothing
// static, so no other system loses RAM; without it everything works as before.
struct VdpSnap {
  uint8_t vram[0x4000];
  uint8_t vreg[16];
  uint8_t cram[32];
  uint8_t lineHS[VDP_H];
  uint8_t vsLatch;
  uint8_t statusOut;   // overflow / collision found by the last render, handed back to vstatus
};
static VdpSnap* snap = nullptr;

bool vdpSnapAlloc() {
  if (!snap) snap = (VdpSnap*)malloc(sizeof(VdpSnap));
  if (snap) memset(snap, 0, sizeof(VdpSnap));
  return snap != nullptr;
}
bool vdpHasSnap() { return snap != nullptr; }

// Z80's core, at the frame boundary, only while the display core is idle (frameReady == false).
void vdpCapture() {
  memcpy(snap->vram, vram, sizeof(snap->vram));
  memcpy(snap->vreg, vreg, sizeof(snap->vreg));
  memcpy(snap->cram, cram, sizeof(snap->cram));
  memcpy(snap->lineHS, lineHS, sizeof(snap->lineHS));
  snap->vsLatch = vsLatch;
  vstatus |= snap->statusOut;                              // a frame late; nothing else touches it now
  snap->statusOut = 0;
}

// Display core: draw the captured frame into framebuffer.
void vdpRenderCaptured() {
  rVram = snap->vram; rReg = snap->vreg; rHS = snap->lineHS; rVs = snap->vsLatch;
  renderFrame();
  snap->statusOut = rStat;
}

// Register / palette of the picture being shown (the capture when there is one).
// ---- save state (sms_snapshot.cpp) -------------------------------------------------------------
// "SVDP" record: R0-R15, CRAM, status, VRAM address (u16), command code, control latch toggle +
// first byte, read-ahead, line-IRQ pending, line counter, current line, the latched vscroll and the
// per-line hscroll table, and the overflow/collision bits the display-core render owes vstatus.
// VRAM itself is saved by the machine.
void vdpSnapshot(SnapIO &io) {
  io.tag("SVDP");
  io.arr(vreg);
  io.arr(cram);
  io.val(vstatus);
  io.val(vaddr);
  io.val(vcode);
  io.flag(vlatched);
  io.val(vlatch);
  io.val(vreadAhead);
  io.flag(lineIntPending);
  snapInt(io, lineCounter);
  snapInt(io, curLine);
  io.val(vsLatch);
  io.bytes(lineHS, sizeof(lineHS));
  uint8_t owed = snap ? snap->statusOut : 0;
  io.val(owed);
  if (io.loading && snap) snap->statusOut = owed;
}

// After a load: hand the restored frame to the display path (capture it, or draw it here when there
// is no capture buffer) without changing the restored status register.
void vdpRedraw() {
  if (snap) {
    const uint8_t owed = snap->statusOut;
    snap->statusOut = 0;                                   // vdpCapture would OR it into vstatus
    vdpCapture();
    snap->statusOut = owed;
  } else {
    const uint8_t s = vstatus;
    vdpRender();
    vstatus = s;
  }
}

uint8_t vdpShownRegister(int r) { return snap ? snap->vreg[r & 15] : vreg[r & 15]; }
void vdpBuildShownPalette(uint16_t* lut32) {
  const uint8_t* c = snap ? snap->cram : cram;
  for (int i = 0; i < 32; i++) lut32[i] = cramToRgb565(c[i]);
}

} // namespace sms

#endif  // BOARD_HAS_SMS_CORE
