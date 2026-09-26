// usbkeyboard.cpp - USB-HID host KEYBOARD input for the JC4827W543 (ESP32-S3 native USB).
//
// Companion to usbgamepad.cpp: the same EspUsbHost instance that decodes the SNES pad also
// receives boot-protocol keyboard reports. Its onKeyboard() override forwards each report here,
// and we translate the standard 8-byte HID boot report (modifier byte + up to 6 keycodes) into
// whatever the currently-selected console expects:
//
//   * Apple II        : set the global keymem to an Apple keycode (high "key ready" bit set),
//                       exactly like the PS/2 and on-screen-keyboard paths. Left/Right ALT drive
//                       the open-/solid-apple paddle buttons (Pb0/Pb1).
//   * C64             : press/release the CIA1 keyboard matrix via c64KeyMatrix(row,col,down),
//                       with PC Shift/Ctrl/Alt mapped to the C64 SHIFT/CTRL/Commodore lines.
//   * NES             : map the d-pad + Z/X/Enter/Tab, or F4/F5 (A/B) and F2/F3
//                       (Start/Select), onto controller 1 (nesSetController()).
//   * Atari 2600      : map the d-pad + Space/Enter/Tab onto the stick + Fire/Reset/Select.
//
// Settings menu (every platform): F12 opens/closes it; while it is open the arrow keys navigate
// (Left/Right move the selection, Up/Down change the value) and Enter activates. F11 is a CPU
// reset on the Apple II.
//
// Build target only: BOARD_INPUT_USB (the JC4827W543). The CYD uses its PS/2 header instead.

#include "../../emu.h"

#if BOARD_INPUT_USB

#if BOARD_PANEL_DSI
#include "p4/usb/EspUsbHost.h"   // P4: vendored EspUsbHost fork, patched for IDF 5.x (in-repo)
#else
#include "EspUsbHost.h"   // S3: external fork — HID_KEY_* / KEYBOARD_MODIFIER_* + HID_KEYCODE_TO_ASCII
#endif

// --- small helpers -------------------------------------------------------------------------
static bool kbContains(const uint8_t *arr, uint8_t kc)
{
  for (int i = 0; i < 6; i++) if (arr[i] == kc) return true;
  return false;
}

// Does this report carry a key that TYPES -- i.e. anything but the four arrows? Used to tell a
// SHIFT that is modifying a keystroke from one held on its own (the PicoCalc's fire button).
static bool kbHasTypingKey(const uint8_t *arr)
{
  for (int i = 0; i < 6; i++) {
    if (!arr[i]) continue;
    if (arr[i] == HID_KEY_ARROW_UP || arr[i] == HID_KEY_ARROW_DOWN ||
        arr[i] == HID_KEY_ARROW_LEFT || arr[i] == HID_KEY_ARROW_RIGHT) continue;
    return true;
  }
  return false;
}

// US-layout HID keycode -> ASCII, reusing the library's conversion table.
static char kbToAscii(uint8_t kc, bool shift)
{
  static const uint8_t conv[128][2] = { HID_KEYCODE_TO_ASCII };
  if (kc >= 128) return 0;
  return (char)conv[kc][shift ? 1 : 0];
}

// ============================ Apple II =====================================================
// Only key-down matters: write an Apple keycode into keymem (bit7 = key available).
static void appleKeyDown(uint8_t kc, bool shift, bool ctrl)
{
  uint8_t code;
  switch (kc) {
    case HID_KEY_ARROW_LEFT:   code = 0x88; break;   // back / left
    case HID_KEY_ARROW_RIGHT:  code = 0x95; break;   // forward / right
    case HID_KEY_ARROW_UP:     code = 0x8B; break;
    case HID_KEY_ARROW_DOWN:   code = 0x8A; break;
    case HID_KEY_ENTER:
    case HID_KEY_KEYPAD_ENTER: code = 0x8D; break;
    case HID_KEY_ESCAPE:       code = 0x9B; break;
    case HID_KEY_TAB:          code = 0x89; break;
    case HID_KEY_BACKSPACE:    code = 0x88; break;   // backspace == left
    case HID_KEY_DELETE:       code = 0xFF; break;
    default: {
      char c = kbToAscii(kc, shift);
      if (c == 0) return;
      if (c >= 'a' && c <= 'z') c -= 32;                  // Apple keyboard is upper-case
      if (ctrl && c >= '@' && c <= '_') c = (char)(c & 0x1F);  // Ctrl masks to 0x00-0x1F
      code = (uint8_t)c | 0x80;
      break;
    }
  }
  keymem = (char)code;
}

// Apple II keyboard-as-joystick. With the JOYSTICK option ON the arrow keys deflect the paddles
// and Space is the button, exactly as the C64/MSX cores already do -- and exactly as the CYD's
// PS/2 path has always done (keyboardPs2.cpp), which is why this was the one input backend where
// turning JOYSTICK on did nothing at all on a USB/matrix keyboard.
//
// The Apple II has no digital stick: games read the PADDLE TIMERS at $C064/$C065, which
// processJoystick() ramps toward timerpdl0/timerpdl1 after a $C070 trigger. So a held arrow is a
// full deflection and neutral is centre -- the same three values usbgamepad.cpp writes.
// NOTHING is taken away from the keyboard when JOYSTICK is on. The arrows drive the paddles and
// still deliver 0x88/0x95/0x8B/0x8A to keymem, so a game that steers with the paddles and one that
// reads the arrow keys both work without leaving the menu to flip the mode -- and Space fires and
// still reaches keymem as 0xA0, for the same reason.
//
// Space used to be the exception, on the theory that typing a space into a running game was worse
// than useless. Karateka disproves it: pick its KEYBOARD control mode and Space is a control key,
// so with JOYSTICK on -- which is the DEFAULT (globals.cpp) -- every key worked except that one,
// with nothing on screen to suggest the emulator was eating it. A game that misreads a stray space
// is the milder failure, and it is the one the arrows already accept.

// The two paddle buttons. Alt / right-Alt are open-apple / solid-apple on a PC keyboard, but
// the PicoCalc has one Alt and no right-Alt, so F4 and F5 are the buttons there. They are
// continuous state (held, not tapped), which is why they are read from keys[] below rather
// than in the key-down loop, and they work whether or not JOYSTICK mode is on -- open-apple
// and solid-apple are ordinary Apple II keys, not just game buttons.
static bool appleIsButtonKey(uint8_t kc)
{
  return kc == HID_KEY_F4 || kc == HID_KEY_F5;
}

static void appleApplyJoystick(const uint8_t *keys)
{
  bool l = kbContains(keys, HID_KEY_ARROW_LEFT),  r = kbContains(keys, HID_KEY_ARROW_RIGHT);
  bool u = kbContains(keys, HID_KEY_ARROW_UP),    d = kbContains(keys, HID_KEY_ARROW_DOWN);
  timerpdl0 = l ? JOY_MIN : r ? JOY_MAX : JOY_MID;   // paddle 0 = X
  timerpdl1 = u ? JOY_MIN : d ? JOY_MAX : JOY_MID;   // paddle 1 = Y
  if (kbContains(keys, HID_KEY_SPACE)) Pb0 = true;   // fire (also open-apple; Alt still works)
}

// ============================ C64 keyboard matrix ========================================
// Map a HID keycode to a CIA1 matrix position in this codebase's (col,row) convention
// (the same one the touch keyboard feeds to c64KeyMatrix(row, col, down)). col < 0 = unmapped.
struct C64Pos { int8_t col, row; };
static C64Pos c64Map(uint8_t kc)
{
  switch (kc) {
    // letters
    case HID_KEY_A: return {1,2}; case HID_KEY_B: return {3,4}; case HID_KEY_C: return {2,4};
    case HID_KEY_D: return {2,2}; case HID_KEY_E: return {1,6}; case HID_KEY_F: return {2,5};
    case HID_KEY_G: return {3,2}; case HID_KEY_H: return {3,5}; case HID_KEY_I: return {4,1};
    case HID_KEY_J: return {4,2}; case HID_KEY_K: return {4,5}; case HID_KEY_L: return {5,2};
    case HID_KEY_M: return {4,4}; case HID_KEY_N: return {4,7}; case HID_KEY_O: return {4,6};
    case HID_KEY_P: return {5,1}; case HID_KEY_Q: return {7,6}; case HID_KEY_R: return {2,1};
    case HID_KEY_S: return {1,5}; case HID_KEY_T: return {2,6}; case HID_KEY_U: return {3,6};
    case HID_KEY_V: return {3,7}; case HID_KEY_W: return {1,1}; case HID_KEY_X: return {2,7};
    case HID_KEY_Y: return {3,1}; case HID_KEY_Z: return {1,4};
    // digits
    case HID_KEY_1: return {7,0}; case HID_KEY_2: return {7,3}; case HID_KEY_3: return {1,0};
    case HID_KEY_4: return {1,3}; case HID_KEY_5: return {2,0}; case HID_KEY_6: return {2,3};
    case HID_KEY_7: return {3,0}; case HID_KEY_8: return {3,3}; case HID_KEY_9: return {4,0};
    case HID_KEY_0: return {4,3};
    // controls / punctuation
    case HID_KEY_ENTER:
    case HID_KEY_KEYPAD_ENTER: return {0,1};   // RETURN
    case HID_KEY_BACKSPACE:
    case HID_KEY_DELETE:       return {0,0};   // INST/DEL
    case HID_KEY_SPACE:        return {7,4};
    case HID_KEY_MINUS:        return {5,3};   // -
    case HID_KEY_EQUAL:        return {6,5};   // =
    case HID_KEY_SEMICOLON:    return {6,2};   // ;
    case HID_KEY_APOSTROPHE:   return {5,5};   // : (nearest C64 key)
    case HID_KEY_GRAVE:        return {7,1};   // C64 left-arrow
    case HID_KEY_COMMA:        return {5,7};   // ,
    case HID_KEY_PERIOD:       return {5,4};   // .
    case HID_KEY_SLASH:        return {6,7};   // /
    case HID_KEY_BRACKET_LEFT: return {5,6};   // @
    case HID_KEY_BRACKET_RIGHT:return {6,1};   // *
    case HID_KEY_BACKSLASH:    return {5,0};   // +
    case HID_KEY_ESCAPE:       return {7,7};   // RUN/STOP
    // cursor keys (LEFT/UP add SHIFT, applied in c64ApplyModifiers)
    case HID_KEY_ARROW_RIGHT:
    case HID_KEY_ARROW_LEFT:   return {0,2};
    case HID_KEY_ARROW_DOWN:
    case HID_KEY_ARROW_UP:     return {0,7};
  }
  return {-1, -1};
}

static void c64KeyDown(uint8_t kc)
{
  C64Pos p = c64Map(kc);
  if (p.col >= 0) c64KeyMatrix(p.row, p.col, true);
}
static void c64KeyUp(uint8_t kc)
{
  C64Pos p = c64Map(kc);
  if (p.col >= 0) c64KeyMatrix(p.row, p.col, false);
}

// On the PicoCalc, either SHIFT key is fire as well: the cursor cluster sits at the bottom right and
// the space bar is a long reach from it, while the two shifts flank that same bottom row. The keyboard
// reports both shifts as one modifier bit (input_picocalc.cpp), so the plain `shift` flag covers left
// and right. Space still fires. Other boards have a full keyboard within reach of the arrows and
// keep SHIFT for typing only.
#if defined(BOARD_PICOCALC)
#define C64_SHIFT_IS_FIRE 1
#else
#define C64_SHIFT_IS_FIRE 0
#endif

// JOYSTICK mode takes NOTHING away from the C64 keyboard: the arrows, Space and (on the PicoCalc)
// SHIFT drive the stick AND still reach the matrix, the double duty the Apple II paddles already
// give their arrows. It used to hold the arrows exclusively, which left the C64 with no cursor keys
// at the BASIC prompt -- and since JOYSTICK is ON by default, the only way to get them back was to
// open the menu and turn it off. SHIFT was withheld for longer, which cost the PicoCalc every
// shifted character: no quotes for LOAD"$",8 and no upper case, with the same menu round trip as
// the only way out. It now does both jobs too (see c64ApplyModifiers).

// Refresh the modifier matrix lines from the live report each call. Cursor LEFT/UP are SHIFT+CRSR
// on the C64, so a held LEFT or UP also asserts SHIFT.
static void c64ApplyModifiers(bool shift, bool ctrl, bool alt, const uint8_t *keys)
{
  bool wantShift = shift;
  // The PicoCalc's SHIFT keys fire AND type. Both at once needs one distinction, because the two
  // jobs never happen in the same report: SHIFT pressed ALONE is a shot, SHIFT pressed WITH a
  // character key is a keystroke. So the C64's SHIFT line is asserted only when the report also
  // carries a typing key -- firing then shifts nothing, and the arrows don't count, so holding
  // fire while steering stays clean.
  //
  // This is exact rather than a guess, because of how the keyboard reports shift
  // (hidModifiers, input_picocalc.cpp): the bit is set from the CHARACTER (a vs A, which is also
  // the only thing that gets caps lock right) as well as from the physical key, so a shifted
  // character is never in flight without its own key in the same report.
  //
  // Dropped first, so the cursor-derived SHIFT below still gets applied.
  if (C64_SHIFT_IS_FIRE && joystick && !kbHasTypingKey(keys)) wantShift = false;
  // Cursor LEFT/UP are SHIFT+CRSR on the C64, so a held LEFT or UP also asserts SHIFT. The arrows
  // are cursor keys in every mode now, so this applies whether or not the stick is reading them.
  wantShift = wantShift || kbContains(keys, HID_KEY_ARROW_LEFT) || kbContains(keys, HID_KEY_ARROW_UP);
  c64KeyMatrix(7, 1, wantShift);  // left SHIFT  (col1,row7)
  c64KeyMatrix(2, 7, ctrl);       // CTRL        (col7,row2)
  c64KeyMatrix(5, 7, alt);        // Commodore   (col7,row5)
}

// C64 keyboard-as-joystick. With the JOYSTICK option ON, the arrow keys + Space (+ either SHIFT on
// the PicoCalc) drive the C64 joystick, routed to port 1/2 per joyPort. This runs alongside the
// keyboard matrix rather than replacing it, so the same arrows still move the BASIC cursor.
// CIA bits are active-low: bit0=up, 1=down, 2=left, 3=right, 4=fire (0xff = idle).
static void c64ApplyJoystick(const uint8_t *keys, bool shift)
{
  uint8_t m = 0xff;
  if (kbContains(keys, HID_KEY_ARROW_UP))    m &= ~0x01;
  if (kbContains(keys, HID_KEY_ARROW_DOWN))  m &= ~0x02;
  if (kbContains(keys, HID_KEY_ARROW_LEFT))  m &= ~0x04;
  if (kbContains(keys, HID_KEY_ARROW_RIGHT)) m &= ~0x08;
  if (kbContains(keys, HID_KEY_SPACE))       m &= ~0x10;   // fire
  if (C64_SHIFT_IS_FIRE && shift)            m &= ~0x10;   // PicoCalc: either SHIFT is fire too
  c64SetJoystick(m);
}

// ============================ NES controller 1 ===========================================
// Active-high bits: bit0=A bit1=B bit2=Select bit3=Start bit4=Up bit5=Down bit6=Left bit7=Right.
static uint8_t nesKbBits = 0;
static uint8_t nesBit(uint8_t kc)
{
  switch (kc) {
    case HID_KEY_ARROW_UP:    return 0x10;
    case HID_KEY_ARROW_DOWN:  return 0x20;
    case HID_KEY_ARROW_LEFT:  return 0x40;
    case HID_KEY_ARROW_RIGHT: return 0x80;
    case HID_KEY_X:
    case HID_KEY_F4:          return 0x01;   // A
    case HID_KEY_Z:
    case HID_KEY_F5:          return 0x02;   // B
    case HID_KEY_TAB:
    case HID_KEY_F3:          return 0x04;   // Select
    case HID_KEY_ENTER:
    case HID_KEY_F2:          return 0x08;   // Start
  }
  return 0;
}

// ============================ Atari 2600 ================================================
// dirBits: bit0=Up bit1=Down bit2=Left bit3=Right, plus Fire / Select / Reset switches.
static uint8_t atariDir = 0;
static bool    atariFire = false, atariSelect = false, atariReset = false;
static void atariApply() { atariSetInput(atariDir, atariFire, atariSelect, atariReset); }
static bool atariKey(uint8_t kc, bool down)   // returns true if it was an Atari key
{
  switch (kc) {
    case HID_KEY_ARROW_UP:    if (down) atariDir |= 0x01; else atariDir &= ~0x01; return true;
    case HID_KEY_ARROW_DOWN:  if (down) atariDir |= 0x02; else atariDir &= ~0x02; return true;
    case HID_KEY_ARROW_LEFT:  if (down) atariDir |= 0x04; else atariDir &= ~0x04; return true;
    case HID_KEY_ARROW_RIGHT: if (down) atariDir |= 0x08; else atariDir &= ~0x08; return true;
    case HID_KEY_SPACE:
    case HID_KEY_X:           atariFire   = down; return true;
    case HID_KEY_TAB:         atariSelect = down; return true;
    case HID_KEY_ENTER:       atariReset  = down; return true;
#if defined(BOARD_PICOCALC)
    // PicoCalc function row (input_picocalc.cpp sends its F3 as HID F12): F5 = fire,
    // F3 = Reset switch, F4 = Select switch.
    case HID_KEY_F5:          atariFire   = down; return true;
    case HID_KEY_F12:         atariReset  = down; return true;
    case HID_KEY_F4:          atariSelect = down; return true;
#endif
  }
  return false;
}

// ============================ MSX keyboard matrix ========================================
// Map a HID keycode to an MSX 8x11 matrix position in this codebase's (col,row) convention; fed to
// msxKeyMatrix(row, col, down). col < 0 = unmapped. See the matrix table in src/msx/msx_ppi.cpp.
struct MsxPos { int8_t col, row; };
static MsxPos msxMap(uint8_t kc)
{
  switch (kc) {
    case HID_KEY_A: return {6,2}; case HID_KEY_B: return {7,2}; case HID_KEY_C: return {0,3};
    case HID_KEY_D: return {1,3}; case HID_KEY_E: return {2,3}; case HID_KEY_F: return {3,3};
    case HID_KEY_G: return {4,3}; case HID_KEY_H: return {5,3}; case HID_KEY_I: return {6,3};
    case HID_KEY_J: return {7,3}; case HID_KEY_K: return {0,4}; case HID_KEY_L: return {1,4};
    case HID_KEY_M: return {2,4}; case HID_KEY_N: return {3,4}; case HID_KEY_O: return {4,4};
    case HID_KEY_P: return {5,4}; case HID_KEY_Q: return {6,4}; case HID_KEY_R: return {7,4};
    case HID_KEY_S: return {0,5}; case HID_KEY_T: return {1,5}; case HID_KEY_U: return {2,5};
    case HID_KEY_V: return {3,5}; case HID_KEY_W: return {4,5}; case HID_KEY_X: return {5,5};
    case HID_KEY_Y: return {6,5}; case HID_KEY_Z: return {7,5};
    case HID_KEY_0: return {0,0}; case HID_KEY_1: return {1,0}; case HID_KEY_2: return {2,0};
    case HID_KEY_3: return {3,0}; case HID_KEY_4: return {4,0}; case HID_KEY_5: return {5,0};
    case HID_KEY_6: return {6,0}; case HID_KEY_7: return {7,0}; case HID_KEY_8: return {0,1};
    case HID_KEY_9: return {1,1};
    case HID_KEY_MINUS:        return {2,1};   // -
    case HID_KEY_EQUAL:        return {3,1};   // =
    case HID_KEY_BACKSLASH:    return {4,1};   // backslash
    case HID_KEY_BRACKET_LEFT: return {5,1};   // [
    case HID_KEY_BRACKET_RIGHT:return {6,1};   // ]
    case HID_KEY_SEMICOLON:    return {7,1};   // ;
    case HID_KEY_APOSTROPHE:   return {0,2};   // '
    case HID_KEY_GRAVE:        return {1,2};   // `
    case HID_KEY_COMMA:        return {2,2};   // ,
    case HID_KEY_PERIOD:       return {3,2};   // .
    case HID_KEY_SLASH:        return {4,2};   // /
    case HID_KEY_ENTER:
    case HID_KEY_KEYPAD_ENTER: return {7,7};   // RETURN
    case HID_KEY_BACKSPACE:    return {5,7};   // BS
    case HID_KEY_DELETE:       return {3,8};   // DEL
    case HID_KEY_SPACE:        return {0,8};
    case HID_KEY_ESCAPE:       return {2,7};
    case HID_KEY_TAB:          return {3,7};
    case HID_KEY_ARROW_LEFT:   return {4,8};
    case HID_KEY_ARROW_UP:     return {5,8};
    case HID_KEY_ARROW_DOWN:   return {6,8};
    case HID_KEY_ARROW_RIGHT:  return {7,8};
    case HID_KEY_F1: return {5,6}; case HID_KEY_F2: return {6,6}; case HID_KEY_F3: return {7,6};
    case HID_KEY_F4: return {0,7}; case HID_KEY_F5: return {1,7};
  }
  return {-1, -1};
}
static inline void msxMatrix(int r, int c, bool d) { msxKeyMatrix(r, c, d); }
static inline void msxJoy(uint8_t m) { msxSetInput(m); }
static void msxKeyDown(uint8_t kc) { MsxPos p = msxMap(kc); if (p.col >= 0) msxMatrix(p.row, p.col, true); }
static void msxKeyUp(uint8_t kc)   { MsxPos p = msxMap(kc); if (p.col >= 0) msxMatrix(p.row, p.col, false); }
static void msxApplyModifiers(bool shift, bool ctrl, bool alt)
{
  msxMatrix(6, 0, shift);   // SHIFT (row6,col0)
  msxMatrix(6, 1, ctrl);    // CTRL  (row6,col1)
  msxMatrix(6, 2, alt);     // GRAPH (row6,col2) via Alt
}
static bool msxIsJoyKey(uint8_t kc)
{
  return kc == HID_KEY_ARROW_UP || kc == HID_KEY_ARROW_DOWN ||
         kc == HID_KEY_ARROW_LEFT || kc == HID_KEY_ARROW_RIGHT || kc == HID_KEY_SPACE;
}
static void msxApplyJoystick(const uint8_t *keys)   // active-low: b0 up b1 down b2 left b3 right b4 trgA
{
  uint8_t m = 0xFF;
  if (kbContains(keys, HID_KEY_ARROW_UP))    m &= ~0x01;
  if (kbContains(keys, HID_KEY_ARROW_DOWN))  m &= ~0x02;
  if (kbContains(keys, HID_KEY_ARROW_LEFT))  m &= ~0x04;
  if (kbContains(keys, HID_KEY_ARROW_RIGHT)) m &= ~0x08;
  if (kbContains(keys, HID_KEY_SPACE))       m &= ~0x10;   // trigger A
  msxJoy(m);
}
// SMS has no keyboard: map the USB keyboard straight to controller 1 (active-low, same bit order).
static void smsApplyJoystick(const uint8_t *keys)   // b0 up b1 down b2 left b3 right b4 btn1 b5 btn2
{
  uint8_t m = 0xFF;
  if (kbContains(keys, HID_KEY_ARROW_UP))    m &= ~0x01;
  if (kbContains(keys, HID_KEY_ARROW_DOWN))  m &= ~0x02;
  if (kbContains(keys, HID_KEY_ARROW_LEFT))  m &= ~0x04;
  if (kbContains(keys, HID_KEY_ARROW_RIGHT)) m &= ~0x08;
  if (kbContains(keys, HID_KEY_SPACE) || kbContains(keys, HID_KEY_Z)) m &= ~0x10;   // button 1
  if (kbContains(keys, HID_KEY_X))           m &= ~0x20;                            // button 2
#if defined(BOARD_PICOCALC)
  // PicoCalc: the F4/F5 keys sit right above the arrows, so they make the natural fire buttons
  // (input_picocalc.cpp passes them through as bare HID F4/F5).
  if (kbContains(keys, HID_KEY_F4))          m &= ~0x10;                            // button 1 / start
  if (kbContains(keys, HID_KEY_F5))          m &= ~0x20;                            // button 2
#endif
  smsSetInput(m);
}
// ColecoVision: arrows + fire buttons -> controller 1, and the 12-key keypad from the digit row.
// '*' and '#' are Shift-8 / Shift-3 (what they are printed on, and how the PicoCalc sends them) or
// '-' / '=' for one-key access. The keypad holds a single key, so the first one found wins.
static void colecoApplyJoystick(uint8_t modifier, const uint8_t *keys)
{
  const bool shift = modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
  uint8_t m = 0xFF;
  if (kbContains(keys, HID_KEY_ARROW_UP))    m &= ~0x01;
  if (kbContains(keys, HID_KEY_ARROW_DOWN))  m &= ~0x02;
  if (kbContains(keys, HID_KEY_ARROW_LEFT))  m &= ~0x04;
  if (kbContains(keys, HID_KEY_ARROW_RIGHT)) m &= ~0x08;
  if (kbContains(keys, HID_KEY_SPACE) || kbContains(keys, HID_KEY_Z) || kbContains(keys, HID_KEY_F4)) m &= ~0x10;  // left fire
  if (kbContains(keys, HID_KEY_X) || kbContains(keys, HID_KEY_F5))                                   m &= ~0x20;  // right fire
  colecoSetInput(m);

  int key = -1;
  for (int i = 0; i < 6 && key < 0; i++) {
    const uint8_t kc = keys[i];
    if (shift && kc == HID_KEY_8)                          key = 10;   // '*'
    else if (shift && kc == HID_KEY_3)                     key = 11;   // '#'
    else if (kc == HID_KEY_MINUS || kc == HID_KEY_KEYPAD_MULTIPLY) key = 10;
    else if (kc == HID_KEY_EQUAL)                          key = 11;
    else if (kc == HID_KEY_0 || kc == HID_KEY_KEYPAD_0)    key = 0;
    else if (kc >= HID_KEY_1 && kc <= HID_KEY_9)           key = kc - HID_KEY_1 + 1;
    else if (kc >= HID_KEY_KEYPAD_1 && kc <= HID_KEY_KEYPAD_9) key = kc - HID_KEY_KEYPAD_1 + 1;
  }
  colecoSetKeypad(key);
}

// ============================ ZX Spectrum keyboard matrix =================================
// The Spectrum's 40 keys sit in 8 half-rows of 5 (row = address line A8..A15, bit 0 = the key nearest
// the edge), and everything else is a chord with CAPS SHIFT or SYMBOL SHIFT. So the matrix is rebuilt
// from the whole report every time rather than tracked per key: a PC key that stands for a chord
// (Backspace = CAPS+0, ',' = SYMBOL+N) can then overlap anything else without a key getting stuck.
//   Shift = CAPS SHIFT, Ctrl/Alt = SYMBOL SHIFT, Backspace = DELETE, arrows = cursor keys,
//   Esc = BREAK, Tab = EXTENDED MODE, and PC punctuation types the Spectrum symbol printed on it.
// With JOY on, arrows + Space drive the Kempston joystick instead (as on the MSX).
struct ZxPos { int8_t row, bit; };
static const char ZX_ROWS[8][6] = { "cZXCV", "ASDFG", "QWERT", "12345", "09876", "POIUY", "eLKJH", "syMNB" };  // c/e/s/y = CAPS/ENTER/SPACE/SYMBOL
static ZxPos zxFindChar(char ch)
{
  for (int r = 0; r < 8; r++) for (int b = 0; b < 5; b++) if (ZX_ROWS[r][b] == ch) return {(int8_t)r, (int8_t)b};
  return {-1, -1};
}
static const ZxPos ZX_CAPS = {0, 0}, ZX_SYM = {7, 1}, ZX_SPACE = {7, 0}, ZX_ENTER = {6, 0};

static void zxApplyKeyboard(uint8_t modifier, const uint8_t *keys)
{
  bool shift = modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
  bool sym   = modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL |
                           KEYBOARD_MODIFIER_LEFTALT  | KEYBOARD_MODIFIER_RIGHTALT);
  bool caps  = shift;
  uint8_t rows[8] = {0, 0, 0, 0, 0, 0, 0, 0};          // 1 = held
  auto hold = [&](ZxPos p) { if (p.row >= 0) rows[p.row] |= (uint8_t)(1 << p.bit); };
  uint8_t kemp = 0;
  for (int i = 0; i < 6; i++) {
    const uint8_t kc = keys[i];
    if (!kc) continue;
    if (joystick) {
      switch (kc) {
        case HID_KEY_ARROW_RIGHT: kemp |= 0x01; continue;
        case HID_KEY_ARROW_LEFT:  kemp |= 0x02; continue;
        case HID_KEY_ARROW_DOWN:  kemp |= 0x04; continue;
        case HID_KEY_ARROW_UP:    kemp |= 0x08; continue;
        case HID_KEY_SPACE:       kemp |= 0x10; continue;
      }
    }
    if (kc >= HID_KEY_A && kc <= HID_KEY_Z) { hold(zxFindChar((char)('A' + kc - HID_KEY_A))); continue; }
    if (kc >= HID_KEY_1 && kc <= HID_KEY_9) { hold(zxFindChar((char)('1' + kc - HID_KEY_1))); continue; }
    // A punctuation key: its SYMBOL SHIFT chord, the shifted PC symbol when Shift is down (which
    // then must not also press CAPS SHIFT).
    char plain = 0, shifted = 0;
    switch (kc) {
      case HID_KEY_0:            hold(zxFindChar('0')); continue;
      case HID_KEY_ENTER:
      case HID_KEY_KEYPAD_ENTER: hold(ZX_ENTER); continue;
      case HID_KEY_SPACE:        hold(ZX_SPACE); continue;
      case HID_KEY_BACKSPACE:    hold(ZX_CAPS); hold(zxFindChar('0')); continue;
      case HID_KEY_ESCAPE:       hold(ZX_CAPS); hold(ZX_SPACE); continue;   // BREAK
      case HID_KEY_TAB:          hold(ZX_CAPS); hold(ZX_SYM); continue;     // EXTENDED MODE
      case HID_KEY_ARROW_LEFT:   hold(ZX_CAPS); hold(zxFindChar('5')); continue;
      case HID_KEY_ARROW_DOWN:   hold(ZX_CAPS); hold(zxFindChar('6')); continue;
      case HID_KEY_ARROW_UP:     hold(ZX_CAPS); hold(zxFindChar('7')); continue;
      case HID_KEY_ARROW_RIGHT:  hold(ZX_CAPS); hold(zxFindChar('8')); continue;
      case HID_KEY_COMMA:        plain = 'N'; shifted = 'R'; break;   // ,  <
      case HID_KEY_PERIOD:       plain = 'M'; shifted = 'T'; break;   // .  >
      case HID_KEY_SEMICOLON:    plain = 'O'; shifted = 'Z'; break;   // ;  :
      case HID_KEY_APOSTROPHE:   plain = '7'; shifted = 'P'; break;   // '  "
      case HID_KEY_MINUS:        plain = 'J'; shifted = '0'; break;   // -  _
      case HID_KEY_EQUAL:        plain = 'L'; shifted = 'K'; break;   // =  +
      case HID_KEY_SLASH:        plain = 'V'; shifted = 'C'; break;   // /  ?
      default: continue;
    }
    hold(zxFindChar(shift ? shifted : plain));
    hold(ZX_SYM);
    caps = false;
  }
  if (caps) hold(ZX_CAPS);
  if (sym)  hold(ZX_SYM);
  for (int r = 0; r < 8; r++)
    for (int b = 0; b < 5; b++) zxKey(r, b, (rows[r] >> b) & 1);
  zxSetKempston(kemp);
}

// ============================ public entry points ========================================
// Called from the USB host task (usbgamepad.cpp onKeyboard) with the current and previous
// boot-report keycode arrays. We diff them to emit key-down / key-up events.
void usbKeyboardReport(uint8_t modifier, const uint8_t *keys, const uint8_t *last)
{
  bool shift = modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
  bool ctrl  = modifier & (KEYBOARD_MODIFIER_LEFTCTRL  | KEYBOARD_MODIFIER_RIGHTCTRL);
  bool alt   = modifier & (KEYBOARD_MODIFIER_LEFTALT   | KEYBOARD_MODIFIER_RIGHTALT |
                           KEYBOARD_MODIFIER_LEFTGUI   | KEYBOARD_MODIFIER_RIGHTGUI);

  // SD Manager mode has no core to type into and no settings menu to open (F10 would open one the
  // render loop never draws). Its one key, Ctrl-F6 back to the system menu, is taken upstream.
  if (currentPlatform == PLATFORM_SDMANAGER) return;

  // --- new key-down events ---
  for (int i = 0; i < 6; i++) {
    uint8_t kc = keys[i];
    if (!kc || kbContains(last, kc)) continue;

    // Settings menu: F10 toggles; while open, the keyboard drives the menu (all platforms).
    // All four arrows browse; Enter flips a toggle, or opens VOLUME / the file list so the
    // arrows then change the value inside it; Escape backs out of whatever Enter opened, and
    // Escape with nothing open closes the window. The joystick keeps its own simpler scheme
    // (optionsUiNav/Adjust/Activate) -- see the note in optionsui.cpp.
    if (OptionsWindow) {
      switch (kc) {
        case HID_KEY_ARROW_LEFT:   optionsUiKeyArrow(-1,  0); break;
        case HID_KEY_ARROW_RIGHT:  optionsUiKeyArrow(+1,  0); break;
        case HID_KEY_ARROW_UP:     optionsUiKeyArrow( 0, -1); break;
        case HID_KEY_ARROW_DOWN:   optionsUiKeyArrow( 0, +1); break;
        case HID_KEY_ENTER:
        case HID_KEY_KEYPAD_ENTER: optionsUiKeyEnter(ctrl); break;
        case HID_KEY_ESCAPE:       if (!optionsUiKeyEscape()) showHideOptionsWindow(); break;
        case HID_KEY_F10:          showHideOptionsWindow(); break;
      }
      continue;   // menu swallows every key
    }
    if (kc == HID_KEY_F10) { showHideOptionsWindow(); continue; }
    if (kc == HID_KEY_F11 &&
        currentPlatform == PLATFORM_APPLE2) {
      cpuReset(); continue;
    }
    if (currentPlatform == PLATFORM_SMS) {
      if (kc == HID_KEY_F11) { smsPauseButton(); continue; }   // SMS PAUSE -> NMI
      if (kc == HID_KEY_F12) { smsHardReset();   continue; }   // soft power-cycle
    }
    if (currentPlatform == PLATFORM_PCXT && kc == HID_KEY_F12) { pcxtHardReset(); continue; }  // soft reboot
    if (currentPlatform == PLATFORM_COLECO && kc == HID_KEY_F12) { colecoHardReset(); continue; } // power-cycle
    if (currentPlatform == PLATFORM_ZX && kc == HID_KEY_F12) { zxHardReset(); continue; }         // power-cycle

    switch (currentPlatform) {
      case PLATFORM_APPLE2: if (!appleIsButtonKey(kc)) appleKeyDown(kc, shift, ctrl);
                            break;   // arrows/Space = paddles AND keys; F4/F5 = buttons only
      case PLATFORM_C64:   c64KeyDown(kc); break;   // every key types; the stick reads the arrows too
      case PLATFORM_NES:   nesKbBits |= nesBit(kc); nesSetController(nesKbBits); break;
      case PLATFORM_ATARI: if (atariKey(kc, true)) atariApply(); break;
      case PLATFORM_MSX:   if (!(joystick && msxIsJoyKey(kc))) msxKeyDown(kc); break;  // arrows+Space = joystick when JOY on
      case PLATFORM_PCXT:  pcxtKeyDown(kc, shift, ctrl, alt); break;                    // USB key -> XT make scancode
    }
  }

  // While the menu is open the consoles get no key events (and no modifier updates).
  if (OptionsWindow) return;

  // --- new key-up events ---
  for (int i = 0; i < 6; i++) {
    uint8_t kc = last[i];
    if (!kc || kbContains(keys, kc)) continue;
    switch (currentPlatform) {
      case PLATFORM_C64:   c64KeyUp(kc); break;
      case PLATFORM_NES:   nesKbBits &= ~nesBit(kc); nesSetController(nesKbBits); break;
      case PLATFORM_ATARI: if (atariKey(kc, false)) atariApply(); break;
      case PLATFORM_MSX:   if (!(joystick && msxIsJoyKey(kc))) msxKeyUp(kc); break;
      case PLATFORM_PCXT:  pcxtKeyUp(kc); break;   // USB key -> XT break scancode
      default: break;   // Apple keystrokes are edge-triggered (keymem); nothing to release
    }
  }

  // --- continuous modifier state ---
  if (currentPlatform == PLATFORM_C64) {
    c64ApplyModifiers(shift, ctrl, alt, keys);
    if (joystick) c64ApplyJoystick(keys, shift);   // arrows + Space (+ SHIFT on PicoCalc) -> joystick
    else          c64SetJoystick(0xff);     // typing mode: keep the joystick released
  } else if (currentPlatform == PLATFORM_MSX) {
    msxApplyModifiers(shift, ctrl, alt);
    if (joystick) msxApplyJoystick(keys);   // arrows + Space -> joystick
    else          msxJoy(0xFF);             // typing mode: joystick released
  } else if (currentPlatform == PLATFORM_SMS) {
    smsApplyJoystick(keys);                 // joystick-only: arrows + Z/X/Space -> controller 1
  } else if (currentPlatform == PLATFORM_COLECO) {
    colecoApplyJoystick(modifier, keys);    // arrows + fire keys + digit-row keypad -> controller 1
  } else if (currentPlatform == PLATFORM_ZX) {
    zxApplyKeyboard(modifier, keys);        // the whole matrix (+ Kempston when JOY on), rebuilt per report
  } else if (currentPlatform == PLATFORM_PCXT) {
    // PC needs make/break for shift/ctrl/alt (they arrive as the modifier byte, not in keys[]).
    static uint8_t prevMod = 0;
    static const struct { uint8_t bit; uint8_t usage; } mm[] = {
      {0x01,0xE0},{0x02,0xE1},{0x04,0xE2},{0x10,0xE4},{0x20,0xE5} };  // LCtrl LShift LAlt RCtrl RShift
    for (auto& e : mm) {
      bool now = (modifier & e.bit) != 0, was = (prevMod & e.bit) != 0;
      if (now && !was) pcxtKeyDown(e.usage, false, false, false);
      else if (!now && was) pcxtKeyUp(e.usage);
    }
    prevMod = modifier;
  } else if (currentPlatform == PLATFORM_APPLE2) {
    // open-apple / solid-apple: Alt and right-Alt on a PC keyboard, F4 and F5 on the PicoCalc.
    Pb0 = (modifier & KEYBOARD_MODIFIER_LEFTALT)  != 0 || kbContains(keys, HID_KEY_F4);
    Pb1 = (modifier & KEYBOARD_MODIFIER_RIGHTALT) != 0 || kbContains(keys, HID_KEY_F5);
    if (joystick) appleApplyJoystick(keys);               // arrows -> paddles, Space -> button 0
    else { timerpdl0 = JOY_MID; timerpdl1 = JOY_MID; }    // typing mode: paddles centred
  }
}

// Release everything a keyboard was holding (called when the USB device disconnects).
void usbKeyboardReset()
{
  if (currentPlatform == PLATFORM_C64) {
    c64KeyMatrix(7, 1, false); c64KeyMatrix(2, 7, false); c64KeyMatrix(5, 7, false);
    c64SetJoystick(0xff);   // release the keyboard joystick
  }
  nesKbBits = 0;
  if (currentPlatform == PLATFORM_NES) nesSetController(0);
  atariDir = 0; atariFire = atariSelect = atariReset = false;
  if (currentPlatform == PLATFORM_ATARI) atariApply();
  if (currentPlatform == PLATFORM_MSX) {
    msxMatrix(6, 0, false); msxMatrix(6, 1, false); msxMatrix(6, 2, false);
    msxJoy(0xFF);
  }
  if (currentPlatform == PLATFORM_SMS) smsSetInput(0xFF);
  if (currentPlatform == PLATFORM_COLECO) { colecoSetInput(0xFF); colecoSetKeypad(-1); }
  if (currentPlatform == PLATFORM_ZX) { zxKeysReleaseAll(); zxSetKempston(0); }
}

#endif // BOARD_INPUT_USB
