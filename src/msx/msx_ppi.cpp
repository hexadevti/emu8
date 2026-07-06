// msx_ppi.cpp - Intel 8255 PPI for the MSX1 core (ports $A8-$AB).
//   port A ($A8, out): primary-slot select - 2 bits per 16K page (drives the memory map in
//                      msx_machine.cpp via ppiPageSlot()).
//   port B ($A9, in) : reads the 8 columns (active-low) of the keyboard row selected on port C.
//   port C ($AA, out): low nibble = keyboard row select (0-10); bit4 CAPS LED; bit7 keyboard click.
//   control ($AB)    : mode set (ignored) or bit-set/reset of a port C bit (used for CAPS/click).
//
// The MSX keyboard is an 8 (columns) x 11 (rows) matrix; kbSetKey/kbReset maintain it and the
// touch/USB keyboards drive it through msxKeyMatrix (see msx.cpp).

#include "msx.h"

namespace msx {

static uint8_t ppiA;          // slot-select register
static uint8_t ppiC;          // row select + CAPS/click
static uint8_t keyRows[11];   // active-low: bit clear = key pressed

void ppiReset() { ppiA = 0; ppiC = 0; kbReset(); }
void kbReset()  { for (int i = 0; i < 11; i++) keyRows[i] = 0xFF; }

int ppiPageSlot(int page) { return (ppiA >> ((page & 3) * 2)) & 3; }

uint8_t ppiRead(uint8_t port) {
  switch (port) {
    case 0xA8: return ppiA;
    case 0xA9: { int row = ppiC & 0x0F; return (row < 11) ? keyRows[row] : 0xFF; }   // keyboard columns
    case 0xAA: return ppiC;
    default:   return 0xFF;
  }
}

void ppiWrite(uint8_t port, uint8_t v) {
  switch (port) {
    case 0xA8: ppiA = v; break;
    case 0xAA: ppiC = v; break;
    case 0xAB:                                   // control word
      if (v & 0x80) { /* mode set: MSX always uses A/C out, B in - nothing to do */ }
      else {                                     // bit-set/reset of an individual port C bit
        int bit = (v >> 1) & 0x07; bool set = v & 1;
        if (set) ppiC |= (1 << bit); else ppiC &= ~(1 << bit);
      }
      break;
    default: break;
  }
}

void kbSetKey(int row, int col, bool down) {
  if (row < 0 || row >= 11 || col < 0 || col >= 8) return;
  if (down) keyRows[row] &= ~(1 << col);
  else      keyRows[row] |=  (1 << col);
}

} // namespace msx
