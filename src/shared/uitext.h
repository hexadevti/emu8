// uitext.h -- the menu text renderer shared by the desktop (display_sdl.cpp) and PicoCalc
// (display_picocalc.cpp) backends, so the two draw byte-identical text. Header-only: each backend
// passes its own pixel sink, and nothing here allocates or touches hardware.
//
// The fonts are fixed-cell X11 bitmaps (fonts/uifont.h) drawn 1:1 -- the old renderer shrank
// FreeSans 9pt by 2/3 or 4/5, which smeared strokes and gave widths that were hard to predict.
// Every glyph advances a whole number of pixels, so a label's width is simply strlen * advance and
// callers can budget a button exactly. Font numbers keep their TFT_eSPI meaning:
//   font 1  -- 6x10        dense rows: option labels, help rows, file rows on 320x240
//   font 3  -- 7x13        the PicoCalc's taller file list (TFT_eSPI has no font 3)
//   font 2+ -- 7x13 bold   titles and buttons (1px horizontal smear, 8px advance)
#pragma once

#include <stdint.h>
#include "fonts/uifont.h"

struct UiFont {
  const uint8_t *bits;   // 95 glyphs (0x20..0x7E), `rows` bytes each
  uint8_t rows;          // glyph rows stored
  uint8_t adv;           // pixels from one character to the next
  uint8_t r0;            // first row of the alignment box (rows above it are always blank)
  uint8_t boxH;          // alignment box height: capitals sit in it, descenders hang below
  bool    bold;
};

static inline const UiFont &uiFont(uint8_t font) {
  static const UiFont small = { UIFONT_6X10, 10, 6, 0,  9, false };
  static const UiFont list  = { UIFONT_7X13, 13, 7, 1, 11, false };
  static const UiFont big   = { UIFONT_7X13, 13, 8, 1, 11, true  };
  return font == 3 ? list : font >= 2 ? big : small;
}

static inline int uiTextWidth(const char *s, uint8_t font) {
  int n = 0;
  if (s) while (*s++) n++;
  return n * uiFont(font).adv;
}

// Height of the cell a string occupies (alignment box plus descenders): what a background fill covers.
static inline int uiTextCellH(uint8_t font) { const UiFont &f = uiFont(font); return f.rows - f.r0; }

// TFT_eSPI datum (0..8: TL TC TR ML MC MR BL BC BR) -> top-left corner of the alignment box.
// Vertical alignment uses the font box, not the ink of this particular string, so labels in a row
// of buttons share a baseline whether or not they contain capitals or descenders.
static inline void uiTextOrigin(const char *s, int32_t x, int32_t y, uint8_t datum, uint8_t font,
                                int32_t &left, int32_t &top) {
  const UiFont &f = uiFont(font);
  const int w = uiTextWidth(s, font), horiz = datum % 3, vert = datum / 3;
  left = horiz == 1 ? x - w / 2 : horiz == 2 ? x - w : x;
  top  = vert  == 1 ? y - f.boxH / 2 : vert == 2 ? y - f.boxH : y;
}

// Walk the set pixels of `s` with its alignment box at (left, top). Rows are the outer loop and
// pixels arrive left to right, so a sink that coalesces horizontal runs (the PicoCalc's pxRun)
// gets the longest runs possible. plot(x, y) is called once per lit pixel.
template <class Plot>
static inline void uiTextDraw(const char *s, int32_t left, int32_t top, uint8_t font, Plot plot) {
  if (!s) return;
  const UiFont &f = uiFont(font);
  for (int r = f.r0; r < f.rows; r++) {
    const int32_t y = top + r - f.r0;
    int32_t x = left;
    for (const char *p = s; *p; p++, x += f.adv) {
      uint8_t c = (uint8_t)*p;
      if (c < 0x20 || c > 0x7E) c = '?';
      uint8_t bits = f.bits[(c - 0x20) * f.rows + r];
      if (!bits) continue;
      uint16_t row = bits;
      if (f.bold) row |= (uint16_t)(bits >> 1);       // smear right: 7px glyph -> 8px bold
      for (int b = 0; b < 8; b++)
        if (row & (0x80 >> b)) plot(x + b, y);
    }
  }
}
