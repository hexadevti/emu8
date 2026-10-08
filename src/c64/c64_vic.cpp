#include "../../emu.h"
#if defined(BOARD_DESKTOP)
#include "../desktop/debug_bridge.h"   // dbgVicMarkFrame: tag VIC DMA reads in the heat map (no-op on device)
#endif
#include "c64.h"

// VIC-II renderer ported from C64Esp32 VIC.ino: renders each scanline into the RGB565
// `bitmap` framebuffer. All char/bitmap modes + sprites + collisions. Wrapped in
// namespace c64 so it doesn't collide with the Apple II core.

namespace c64 {

// The framebuffer is 8-bit indexed now, so the renderer stores raw C64 colour indices
// (0-15) instead of RGB565. tftColorFromC64ColorArr is therefore an identity table; the
// index->RGB565 conversion happens once, when c64RenderFrame pushes to the TFT.
static const uint8_t c64ColorIdentity[16] =
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const uint8_t *tftColorFromC64ColorArr = c64ColorIdentity;

// Start of display line `dline` (0..199) within the correct framebuffer half.
static inline uint8_t *vicLine(int dline) {
  return (dline < 100) ? (fbTop + dline * 320) : (fbBot + (dline - 100) * 320);
}

static bool collArr[4] = {false, true, true, true};

uint8_t spritespritecoll[320];
bool spritedatacoll[320];
uint8_t startbyte;
bool badlinecond;
bool vertborder;
uint8_t lineC64map;

// True while the current line has a sprite on it. Only then does the background renderer record
// its foreground pixels in spritedatacoll (drawSprites needs them for $D01F collisions and the
// sprite-behind-background priority); on every other line that per-pixel bookkeeping is skipped.
static bool vicColl = false;

// Set by read8 the first time the program reads a collision register ($D01E/$D01F); cleared on
// reset. Until then, and while the collision IRQs ($D01A bits 1-2) are off, nothing can observe a
// collision, so a skipped frame does not have to render its sprite lines just to detect them.
bool vicCollPolled = false;


// The two pixel writers every background mode ends in. They work on local pointers and write the
// indices back once: the framebuffer is uint8_t, and a store through a byte pointer may alias
// anything, so writing bitmap[idx++] through the idx/xp references made the compiler reload and
// re-store both around every single pixel.
static inline __attribute__((always_inline))
void drawByteStdData(uint8_t data, uint16_t &idx, uint16_t &xp,
                     uint16_t col, uint16_t bgcol, uint8_t dx) {
  uint8_t *p = bitmap + idx;
  const uint8_t c = (uint8_t)col, b = (uint8_t)bgcol;
  const uint8_t n = 8 - dx;
  if (n == 8 && !vicColl) {
    p[0] = (data & 0x80) ? c : b; p[1] = (data & 0x40) ? c : b;
    p[2] = (data & 0x20) ? c : b; p[3] = (data & 0x10) ? c : b;
    p[4] = (data & 0x08) ? c : b; p[5] = (data & 0x04) ? c : b;
    p[6] = (data & 0x02) ? c : b; p[7] = (data & 0x01) ? c : b;
  } else {
    bool *sc = spritedatacoll + xp;
    uint8_t bitval = 128;
    for (uint8_t i = 0; i < n; i++) {
      bool on = data & bitval;
      p[i] = on ? c : b;
      if (on && vicColl) sc[i] = true;
      bitval >>= 1;
    }
  }
  idx += n;
  xp += n;
}

static inline __attribute__((always_inline))
void drawByteMCData(uint8_t data, uint16_t &idx, uint16_t &xp,
                    uint16_t *tftColArr, bool *collArr, uint8_t dx) {
  uint8_t *p = bitmap + idx;
  bool *sc = spritedatacoll + xp;
  const uint8_t n = (8 - dx) >> 1;
  uint8_t bitshift = 6;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t bitpair = (data >> bitshift) & 0x03;
    uint8_t c = (uint8_t)tftColArr[bitpair];
    p[2 * i] = c;
    p[2 * i + 1] = c;
    if (vicColl) { sc[2 * i] = collArr[bitpair]; sc[2 * i + 1] = collArr[bitpair]; }
    bitshift -= 2;
  }
  idx += 2 * n;
  xp += 2 * n;
}

void drawblankline(uint8_t line) {
  uint16_t framecol = tftColorFromC64ColorArr[vicreg[0x20] & 15];
  uint16_t idx = 0;            // line-relative (bitmap points at this scanline's start)
  for (uint16_t i = 0; i < 320; i++) {
    bitmap[idx++] = framecol;
  }
}

bool shiftDy(uint8_t line, int8_t dy, uint16_t bgcol) {
  if ((line < dy) || (dy < line - 199)) {
    uint16_t idx = 0;            // line-relative (bitmap points at this scanline's start)
    uint16_t framecol = tftColorFromC64ColorArr[vicreg[0x20] & 15];
    bool only38cols = !(vicreg[0x16] & 8);
    if (only38cols) {
      for (uint8_t xp = 0; xp < 8; xp++) {
        bitmap[idx++] = framecol;
      }
      for (uint16_t xp = 8; xp < 39 * 8; xp++) {
        bitmap[idx++] = bgcol;
      }
      for (uint8_t xp = 0; xp < 8; xp++) {
        bitmap[idx++] = framecol;
      }
    } else {
      for (uint16_t xp = 0; xp < 40 * 8; xp++) {
        bitmap[idx++] = bgcol;
      }
    }
    return true;
  }
  return false;
}

void shiftDx(uint8_t dx, uint16_t bgcol, uint16_t &idx) {
  for (uint8_t i = 0; i < dx; i++) {
    bitmap[idx++] = bgcol;
  }
}

void drawOnly38ColsFrame(uint16_t tmpidx) {
  bool only38cols = !(vicreg[0x16] & 8);
  if (only38cols) {
    uint16_t framecol = tftColorFromC64ColorArr[vicreg[0x20] & 15];
    for (uint8_t xp = 0; xp < 8; xp++) {
      bitmap[tmpidx++] = framecol;
    }
  }
}

void drawStdCharModeInt(uint8_t *screenMap, uint16_t bgcol, uint8_t row,
                             uint8_t dx, uint16_t &xp, uint16_t idxmap,
                             uint16_t &idx) {
  uint16_t col = tftColorFromC64ColorArr[colormap[idxmap] & 15];
  uint8_t ch = screenMap[idxmap];
  uint16_t idxch = ch << 3;
  uint8_t chardata = charset[idxch + row];
  drawByteStdData(chardata, idx, xp, col, bgcol, dx);
}

void drawStdCharMode(uint8_t *screenMap, uint8_t bgColor, int8_t dy,
                          uint8_t dx) {
  uint16_t bgcol = tftColorFromC64ColorArr[bgColor & 15];
  uint8_t dline = rasterline - 51;
  if (shiftDy(dline, dy, bgcol)) {
    return;
  }
  uint16_t idx = 0;            // line-relative (bitmap points at this scanline's start)
  shiftDx(dx, bgcol, idx);
  uint8_t corrline = lineC64map - dy;
  uint8_t y = corrline >> 3;
  uint8_t row = corrline & 7;
  uint16_t idxmap = y * 40;
  uint16_t xp = 0;
  drawStdCharModeInt(screenMap, bgcol, row, 0, xp, idxmap++, idx);
  drawOnly38ColsFrame(idx - 8 - dx);
  for (uint8_t x = 1; x < 39; x++) {
    drawStdCharModeInt(screenMap, bgcol, row, 0, xp, idxmap++, idx);
  }
  drawStdCharModeInt(screenMap, bgcol, row, dx, xp, idxmap, idx);
  drawOnly38ColsFrame(idx - 8);
}

void drawMCCharModeInt(uint8_t *screenMap, uint16_t bgcol,
                            uint16_t *tftColArr, uint8_t row, uint8_t dx,
                            uint16_t &xp, uint16_t idxmap, uint16_t &idx) {
  uint8_t colc64 = colormap[idxmap] & 15;
  uint8_t ch = screenMap[idxmap];
  uint16_t idxch = ch << 3;
  uint8_t chardata = charset[idxch + row];
  if (colc64 & 8) {
    tftColArr[3] = tftColorFromC64ColorArr[colc64 & 7];
    drawByteMCData(chardata, idx, xp, tftColArr, collArr, dx);
  } else {
    drawByteStdData(chardata, idx, xp, tftColorFromC64ColorArr[colc64], bgcol,
                    dx);
  }
}

void drawMCCharMode(uint8_t *screenMap, uint8_t bgColor, uint8_t color1,
                         uint8_t color2, int8_t dy, uint8_t dx) {
  uint16_t bgcol = tftColorFromC64ColorArr[bgColor & 15];
  uint8_t dline = rasterline - 51;
  if (shiftDy(dline, dy, bgcol)) {
    return;
  }
  uint16_t idx = 0;            // line-relative (bitmap points at this scanline's start)
  shiftDx(dx, bgcol, idx);
  uint16_t tftColArr[4];
  tftColArr[0] = bgcol;
  tftColArr[1] = tftColorFromC64ColorArr[color1 & 15];
  tftColArr[2] = tftColorFromC64ColorArr[color2 & 15];
  uint8_t corrline = lineC64map - dy;
  uint8_t y = corrline >> 3;
  uint8_t row = corrline & 7;
  uint16_t idxmap = y * 40;
  uint16_t xp = 0;
  drawMCCharModeInt(screenMap, bgcol, tftColArr, row, 0, xp, idxmap++, idx);
  drawOnly38ColsFrame(idx - 8 - dx);
  for (uint8_t x = 1; x < 39; x++) {
    drawMCCharModeInt(screenMap, bgcol, tftColArr, row, 0, xp, idxmap++, idx);
  }
  drawMCCharModeInt(screenMap, bgcol, tftColArr, row, dx, xp, idxmap, idx);
  drawOnly38ColsFrame(idx - 8);
}

void drawExtBGColCharModeInt(uint8_t *screenMap, uint8_t *bgColArr,
                                  uint8_t row, uint8_t dx, uint16_t &xp,
                                  uint16_t idxmap, uint16_t &idx) {
  uint16_t col = tftColorFromC64ColorArr[colormap[idxmap] & 15];
  uint8_t ch = screenMap[idxmap];
  uint8_t ch6bits = ch & 0x3f;
  uint16_t bgcol = tftColorFromC64ColorArr[bgColArr[ch >> 6] & 15];
  uint16_t idxch = ch6bits << 3;
  uint8_t chardata = charset[idxch + row];
  drawByteStdData(chardata, idx, xp, col, bgcol, dx);
}

void drawExtBGColCharMode(uint8_t *screenMap, uint8_t *bgColArr, int8_t dy,
                               uint8_t dx) {
  uint8_t bgcol0 = tftColorFromC64ColorArr[bgColArr[0] & 15];   // $D021's high nibble is junk
  uint8_t dline = rasterline - 51;
  if (shiftDy(dline, dy, bgcol0)) {
    return;
  }
  uint16_t idx = 0;            // line-relative (bitmap points at this scanline's start)
  shiftDx(dx, bgcol0, idx);
  uint8_t corrline = lineC64map - dy;
  uint8_t y = corrline >> 3;
  uint8_t row = corrline & 7;
  uint16_t idxmap = y * 40;
  uint16_t xp = 0;
  drawExtBGColCharModeInt(screenMap, bgColArr, row, 0, xp, idxmap++, idx);
  drawOnly38ColsFrame(idx - 8 - dx);
  for (uint8_t x = 1; x < 39; x++) {
    drawExtBGColCharModeInt(screenMap, bgColArr, row, 0, xp, idxmap++, idx);
  }
  drawExtBGColCharModeInt(screenMap, bgColArr, row, dx, xp, idxmap, idx);
  drawOnly38ColsFrame(idx - 8);
}

void drawMCBitmapModeInt(uint8_t *multicolorBitmap, uint8_t *colorMap1,
                              uint16_t *tftColArr, uint16_t cidx,
                              uint16_t mcidx, uint8_t row, uint8_t dx,
                              uint16_t &xp, uint16_t &idx) {
  uint8_t color1 = colorMap1[cidx];
  uint8_t color2 = colormap[cidx];
  tftColArr[1] = tftColorFromC64ColorArr[(color1 >> 4) & 0x0f];
  tftColArr[2] = tftColorFromC64ColorArr[color1 & 0x0f];
  tftColArr[3] = tftColorFromC64ColorArr[color2 & 0x0f];
  uint8_t data = multicolorBitmap[mcidx + row];
  drawByteMCData(data, idx, xp, tftColArr, collArr, dx);
}

void drawMCBitmapMode(uint8_t *multicolorBitmap, uint8_t *colorMap1,
                           uint8_t backgroundColor, int8_t dy, uint8_t dx) {
  uint16_t tftColArr[4];
  tftColArr[0] = tftColorFromC64ColorArr[backgroundColor & 0x0f];
  uint8_t dline = rasterline - 51;
  if (shiftDy(dline, dy, tftColArr[0])) {
    return;
  }
  uint16_t idx = 0;            // line-relative (bitmap points at this scanline's start)
  shiftDx(dx, tftColArr[0], idx);
  uint8_t corrline = lineC64map - dy;
  uint8_t y = corrline >> 3;
  uint8_t row = corrline & 7;
  uint16_t cidx = y * 40;
  uint16_t mcidx = (y * 40) << 3;
  uint16_t xp = 0;
  drawMCBitmapModeInt(multicolorBitmap, colorMap1, tftColArr, cidx++, mcidx,
                      row, 0, xp, idx);
  mcidx += 8;
  drawOnly38ColsFrame(idx - 8 - dx);
  for (uint8_t x = 1; x < 39; x++) {
    drawMCBitmapModeInt(multicolorBitmap, colorMap1, tftColArr, cidx++, mcidx,
                        row, 0, xp, idx);
    mcidx += 8;
  }
  drawMCBitmapModeInt(multicolorBitmap, colorMap1, tftColArr, cidx, mcidx, row,
                      dx, xp, idx);
  drawOnly38ColsFrame(idx - 8);
}

void drawStdBitmapModeInt(uint8_t *hiresBitmap, uint8_t *colorMap,
                               uint16_t hiidx, uint16_t &colidx, uint8_t row,
                               uint8_t dx, uint16_t &xp, uint16_t &idx) {
  uint8_t color = colorMap[colidx++];
  uint8_t colorfg = (color & 0xf0) >> 4;
  uint8_t colorbg = color & 0x0f;
  uint16_t col = tftColorFromC64ColorArr[colorfg];
  uint16_t bgcol = tftColorFromC64ColorArr[colorbg];
  uint8_t data = hiresBitmap[hiidx + row];
  drawByteStdData(data, idx, xp, col, bgcol, dx);
}

void drawStdBitmapMode(uint8_t *hiresBitmap, uint8_t *colorMap, int8_t dy,
                            uint8_t dx) {
  // todo: background color is specific for each "tile"
  uint8_t dline = rasterline - 51;
  if (shiftDy(dline, dy, 0)) {
    return;
  }
  uint16_t idx = 0;            // line-relative (bitmap points at this scanline's start)
  shiftDx(dx, 0, idx);
  uint8_t corrline = lineC64map - dy;
  uint8_t y = corrline >> 3;
  uint8_t row = corrline & 7;
  uint16_t colidx = y * 40;
  uint16_t hiidx = (y * 40) << 3;
  uint16_t xp = 0;
  drawStdBitmapModeInt(hiresBitmap, colorMap, hiidx, colidx, row, 0, xp, idx);
  hiidx += 8;
  drawOnly38ColsFrame(idx - 8 - dx);
  for (uint8_t x = 1; x < 39; x++) {
    drawStdBitmapModeInt(hiresBitmap, colorMap, hiidx, colidx, row, 0, xp, idx);
    hiidx += 8;
  }
  drawStdBitmapModeInt(hiresBitmap, colorMap, hiidx, colidx, row, dx, xp, idx);
  drawOnly38ColsFrame(idx - 8);
}

void drawSpriteDataSC(uint8_t bitnr, int16_t xpos, uint8_t ypos,
                           uint8_t *data, uint8_t color) {
  uint8_t *const bm = bitmap;   // a local: stores through uint8_t* would otherwise reload it
  uint16_t tftcolor = tftColorFromC64ColorArr[color];
  uint16_t idx = xpos;         // line-relative; (uint16_t) wraps for xpos<0, ==0 at xpos==0
  for (uint8_t x = 0; x < 3; x++) {
    uint8_t d = *data++;
    uint8_t bitval = 128;
    for (uint8_t i = 0; i < 8; i++) {
      if (xpos < 0) {
        idx++;
        xpos++;
        continue;
      } else if (xpos >= 320) {
        return;
      }
      if (d & bitval) {
        uint8_t bgspriteprio = vicreg[0x1b] & bitnr;
        if (spritedatacoll[xpos]) {
          // sprite - data collision
          vicreg[0x1f] |= bitnr;
        }
        if (bgspriteprio && spritedatacoll[xpos]) {
          // background prio
          idx++;
        } else {
          bm[idx++] = tftcolor;
        }
        uint8_t sprcoll = spritespritecoll[xpos];
        if (sprcoll != 0) {
          // sprite - sprite collision
          vicreg[0x1e] |= sprcoll | bitnr;
        }
        spritespritecoll[xpos++] = sprcoll | bitnr;
      } else {
        idx++;
        xpos++;
      }
      bitval >>= 1;
    }
  }
}

void drawSpriteDataSCDS(uint8_t bitnr, int16_t xpos, uint8_t ypos,
                             uint8_t *data, uint8_t color) {
  uint8_t *const bm = bitmap;   // a local: stores through uint8_t* would otherwise reload it
  uint16_t tftcolor = tftColorFromC64ColorArr[color];
  uint16_t idx = xpos;         // line-relative; (uint16_t) wraps for xpos<0, ==0 at xpos==0
  for (uint8_t x = 0; x < 3; x++) {
    uint8_t d = *data++;
    uint8_t bitval = 128;
    for (uint8_t i = 0; i < 8; i++) {
      if (xpos < 0) {
        idx += 2;
        xpos += 2;
        continue;
      } else if (xpos >= 320) {
        return;
      }
      if (d & bitval) {
        uint8_t bgspriteprio = vicreg[0x1b] & bitnr;
        if (spritedatacoll[xpos] || spritedatacoll[xpos + 1]) {
          // sprite - data collision
          vicreg[0x1f] |= bitnr;
        }
        if (bgspriteprio && spritedatacoll[xpos]) {
          // background prio
          idx++;
        } else {
          bm[idx++] = tftcolor;
        }
        if (bgspriteprio && spritedatacoll[xpos + 1]) {
          // background prio
          idx++;
        } else {
          bm[idx++] = tftcolor;
        }
        uint8_t sprcoll = spritespritecoll[xpos];
        if (sprcoll != 0) {
          // sprite - sprite collision
          vicreg[0x1e] |= sprcoll | bitnr;
        }
        spritespritecoll[xpos++] = sprcoll | bitnr;
      } else {
        idx += 2;
        xpos += 2;
      }
      bitval >>= 1;
    }
  }
}

static inline __attribute__((always_inline))
void drawSpriteDataMC2Bits(uint8_t idxc, uint16_t &idx, int16_t &xpos,
                                uint8_t bitnr, uint16_t *tftcolor) {
  uint8_t *const bm = bitmap;   // a local: stores through uint8_t* would otherwise reload it
  if (xpos < 0) {
    idx += 2;
    xpos += 2;
    return;
  } else if (xpos >= 320) {
    return;
  }
  if (idxc) {
    uint8_t bgspriteprio = vicreg[0x1b] & bitnr;
    if (spritedatacoll[xpos] || spritedatacoll[xpos + 1]) {
      // sprite - data collision
      vicreg[0x1f] |= bitnr;
    }
    if (bgspriteprio && spritedatacoll[xpos]) {
      // background prio
      idx++;
    } else {
      bm[idx++] = tftcolor[idxc];
    }
    if (bgspriteprio && spritedatacoll[xpos + 1]) {
      // background prio
      idx++;
    } else {
      bm[idx++] = tftcolor[idxc];
    }
    uint8_t bitnrcollxpos0 = spritespritecoll[xpos];
    uint8_t bitnrcollxpos1 = spritespritecoll[xpos + 1];
    if (bitnrcollxpos0 != 0) {
      // sprite - sprite collision
      vicreg[0x1e] |= bitnrcollxpos0 | bitnr;
    }
    if (bitnrcollxpos1 != 0) {
      // sprite - sprite collision
      vicreg[0x1e] |= bitnrcollxpos1 | bitnr;
    }
    spritespritecoll[xpos++] = bitnrcollxpos0 | bitnr;
    spritespritecoll[xpos++] = bitnrcollxpos1 | bitnr;
  } else {
    idx += 2;
    xpos += 2;
  }
}

void drawSpriteDataMC(uint8_t bitnr, int16_t xpos, uint8_t ypos,
                           uint8_t *data, uint8_t color10, uint8_t color01,
                           uint8_t color11) {
  uint16_t tftcolor[4] = {0, tftColorFromC64ColorArr[color01],
                          tftColorFromC64ColorArr[color10],
                          tftColorFromC64ColorArr[color11]};
  uint16_t idx = xpos;         // line-relative; (uint16_t) wraps for xpos<0, ==0 at xpos==0
  for (uint8_t x = 0; x < 3; x++) {
    uint8_t d = *data++;
    uint8_t idxc = (d & 192) >> 6;
    drawSpriteDataMC2Bits(idxc, idx, xpos, bitnr, tftcolor);
    idxc = (d & 48) >> 4;
    drawSpriteDataMC2Bits(idxc, idx, xpos, bitnr, tftcolor);
    idxc = (d & 12) >> 2;
    drawSpriteDataMC2Bits(idxc, idx, xpos, bitnr, tftcolor);
    idxc = (d & 3);
    drawSpriteDataMC2Bits(idxc, idx, xpos, bitnr, tftcolor);
  }
}

void drawSpriteDataMCDS(uint8_t bitnr, int16_t xpos, uint8_t ypos,
                             uint8_t *data, uint8_t color10, uint8_t color01,
                             uint8_t color11) {
  uint16_t tftcolor[4] = {0, tftColorFromC64ColorArr[color01],
                          tftColorFromC64ColorArr[color10],
                          tftColorFromC64ColorArr[color11]};
  uint16_t idx = xpos;         // line-relative; (uint16_t) wraps for xpos<0, ==0 at xpos==0
  for (uint8_t x = 0; x < 3; x++) {
    uint8_t d = *data++;
    uint8_t idxc = (d & 192) >> 6;
    drawSpriteDataMC2Bits(idxc, idx, xpos, bitnr, tftcolor);
    xpos -= 2;
    drawSpriteDataMC2Bits(idxc, idx, xpos, bitnr, tftcolor);
    idxc = (d & 48) >> 4;
    drawSpriteDataMC2Bits(idxc, idx, xpos, bitnr, tftcolor);
    xpos -= 2;
    drawSpriteDataMC2Bits(idxc, idx, xpos, bitnr, tftcolor);
    idxc = (d & 12) >> 2;
    drawSpriteDataMC2Bits(idxc, idx, xpos, bitnr, tftcolor);
    xpos -= 2;
    drawSpriteDataMC2Bits(idxc, idx, xpos, bitnr, tftcolor);
    idxc = (d & 3);
    drawSpriteDataMC2Bits(idxc, idx, xpos, bitnr, tftcolor);
    xpos -= 2;
    drawSpriteDataMC2Bits(idxc, idx, xpos, bitnr, tftcolor);
  }
}

void drawSprites(uint8_t line) {
  uint8_t spritesenabled = vicreg[0x15];
  uint8_t spritesdoubley = vicreg[0x17];
  uint8_t spritesdoublex = vicreg[0x1d];
  uint8_t multicolorreg = vicreg[0x1c];
  uint8_t color01 = vicreg[0x25] & 0x0f;
  uint8_t color11 = vicreg[0x26] & 0x0f;
  memset(spritespritecoll, 0, sizeof(spritespritecoll));
  const uint8_t coll1e = vicreg[0x1e], coll1f = vicreg[0x1f];   // IRQ only on the first hit
  uint8_t bitval = 128;
  for (int8_t nr = 7; nr >= 0; nr--) {
    if (spritesenabled & bitval) {
      uint8_t facysize = (spritesdoubley & bitval) ? 2 : 1;
      uint16_t y = vicreg[0x01 + nr * 2];
      if ((line >= y) && (line < (y + 21 * facysize))) {
        int16_t x = vicreg[0x00 + nr * 2] - 24;
        if (vicreg[0x10] & bitval) {
          x += 256;
        }
        uint8_t ypos = line - 50;
        uint16_t dataaddr = ram[screenmemstart + 1016 + nr] * 64;
        uint8_t *data = ram + vicmem + dataaddr + ((line - y) / facysize) * 3;
        uint8_t col = vicreg[0x27 + nr] & 0x0f;
        if (multicolorreg & bitval) {
          if (spritesdoublex & bitval) {
            drawSpriteDataMCDS(bitval, x, ypos, data, col, color01, color11);
          } else {
            drawSpriteDataMC(bitval, x, ypos, data, col, color01, color11);
          }
        } else {
          if (spritesdoublex & bitval) {
            drawSpriteDataSCDS(bitval, x, ypos, data, col);
          } else {
            drawSpriteDataSC(bitval, x, ypos, data, col);
          }
        }
      }
    }
    bitval >>= 1;
  }
  if (vicreg[0x1f] != 0 && coll1f == 0) {   // a register already non-zero raises no new IRQ
    if (vicreg[0x1a] & 2) {
      vicreg[0x19] |= 0x82;
    } else {
      vicreg[0x19] |= 0x02;
    }
  }
  if (vicreg[0x1e] != 0 && coll1e == 0) {   // a register already non-zero raises no new IRQ
    if (vicreg[0x1a] & 4) {
      vicreg[0x19] |= 0x84;
    } else {
      vicreg[0x19] |= 0x04;
    }
  }
}

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
  // Framebuffer = the shared static 64K buffer (also the Apple main RAM; platforms are
  // mutually exclusive). Two 32016-byte halves; +16 padding each absorbs a double-width
  // sprite's ~1px right-edge spill. Static -> always available, no heap-fragmentation failure.
  fbTop  = sharedBigBuf;
  fbBot  = sharedBigBuf + (320 * 100 + 16);
  bitmap = fbTop;                        // non-null = graphics enabled (drawRasterline gate)

  if (!colormap) colormap = (uint8_t *)malloc(1024);   // color RAM ($D800)
  initVarsAndRegs();
}

void checkFrameColor() {
  uint8_t framecol = vicreg[0x20] & 15;
  if (framecol != syncd020) {
    syncd020 = framecol;
    drawFrame(tftColorFromC64ColorArr[framecol]);
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
  // badline?
  if (((vicreg[0x11] & 7) == (raster7 & 7)) && badlinecond &&
      (raster7 >= 0x30) && (raster7 <= 0xf7)) {
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

// Frame pacing against the panel (PicoCalc). Pushing the 320x200 picture takes the render task on
// the other core ~60 ms, three to four PAL frames. The VIC used to render one frame in three
// regardless, so every image the panel took was stitched from two frames at a moving seam --
// sprites and scrolling split and jumped. Now the two take turns: the VIC renders a frame only
// once the panel has taken the previous one (vicFbState FREE -> WRITING -> DONE), and the render
// task pushes each band as soon as the VIC has passed it (vicLinesDone), then hands the buffer
// back. Every image is one whole frame, and the VIC renders no frame nobody sees.
// A frame not drawn still renders any line a sprite is on, into a scratch line: the collision
// registers ($D01E/$D01F) are set by the renderer, and a game polling them must not miss a hit.
volatile uint8_t vicFbState = VIC_FB_FREE;
volatile uint8_t vicLinesDone = 0;   // display lines (0..200) of the WRITING frame complete
volatile uint32_t vicFrameNo = 0;    // TEMP debug: PAL frames since boot (serial fb dump)
volatile uint32_t vicDrawnFrameNo = 0;
static bool vicDrawFrame = true;     // this frame goes to the framebuffer
// At most one frame in VIC_MIN_FRAMES is drawn, even when the panel is quick to take it (the render
// task only sends the bands that changed, so a mostly-still screen goes out in a few ms): rendering
// is time taken from the 6510. Three keeps the cadence steady (a scroller moves by the same step
// every image) and odd, so a game that flickers sprites on alternate frames shows both sets.
#define VIC_MIN_FRAMES 3
static uint8_t vicSinceDraw = VIC_MIN_FRAMES;

static bool vicSpriteOnLine(uint16_t line) {
  uint8_t en = vicreg[0x15];
  if (!en) return false;
  for (uint8_t nr = 0; nr < 8; nr++) {
    if (!(en & (1 << nr))) continue;
    uint16_t y = vicreg[0x01 + nr * 2];
    uint16_t h = (vicreg[0x17] & (1 << nr)) ? 42 : 21;
    if (line >= y && line < y + h) return true;
  }
  return false;
}

void drawRasterline() {
  if (rasterline == 51) {
#if defined(BOARD_PICOCALC)
    vicFrameNo++;
    if (vicSinceDraw < VIC_MIN_FRAMES) vicSinceDraw++;
    vicDrawFrame = (vicSinceDraw >= VIC_MIN_FRAMES) && (vicFbState == VIC_FB_FREE);
    if (vicDrawFrame) { vicSinceDraw = 1; vicLinesDone = 0; vicDrawnFrameNo = vicFrameNo; vicFbState = VIC_FB_WRITING; }
#else
    vicDrawFrame = true;
#endif
  }
  if ((rasterline >= 51) && (rasterline < 251)) {
    vicColl = vicSpriteOnLine(rasterline - 1);
    const bool collWatched = vicCollPolled || (vicreg[0x1a] & 0x06);
    if (!vicDrawFrame && !(vicColl && collWatched)) {
      lineC64map++;                      // keep the character-row counter in step
      return;
    }
    // A frame not drawn renders into a scratch line: it only needs the collision bits, and the
    // framebuffer may be on its way to the panel.
    static uint8_t scratchLine[320 + 16];   // +16: double-width sprite spill, as in the fb halves
    bitmap = vicDrawFrame ? vicLine(rasterline - 51) : scratchLine;
    if (!vertborder) {
      uint8_t d011 = vicreg[0x11];
      uint8_t deltay = d011 & 7;
      if (vicColl) memset(spritedatacoll, false, sizeof(spritedatacoll));
      uint8_t d016 = vicreg[0x16];
      uint8_t deltax = d016 & 7;
      bool bmm = d011 & 32;
      bool ecm = d011 & 64;
      bool mcm = d016 & 16;
      if (ecm && (bmm || mcm)) {
        // Invalid modes (ECM with BMM and/or MCM): the VIC outputs black, and sprites still show.
        // Before this, ECM+MCM drew nothing (stale pixels) and ECM+BMM drew a normal bitmap.
        // Games use them to blank the screen while they update it.
        memset(bitmap, tftColorFromC64ColorArr[0], 320);
      } else if (bmm) {
        if (mcm) {
          drawMCBitmapMode(ram + bitmapstart, ram + screenmemstart,
                           vicreg[0x21], deltay - 3, deltax);
        } else {
          drawStdBitmapMode(ram + bitmapstart, ram + screenmemstart, deltay - 3,
                            deltax);
        }
      } else {
        if ((!ecm) && (!mcm)) {
          drawStdCharMode(ram + screenmemstart, vicreg[0x21], deltay - 3,
                          deltax);
        } else if ((!ecm) && mcm) {
          drawMCCharMode(ram + screenmemstart, vicreg[0x21], vicreg[0x22],
                         vicreg[0x23], deltay - 3, deltax);
        } else if (ecm && (!mcm)) {
          uint8_t bgColArr[] = {vicreg[0x21], vicreg[0x22], vicreg[0x23],
                                vicreg[0x24]};
          drawExtBGColCharMode(ram + screenmemstart, bgColArr, deltay - 3,
                               deltax);
        }
      }
      if (vicColl) drawSprites(rasterline - 1);   // no sprite on this line: nothing to draw or collide
    } else {
      drawblankline(rasterline - 51);
    }
    lineC64map++;
#if defined(BOARD_PICOCALC)
    if (vicDrawFrame) {
      __asm volatile("dmb" ::: "memory");   // the line's pixels land before the render task sees it
      vicLinesDone = rasterline - 50;
      if (rasterline == 250) vicFbState = VIC_FB_DONE;
    }
#endif
  }
}

} // namespace c64
