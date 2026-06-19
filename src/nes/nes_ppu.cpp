#include "../../emu.h"
#include "nes.h"

// NES PPU (2C02) — scanline renderer. Driven from the CPU loop via ppuStep(cpuCycles): the
// PPU runs 3 dots per CPU cycle. We process one scanline at a time (262 lines x 341 dots):
// each visible line (0..239) is rendered into the 8-bit indexed framebuffer using the current
// scroll/registers, then the loopy v register advances exactly like hardware (incrementY +
// horizontal reload per line, full vertical reload on the pre-render line). VBlank/NMI is
// raised at line 241. Scanline granularity (not dot-accurate) is enough for background,
// sprites, sprite-0 hit and mid-frame scroll splits (e.g. SMB's status bar).
//
// Loopy register layout (v/t, 15 bits): yyy NN YYYYY XXXXX
//   XXXXX coarse X, YYYYY coarse Y, NN nametable select, yyy fine Y.  x = fine X (3 bits).

namespace nes {

// ---- PPU register / scroll state ----
static uint8_t  ctrl = 0;       // $2000
static uint8_t  mask = 0;       // $2001
static uint8_t  status = 0;     // $2002 (bit7 vblank, bit6 sprite0, bit5 overflow)
static uint8_t  oamAddr = 0;    // $2003
static uint16_t v = 0;          // current VRAM address (loopy)
static uint16_t t = 0;          // temp VRAM address (loopy)
static uint8_t  x = 0;          // fine X scroll
static bool     w = false;      // $2005/$2006 write toggle
static uint8_t  readBuffer = 0; // $2007 read buffer

// ---- frame timing ----
static int scanline = 0;        // 0..261 (0..239 visible, 241 vblank, 261 pre-render)
int dotAcc = 0;                 // accumulated PPU dots (global so cpuLoop can inline ppuStep)

// ---- per-scanline scratch ----
static bool bgOpaque[256];      // background pixel non-transparent (for sprite priority/spr0)
static bool spriteDrawn[256];   // a sprite already owns this pixel (lower OAM index wins)

static inline bool renderingEnabled() { return (mask & 0x18) != 0; }

// Map a nametable address ($2000-$3EFF) into the 2K physical VRAM per the mirroring mode.
static inline uint16_t mirrorAddr(uint16_t a) {
  a &= 0x0FFF;
  int table = a >> 10, offset = a & 0x03FF, phys;
  switch (mirrorMode) {
    case MIRROR_VERTICAL:   phys = table & 1;        break;  // NT0/NT2=0, NT1/NT3=1
    case MIRROR_HORIZONTAL: phys = (table >> 1) & 1; break;  // NT0/NT1=0, NT2/NT3=1
    case MIRROR_SINGLE1:    phys = 1;                break;
    default:                phys = 0;                break;  // SINGLE0
  }
  return (uint16_t)(phys * 0x0400 + offset);
}

// Palette RAM index with the $3F10/$14/$18/$1C -> $3F00/$04/$08/$0C mirror.
static inline uint8_t palIndex(uint16_t a) {
  uint8_t idx = a & 0x1F;
  if (idx >= 0x10 && (idx & 3) == 0) idx -= 0x10;
  return idx;
}

// CHR (pattern table) access — via the mapper's 8 x 1K windows ($0000..$1FFF).
uint8_t chrRead(uint16_t addr) {
  const uint8_t *p = chrMap[(addr >> 10) & 7];
  return p ? p[addr & 0x3FF] : 0;
}
void chrWrite(uint16_t addr, uint8_t val) {
  if (!chrIsRam) return;                 // CHR-ROM is read-only
  uint8_t *p = chrMap[(addr >> 10) & 7]; // windows point into chrData (the 8K CHR-RAM)
  if (p) p[addr & 0x3FF] = val;
}

// PPU bus read/write (pattern / nametable / palette).
static inline uint8_t ppuBusRead(uint16_t a) {
  a &= 0x3FFF;
  if (a < 0x2000) return chrRead(a);
  if (a < 0x3F00) return vram[mirrorAddr(a)];
  return paletteRam[palIndex(a)];
}
static inline void ppuBusWrite(uint16_t a, uint8_t val) {
  a &= 0x3FFF;
  if (a < 0x2000) { chrWrite(a, val); return; }
  if (a < 0x3F00) { vram[mirrorAddr(a)] = val; return; }
  paletteRam[palIndex(a)] = val & 0x3F;
}

// ---- $2007 data port (with the standard 1-byte read buffer for non-palette reads) ----
static uint8_t ppuDataRead() {
  uint16_t a = v & 0x3FFF;
  uint8_t result;
  if (a >= 0x3F00) {                 // palette reads are immediate; buffer gets the NT underneath
    result = paletteRam[palIndex(a)];
    readBuffer = ppuBusRead(a & 0x2FFF);
  } else {
    result = readBuffer;
    readBuffer = ppuBusRead(a);
  }
  v = (v + ((ctrl & 0x04) ? 32 : 1)) & 0x7FFF;
  return result;
}
static void ppuDataWrite(uint8_t val) {
  ppuBusWrite(v & 0x3FFF, val);
  v = (v + ((ctrl & 0x04) ? 32 : 1)) & 0x7FFF;
}

void ppuRegWrite(uint16_t reg, uint8_t val) {
  switch (reg) {
    case 0:  // PPUCTRL
      t = (t & 0xF3FF) | ((uint16_t)(val & 3) << 10);
      // Enabling NMI while the vblank flag is already set triggers one immediately.
      if ((val & 0x80) && !(ctrl & 0x80) && (status & 0x80)) nmiPending = true;
      ctrl = val;
      break;
    case 1:  // PPUMASK
      mask = val;
      break;
    case 3:  // OAMADDR
      oamAddr = val;
      break;
    case 4:  // OAMDATA
      oam[oamAddr++] = val;
      break;
    case 5:  // PPUSCROLL
      if (!w) { t = (t & 0xFFE0) | (val >> 3); x = val & 7; w = true; }
      else    { t = (t & 0x8C1F) | ((uint16_t)(val & 7) << 12) | ((uint16_t)(val & 0xF8) << 2); w = false; }
      break;
    case 6:  // PPUADDR
      if (!w) { t = (t & 0x80FF) | ((uint16_t)(val & 0x3F) << 8); w = true; }
      else    { t = (t & 0xFF00) | val; v = t; w = false; }
      break;
    case 7:  // PPUDATA
      ppuDataWrite(val);
      break;
    default: break;  // $2002 is read-only
  }
}

uint8_t ppuRegRead(uint16_t reg) {
  switch (reg) {
    case 2: {                       // PPUSTATUS
      uint8_t r = status & 0xE0;    // low bits = open bus (ignored)
      status &= ~0x80;              // reading clears vblank
      w = false;                    // and the $2005/$2006 write toggle
      return r;
    }
    case 4: return oam[oamAddr];    // OAMDATA
    case 7: return ppuDataRead();   // PPUDATA
    default: return 0;
  }
}

void oamDmaWrite(uint8_t page) {
  uint16_t base = (uint16_t)page << 8;
  for (int i = 0; i < 256; i++) oam[(oamAddr + i) & 0xFF] = read8(base + i);
  dmaStallCycles += 513;            // the CPU is stalled ~513 cycles during the DMA
}

// ---- loopy scroll advance (hardware-accurate) ----
static inline void incrementY() {
  if ((v & 0x7000) != 0x7000) { v += 0x1000; }       // fine Y++
  else {
    v &= ~0x7000;
    int y = (v & 0x03E0) >> 5;
    if (y == 29)      { y = 0; v ^= 0x0800; }         // wrap to next vertical nametable
    else if (y == 31) { y = 0; }                      // out of bounds -> wrap, no NT swap
    else              { y++; }
    v = (v & ~0x03E0) | (y << 5);
  }
}
static inline void reloadHorizontal() { v = (v & ~0x041F) | (t & 0x041F); }

// ---- scanline rendering ----
static void renderBg(int /*line*/, uint8_t *fb) {
  uint16_t vv = v;
  int fineY = (v >> 12) & 7;
  uint16_t bgTable = (ctrl & 0x10) ? 0x1000 : 0x0000;
  int px = -(int)x;                                   // first tile shifted left by fine X
  bool leftMask = !(mask & 0x02);                     // background hidden in left 8px
  // Incremental nametable index: resolve via mirrorAddr only at the row start and on each
  // coarse-X wrap (nametable switch); within a row consecutive tiles are consecutive bytes.
  // The attribute byte is re-fetched only when its address changes (every 4 tiles). Output is
  // identical to the per-tile mirrorAddr version — just far fewer mirror/vram lookups.
  uint16_t ntOff = mirrorAddr(0x2000 | (vv & 0x0FFF));
  uint16_t lastAt = 0xFFFF; uint8_t at = 0;
  for (int tile = 0; tile < 33; tile++) {
    uint8_t  tileIndex = vram[ntOff];
    uint16_t atAddr = 0x23C0 | (vv & 0x0C00) | ((vv >> 4) & 0x38) | ((vv >> 2) & 0x07);
    if (atAddr != lastAt) { at = vram[mirrorAddr(atAddr)]; lastAt = atAddr; }
    int shift = ((vv >> 4) & 4) | (vv & 2);
    const uint8_t *pal = &paletteRam[((at >> shift) & 3) << 2]; // 4-entry palette for this tile
    uint16_t patAddr = bgTable + (uint16_t)tileIndex * 16 + fineY;
    const uint8_t *clo = chrMap[(patAddr >> 10) & 7];           // inline CHR fetch (no fn call)
    uint8_t lo = clo ? clo[patAddr & 0x3FF] : 0;
    uint8_t hi = clo ? clo[(patAddr + 8) & 0x3FF] : 0;
    if (px >= 8 && px <= 248) {                  // tile fully on-screen and past the left mask
      uint8_t *d = fb + px;                       // -> unrolled, no per-pixel bounds/mask checks
      bool    *op = bgOpaque + px;
      #define NES_BGPX(b, col) do { int pix = ((lo >> (col)) & 1) | (((hi >> (col)) & 1) << 1); \
                                    if (pix) { d[b] = pal[pix] & 0x3F; op[b] = true; } } while (0)
      NES_BGPX(0,7); NES_BGPX(1,6); NES_BGPX(2,5); NES_BGPX(3,4);
      NES_BGPX(4,3); NES_BGPX(5,2); NES_BGPX(6,1); NES_BGPX(7,0);
      #undef NES_BGPX
    } else {                                      // edge tiles (off-screen / left-mask): full checks
      for (int b = 0; b < 8; b++) {
        int sx = px + b;
        if (sx < 0 || sx >= 256) continue;
        if (sx < 8 && leftMask) continue;
        int col = 7 - b;
        int pix = ((lo >> col) & 1) | (((hi >> col) & 1) << 1);
        if (pix) { fb[sx] = pal[pix] & 0x3F; bgOpaque[sx] = true; }
      }
    }
    px += 8;
    if ((vv & 0x001F) == 31) {                                  // coarse X wrap -> next NT
      vv &= ~0x001F; vv ^= 0x0400;
      ntOff = mirrorAddr(0x2000 | (vv & 0x0FFF));
    } else { vv++; ntOff++; }
  }
}

static void renderSprites(int line, uint8_t *fb) {
  int h = (ctrl & 0x20) ? 16 : 8;
  uint16_t sprTable = (ctrl & 0x08) ? 0x1000 : 0x0000;  // 8x8 sprite pattern table
  memset(spriteDrawn, 0, sizeof(spriteDrawn));
  int count = 0;
  for (int i = 0; i < 64; i++) {
    int sy = oam[i * 4 + 0];
    int row = line - sy - 1;                            // OAM Y = top-1; visible sy+1..sy+h
    if (row < 0 || row >= h) continue;
    if (++count > 8) { status |= 0x20; break; }         // sprite overflow (approximate)
    uint8_t tile = oam[i * 4 + 1];
    uint8_t attr = oam[i * 4 + 2];
    int sx = oam[i * 4 + 3];
    int palBase = 0x10 + (attr & 3) * 4;
    bool flipH = attr & 0x40, flipV = attr & 0x80;
    bool front = !(attr & 0x20);
    int r = flipV ? (h - 1 - row) : row;
    uint16_t patAddr;
    if (h == 8) {
      patAddr = sprTable + (uint16_t)tile * 16 + r;
    } else {                                            // 8x16: bit0 of tile selects table
      uint16_t table = (tile & 1) ? 0x1000 : 0x0000;
      uint8_t t8 = tile & 0xFE;
      if (r >= 8) { t8 += 1; r -= 8; }
      patAddr = table + (uint16_t)t8 * 16 + r;
    }
    const uint8_t *clo = chrMap[(patAddr >> 10) & 7];   // inline CHR fetch (no fn call)
    uint8_t lo = clo ? clo[patAddr & 0x3FF] : 0;
    uint8_t hi = clo ? clo[(patAddr + 8) & 0x3FF] : 0;
    bool edge = sx < 0 || sx > 248 || (sx < 8 && !(mask & 0x04)); // needs per-pixel bounds/mask
    for (int b = 0; b < 8; b++) {
      int col = flipH ? b : 7 - b;
      int pix = ((lo >> col) & 1) | (((hi >> col) & 1) << 1);
      if (!pix) continue;
      int screenX = sx + b;
      if (edge) {                                       // off-screen / left-mask sprites only
        if (screenX < 0 || screenX >= 256) continue;
        if (screenX < 8 && !(mask & 0x04)) continue;    // sprites hidden in left 8px
      }
      if (spriteDrawn[screenX]) continue;               // lower-index sprite wins
      spriteDrawn[screenX] = true;
      if (i == 0 && bgOpaque[screenX] && (mask & 0x08) && screenX != 255) status |= 0x40; // spr0 hit
      if (front || !bgOpaque[screenX])
        fb[screenX] = paletteRam[palBase + pix] & 0x3F;
    }
  }
}

static void renderScanline(int line) {
  uint8_t *fb = framebuffer + line * 256;
  uint8_t backdrop = paletteRam[0] & 0x3F;
  memset(fb, backdrop, 256);                  // word-at-a-time fills (vs per-byte loops)
  memset(bgOpaque, 0, sizeof(bgOpaque));
  if (mask & 0x08) renderBg(line, fb);
  if (mask & 0x10) renderSprites(line, fb);
}

// Finish the scanline that just consumed its 341 dots, then advance to the next.
void endScanline() {
  if (scanline < 240) {                          // visible line
    renderScanline(scanline);
    if (renderingEnabled()) { mapperClockScanline(); incrementY(); reloadHorizontal(); }
  } else if (scanline == 261) {                  // pre-render line
    if (renderingEnabled()) v = t;               // full vertical+horizontal reload
  }

  scanline++;
  if (scanline > 261) { scanline = 0; frameReady = true; nesFrameCount++; }

  if (scanline == 241) {                         // start of VBlank
    status |= 0x80;
    if (ctrl & 0x80) nmiPending = true;
  } else if (scanline == 261) {                  // start of pre-render: clear flags
    status &= ~0xE0;
  }
}

void ppuStep(int cpuCycles) {
  dotAcc += cpuCycles * 3;
  while (dotAcc >= 341) { dotAcc -= 341; endScanline(); }
}

void ppuReset() {
  ctrl = mask = status = oamAddr = 0;
  v = t = 0; x = 0; w = false; readBuffer = 0;
  scanline = 0; dotAcc = 0;
  nmiPending = false; frameReady = false; dmaStallCycles = 0;
  if (vram) memset(vram, 0, 0x800);
  memset(oam, 0, sizeof(oam));
  memset(paletteRam, 0, sizeof(paletteRam));
}

} // namespace nes
