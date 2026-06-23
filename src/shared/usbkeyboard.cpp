// usbkeyboard.cpp - USB-HID host KEYBOARD input for the JC4827W543 (ESP32-S3 native USB).
//
// Companion to usbgamepad.cpp: the same EspUsbHost instance that decodes the SNES pad also
// receives boot-protocol keyboard reports. Its onKeyboard() override forwards each report here,
// and we translate the standard 8-byte HID boot report (modifier byte + up to 6 keycodes) into
// whatever the currently-selected console expects:
//
//   * Apple II / IIGS : set the global keymem to an Apple keycode (high "key ready" bit set),
//                       exactly like the PS/2 and on-screen-keyboard paths. Left/Right ALT drive
//                       the open-/solid-apple paddle buttons (Pb0/Pb1).
//   * C64             : press/release the CIA1 keyboard matrix via c64KeyMatrix(row,col,down),
//                       with PC Shift/Ctrl/Alt mapped to the C64 SHIFT/CTRL/Commodore lines.
//   * NES             : map the d-pad + Z/X/Enter/Tab onto controller 1 (nesSetController()).
//   * Atari 2600      : map the d-pad + Space/Enter/Tab onto the stick + Fire/Reset/Select.
//
// Settings menu (every platform): F12 opens/closes it; while it is open the arrow keys navigate
// (Left/Right move the selection, Up/Down change the value) and Enter activates. F11 is a CPU
// reset on the Apple II / IIGS.
//
// Build target only: BOARD_INPUT_USB (the JC4827W543). The CYD uses its PS/2 header instead.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "EspUsbHost.h"   // HID_KEY_* / KEYBOARD_MODIFIER_* constants + HID_KEYCODE_TO_ASCII table

// --- small helpers -------------------------------------------------------------------------
static bool kbContains(const uint8_t *arr, uint8_t kc)
{
  for (int i = 0; i < 6; i++) if (arr[i] == kc) return true;
  return false;
}

// US-layout HID keycode -> ASCII, reusing the library's conversion table.
static char kbToAscii(uint8_t kc, bool shift)
{
  static const uint8_t conv[128][2] = { HID_KEYCODE_TO_ASCII };
  if (kc >= 128) return 0;
  return (char)conv[kc][shift ? 1 : 0];
}

// ============================ Apple II / IIGS =============================================
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
// Refresh the modifier matrix lines from the live report each call. Cursor LEFT/UP are SHIFT+CRSR
// on the C64, so a held LEFT or UP also asserts SHIFT.
static void c64ApplyModifiers(bool shift, bool ctrl, bool alt, const uint8_t *keys)
{
  bool wantShift = shift || kbContains(keys, HID_KEY_ARROW_LEFT) || kbContains(keys, HID_KEY_ARROW_UP);
  c64KeyMatrix(7, 1, wantShift);  // left SHIFT  (col1,row7)
  c64KeyMatrix(2, 7, ctrl);       // CTRL        (col7,row2)
  c64KeyMatrix(5, 7, alt);        // Commodore   (col7,row5)
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
    case HID_KEY_X:           return 0x01;   // A
    case HID_KEY_Z:           return 0x02;   // B
    case HID_KEY_TAB:         return 0x04;   // Select
    case HID_KEY_ENTER:       return 0x08;   // Start
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
  }
  return false;
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

  // --- new key-down events ---
  for (int i = 0; i < 6; i++) {
    uint8_t kc = keys[i];
    if (!kc || kbContains(last, kc)) continue;

    // Settings menu: F10 toggles; while open, the keyboard drives the menu (all platforms).
    if (OptionsWindow) {
      switch (kc) {
        case HID_KEY_ARROW_LEFT:   optionsUiNav(-1);    break;
        case HID_KEY_ARROW_RIGHT:  optionsUiNav(+1);    break;
        case HID_KEY_ARROW_UP:     optionsUiAdjust(-1); break;
        case HID_KEY_ARROW_DOWN:   optionsUiAdjust(+1); break;
        case HID_KEY_ENTER:
        case HID_KEY_KEYPAD_ENTER: optionsUiActivate(); break;
        case HID_KEY_ESCAPE:
        case HID_KEY_F10:          showHideOptionsWindow(); break;
      }
      continue;   // menu swallows every key
    }
    if (kc == HID_KEY_F10) { showHideOptionsWindow(); continue; }
    if (kc == HID_KEY_F11 &&
        (currentPlatform == PLATFORM_APPLE2 || currentPlatform == PLATFORM_IIGS)) {
      cpuReset(); continue;
    }

    switch (currentPlatform) {
      case PLATFORM_APPLE2:
      case PLATFORM_IIGS:  appleKeyDown(kc, shift, ctrl); break;
      case PLATFORM_C64:   c64KeyDown(kc);                break;
      case PLATFORM_NES:   nesKbBits |= nesBit(kc); nesSetController(nesKbBits); break;
      case PLATFORM_ATARI: if (atariKey(kc, true)) atariApply(); break;
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
      default: break;   // Apple/IIGS keystrokes are edge-triggered (keymem); nothing to release
    }
  }

  // --- continuous modifier state ---
  if (currentPlatform == PLATFORM_C64) {
    c64ApplyModifiers(shift, ctrl, alt, keys);
  } else if (currentPlatform == PLATFORM_APPLE2 || currentPlatform == PLATFORM_IIGS) {
    Pb0 = (modifier & KEYBOARD_MODIFIER_LEFTALT)  != 0;   // open-apple  (paddle button 0)
    Pb1 = (modifier & KEYBOARD_MODIFIER_RIGHTALT) != 0;   // solid-apple (paddle button 1)
  }
}

// Release everything a keyboard was holding (called when the USB device disconnects).
void usbKeyboardReset()
{
  if (currentPlatform == PLATFORM_C64) {
    c64KeyMatrix(7, 1, false); c64KeyMatrix(2, 7, false); c64KeyMatrix(5, 7, false);
  }
  nesKbBits = 0;
  if (currentPlatform == PLATFORM_NES) nesSetController(0);
  atariDir = 0; atariFire = atariSelect = atariReset = false;
  if (currentPlatform == PLATFORM_ATARI) atariApply();
}

#endif // BOARD_INPUT_USB
