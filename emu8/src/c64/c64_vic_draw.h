// VIC-II line renderer (ported from C64Esp32 VIC.ino). Included TWICE by c64_vic.cpp:
//  - in namespace c64, on the emulator core, where it only runs for the collision registers;
//  - in namespace c64::rend, on the render core, which draws every visible line from the
//    per-line VIC state the emulator core recorded (see c64_vic.cpp).
// Each copy resolves vicreg / bitmap / vicColl / spritedatacoll / ... to its own namespace's
// variables, and ram / colormap to the shared C64 memory. VIC_SCREENMAP (the screen matrix) and
// VIC_SPRPTR(nr) (sprite pointer nr) are defined by the includer. Deliberately no include guard.

// The framebuffer is 8-bit indexed now, so the renderer stores raw C64 colour indices
// (0-15) instead of RGB565. tftColorFromC64ColorArr is therefore an identity table; the
// index->RGB565 conversion happens once, when c64RenderFrame pushes to the TFT.
static const uint8_t c64ColorIdentity[16] =
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const uint8_t *tftColorFromC64ColorArr = c64ColorIdentity;

static bool collArr[4] = {false, true, true, true};

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

static VIC_DRAW_FN void drawblankline(uint8_t line) {
  uint16_t framecol = tftColorFromC64ColorArr[vicreg[0x20] & 15];
  uint16_t idx = 0;            // line-relative (bitmap points at this scanline's start)
  for (uint16_t i = 0; i < 320; i++) {
    bitmap[idx++] = framecol;
  }
}

static VIC_DRAW_FN bool shiftDy(uint8_t line, int8_t dy, uint16_t bgcol) {
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

static VIC_DRAW_FN void shiftDx(uint8_t dx, uint16_t bgcol, uint16_t &idx) {
  for (uint8_t i = 0; i < dx; i++) {
    bitmap[idx++] = bgcol;
  }
}

static VIC_DRAW_FN void drawOnly38ColsFrame(uint16_t tmpidx) {
  bool only38cols = !(vicreg[0x16] & 8);
  if (only38cols) {
    uint16_t framecol = tftColorFromC64ColorArr[vicreg[0x20] & 15];
    for (uint8_t xp = 0; xp < 8; xp++) {
      bitmap[tmpidx++] = framecol;
    }
  }
}

static VIC_DRAW_FN void drawStdCharModeInt(uint8_t *screenMap, uint16_t bgcol, uint8_t row,
                             uint8_t dx, uint16_t &xp, uint16_t idxmap,
                             uint16_t &idx) {
  uint16_t col = tftColorFromC64ColorArr[colormap[idxmap] & 15];
  uint8_t ch = screenMap[idxmap];
  uint16_t idxch = ch << 3;
  uint8_t chardata = charset[idxch + row];
  drawByteStdData(chardata, idx, xp, col, bgcol, dx);
}

static VIC_DRAW_FN void drawStdCharMode(uint8_t *screenMap, uint8_t bgColor, int8_t dy,
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

static VIC_DRAW_FN void drawMCCharModeInt(uint8_t *screenMap, uint16_t bgcol,
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

static VIC_DRAW_FN void drawMCCharMode(uint8_t *screenMap, uint8_t bgColor, uint8_t color1,
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

static VIC_DRAW_FN void drawExtBGColCharModeInt(uint8_t *screenMap, uint8_t *bgColArr,
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

static VIC_DRAW_FN void drawExtBGColCharMode(uint8_t *screenMap, uint8_t *bgColArr, int8_t dy,
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

static VIC_DRAW_FN void drawMCBitmapModeInt(uint8_t *multicolorBitmap, uint8_t *colorMap1,
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

static VIC_DRAW_FN void drawMCBitmapMode(uint8_t *multicolorBitmap, uint8_t *colorMap1,
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

static VIC_DRAW_FN void drawStdBitmapModeInt(uint8_t *hiresBitmap, uint8_t *colorMap,
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

static VIC_DRAW_FN void drawStdBitmapMode(uint8_t *hiresBitmap, uint8_t *colorMap, int8_t dy,
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

static VIC_DRAW_FN void drawSpriteDataSC(uint8_t bitnr, int16_t xpos, uint8_t ypos,
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

static VIC_DRAW_FN void drawSpriteDataSCDS(uint8_t bitnr, int16_t xpos, uint8_t ypos,
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

static VIC_DRAW_FN void drawSpriteDataMC(uint8_t bitnr, int16_t xpos, uint8_t ypos,
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

static VIC_DRAW_FN void drawSpriteDataMCDS(uint8_t bitnr, int16_t xpos, uint8_t ypos,
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

static VIC_DRAW_FN void drawSprites(uint8_t line) {
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
        uint16_t dataaddr = VIC_SPRPTR(nr) * 64;
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


// Is a sprite on `line` (the renderer's sprite-line numbering)?
static VIC_DRAW_FN bool vicSpriteOnLine(uint16_t line) {
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

// Render raster line `rasterline` (51..250) into `bitmap` (320 colour indices) from vicreg,
// vertborder, lineC64map, screenmemstart, bitmapstart, vicmem and charset. vicColl must already be
// vicSpriteOnLine(rasterline - 1).
static VIC_DRAW_FN void vicDrawLineBody() {
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
      // Games use them to blank the screen while they update it.
      memset(bitmap, tftColorFromC64ColorArr[0], 320);
    } else if (bmm) {
      if (mcm) {
        drawMCBitmapMode(ram + bitmapstart, VIC_SCREENMAP,
                         vicreg[0x21], deltay - 3, deltax);
      } else {
        drawStdBitmapMode(ram + bitmapstart, VIC_SCREENMAP, deltay - 3,
                          deltax);
      }
    } else {
      if ((!ecm) && (!mcm)) {
        drawStdCharMode(VIC_SCREENMAP, vicreg[0x21], deltay - 3,
                        deltax);
      } else if ((!ecm) && mcm) {
        drawMCCharMode(VIC_SCREENMAP, vicreg[0x21], vicreg[0x22],
                       vicreg[0x23], deltay - 3, deltax);
      } else if (ecm && (!mcm)) {
        uint8_t bgColArr[] = {vicreg[0x21], vicreg[0x22], vicreg[0x23],
                              vicreg[0x24]};
        drawExtBGColCharMode(VIC_SCREENMAP, bgColArr, deltay - 3,
                             deltax);
      }
    }
    if (vicColl) drawSprites(rasterline - 1);   // no sprite on this line: nothing to draw or collide
  } else {
    drawblankline(rasterline - 51);
  }
}
