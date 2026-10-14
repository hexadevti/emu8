#include "../../emu.h"
#include "c64.h"

// C64 keyboard matrix (8 columns x 8 rows). A pressed key clears its row bit in the
// selected column. CIA1 reads this; the touch keyboard / joystick set it.

namespace c64 {

static uint8_t keyMatrix[8];   // [col] : bit row = 0 when pressed
static uint8_t joyPort2 = 0xff; // active-low joystick on CIA1 port A (PRA / $DC00)
static uint8_t joyPort1 = 0xff; // active-low joystick on CIA1 port B (PRB / $DC01)

void kbReset() {
  for (int i = 0; i < 8; i++) keyMatrix[i] = 0xff;
  joyPort2 = joyPort1 = 0xff;
}

void kbSetKey(uint8_t row, uint8_t col, bool down) {
  if (row > 7 || col > 7) return;
  if (down) keyMatrix[col] &= ~(1 << row);
  else      keyMatrix[col] |=  (1 << row);
}

// CIA1 PRB read: AND the row bytes of every column selected (low) in PRA, then mask in
// joystick port 1.
uint8_t kbReadRows(uint8_t colSelect) {
  uint8_t result = 0xff;
  for (int c = 0; c < 8; c++)
    if (!((colSelect >> c) & 1)) result &= keyMatrix[c];
  return result & joyPort1;
}

// CIA1 PRA read (reverse scan): AND the column bytes for every row selected in PRB,
// then mask in the joystick (port 2).
uint8_t kbReadCols(uint8_t rowSelect) {
  uint8_t result = 0xff;
  for (int r = 0; r < 8; r++)
    if (!((rowSelect >> r) & 1))
      for (int c = 0; c < 8; c++)
        if (!((keyMatrix[c] >> r) & 1)) result &= ~(1 << c);
  return result & joyPort2;
}

void kbSetJoystick(uint8_t mask) { joyPort2 = mask; }

// Route the joystick to port 1 or 2; release the other so switching ports never sticks.
void kbSetJoystickPort(uint8_t port, uint8_t mask) {
  if (port == 1) { joyPort1 = mask; joyPort2 = 0xff; }
  else           { joyPort2 = mask; joyPort1 = 0xff; }
}

} // namespace c64
