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
//   * NES             : each key presses the controller-1 button neskeymap.cpp says (default:
//                       arrows, X/Z = A/B, Enter/Tab = Start/Select, F2-F5), nesSetController().
//   * SMS             : each key presses the control pad 1 button smskeymap.cpp says (default:
//                       arrows, Space/Z = 1, X = 2, PicoCalc F4/F5), smsSetInput().
//   * ColecoVision    : each key presses the controller 1 stick / fire / keypad key colecokeymap.cpp
//                       says (default: arrows, Space/Z/F4 + X/F5 fire, digits, Shift-8/3 = * / #).
//   * Atari 2600      : each key presses the stick direction / Fire / Select / Reset atarikeymap.cpp
//                       says (default: arrows, Space/X = Fire, Tab/Enter = Select/Reset).
//
// Settings menu (every platform): F12 opens/closes it; while it is open the arrow keys navigate
// (Left/Right move the selection, Up/Down change the value) and Enter activates. F11 is a CPU
// reset on the Apple II.
//
// Build target only: BOARD_INPUT_USB (the JC4827W543). The CYD uses its PS/2 header instead.

#include "../../emu.h"

#if BOARD_INPUT_USB

#if defined(BOARD_PICOCALC_P4)
#include "../picocalc/pico_shim/EspUsbHost.h"   // no USB host: just the HID_KEY_* tables (input_picocalc.cpp feeds us)
#elif BOARD_PANEL_DSI
#include "p4/usb/EspUsbHost.h"   // P4: vendored EspUsbHost fork, patched for IDF 5.x (in-repo)
#else
#include "EspUsbHost.h"   // S3: external fork — HID_KEY_* / KEYBOARD_MODIFIER_* + HID_KEYCODE_TO_ASCII
#endif
#include "c64keymap.h"
#include "c64joymap.h"
#include "neskeymap.h"
#include "atarikeymap.h"
#include "smskeymap.h"
#include "msxkeymap.h"
#include "msxjoymap.h"
#include "colecokeymap.h"
#include "apple2keymap.h"
#include "apple2joymap.h"
#include "zxkeymap.h"
#include "zxjoymap.h"

// --- small helpers -------------------------------------------------------------------------
static bool kbContains(const uint8_t *arr, uint8_t kc)
{
  for (int i = 0; i < 6; i++) if (arr[i] == kc) return true;
  return false;
}

// ============================ Apple II =====================================================
// Which Apple key a host key presses comes from apple2keymap.cpp, what it does to the joystick from
// apple2joymap.cpp: built-in defaults, overridden per key from the SD card or CONFIG > KEYBOARD /
// JOYSTICK. Only key-down types: the Apple keyboard is a strobe (keymem, bit 7 = key available),
// and what a key sends comes from the II+ encoder (apple2KeyCode) with the Apple's SHIFT / CTRL as
// they are at that moment: the host's SHIFT, keys held on the Apple's SHIFT / CTRL, and the rule's
// own +SHIFT / -SHIFT / +CTRL.
//
// The JOYSTICK option (appleArrows) picks what the keys on the stick do: JOY = only the paddles --
// their keycodes are game commands (Lode Runner's table is ESC ^R ^A ^S ^J ^K ^H ^U ^X ^Y, so Up
// (^K) flipped it to keyboard control) -- KBD = only the keys, BOTH = both at once. The buttons
// work in every mode (open-apple / solid-apple are keys on a real Apple too), except that in KBD
// a key that types (Space) is not also a button. Space is button 0 and still types in JOY: in
// Karateka's KEYBOARD control mode Space is a control key.
//
// The Apple has no digital stick: games read the PADDLE TIMERS at $C064/$C065, which
// processJoystick() ramps toward timerpdl0/timerpdl1 after a $C070 trigger. So a held direction is
// a full deflection and neutral is centre -- the same three values usbgamepad.cpp writes.
static uint8_t appleLast = 0;   // the last code typed: REPT sends it again

// Is a typing rule's key one of the Apple's modifiers (held, not typed)?
static bool appleIsMod(uint8_t code, uint8_t index)
{
  return a2kIsKey(code) && a2kIndex(code) == index;
}

// The Apple's SHIFT / CTRL as held through mapped keys (a host key on the Apple's SHIFT or CTRL).
static void appleHeldMods(bool ctrl, bool lalt, bool ralt, const uint8_t *keys, bool *aShift, bool *aCtrl)
{
  auto hold = [&](uint8_t code) {
    if (appleIsMod(code, A2K_CTRL)) *aCtrl = true;
    if (appleIsMod(code, A2K_LSHIFT) || appleIsMod(code, A2K_RSHIFT)) *aShift = true;
  };
  if (ctrl) hold(apple2KeymapLookup(C64K_HID_CTRL, false));   // CTRL by default
  if (lalt) hold(apple2KeymapLookup(C64K_HID_ALT, false));
  if (ralt) hold(apple2KeymapLookup(A2K_HID_RALT, false));
  for (int i = 0; i < 6; i++) if (keys[i]) hold(apple2KeymapLookup(keys[i], false));
}

static void appleKeyDown(uint8_t kc, bool shift, bool ctrl, bool lalt, bool ralt, const uint8_t *keys)
{
  // JOYSTICK = JOY: a key on the stick only steers (see above).
  if (appleArrows == APPLE_ARROWS_JOY) {
    const uint8_t j = apple2JoymapLookup(kc);
    if (a2jIsAction(j) && (j & A2J_DIRS)) return;
  }
  const uint8_t code = apple2KeymapLookup(kc, shift);
  if (code == A2K_RESET) { cpuReset(); return; }
  if (!a2kIsKey(code)) return;                         // no rule / NONE
  const uint8_t index = a2kIndex(code);
  if (index == A2K_REPT) { if (appleLast) keymem = (char)appleLast; return; }
  if (index == A2K_CAPS) { apple2CapsLock = !apple2CapsLock; return; }   // IIe CAPS LOCK: a toggle
  bool aShift = shift, aCtrl = false;
  appleHeldMods(ctrl, lalt, ralt, keys, &aShift, &aCtrl);
  switch (a2kMode(code)) {
    case A2K_SHIFT_ON:  aShift = true;  break;
    case A2K_SHIFT_OFF: aShift = false; break;
    case A2K_CTRL_ON:   aCtrl  = true;  break;
    default: break;
  }
  const uint8_t c = apple2KeyCode(index, aShift, aCtrl);
  if (!c) return;                                      // CTRL / SHIFT: held, not typed
  keymem = (char)(appleLast = c);
}

// The held keys -> paddles + buttons, through the joystick map. CTRL / ALT / RALT / SHIFT count as
// host keys too (C64K_HID_CTRL / C64K_HID_ALT / A2K_HID_RALT / C64K_HID_SHIFT).
static void appleApplyJoystick(bool shift, bool ctrl, bool lalt, bool ralt, const uint8_t *keys)
{
  const bool kbd = appleArrows == APPLE_ARROWS_KBD;
  uint8_t pull = 0;
  auto press = [&](uint8_t hid) {
    // A key on the IIe's OPEN-APPLE / SOLID-APPLE (keyboard map) is that button, in every mode.
    const uint8_t k = apple2KeymapLookup(hid, false);
    const bool apple = a2kIsKey(k) && (a2kIndex(k) == A2K_OAPPLE || a2kIndex(k) == A2K_SAPPLE);
    if (apple) pull |= a2kIndex(k) == A2K_OAPPLE ? A2J_BTN0 : A2J_BTN1;
    const uint8_t j = apple2JoymapLookup(hid);
    if (!a2jIsAction(j)) return;
    if (kbd) {                                         // KBD: no stick; a key that types is no button
      if (!apple && a2kIsKey(apple2KeymapLookup(hid, shift))) return;
      pull |= j & ~A2J_DIRS;
    } else {
      pull |= j;
    }
  };
  for (int i = 0; i < 6; i++) if (keys[i]) press(keys[i]);
  if (ctrl)  press(C64K_HID_CTRL);
  if (lalt)  press(C64K_HID_ALT);
  if (ralt)  press(A2K_HID_RALT);
  if (shift) press(C64K_HID_SHIFT);
  // Both ways at once is centre, as on a real stick.
  const uint8_t x = pull & (A2J_LEFT | A2J_RIGHT), y = pull & (A2J_UP | A2J_DOWN);
  timerpdl0 = x == A2J_LEFT ? JOY_MIN : x == A2J_RIGHT ? JOY_MAX : JOY_MID;   // paddle 0 = X
  timerpdl1 = y == A2J_UP   ? JOY_MIN : y == A2J_DOWN  ? JOY_MAX : JOY_MID;   // paddle 1 = Y
  Pb0 = (pull & A2J_BTN0) != 0;                      // open-apple
  Pb1 = (pull & A2J_BTN1) != 0;                      // solid-apple
}

// ============================ C64 keyboard matrix ========================================
// Which C64 key a host key presses comes from c64keymap.cpp: a built-in default, overridden per key
// by /c64/keymap.txt on the SD card. A rule names a matrix key and what it does to SHIFT (F2 =
// SHIFT+F1, CRSR LEFT = SHIFT+CRSR RIGHT, Shift-2 '@' = the C64's unshifted @ key).
//
// The rule is resolved once, on key-down, with the SHIFT state of that moment, and remembered with
// the key: the key-up then releases exactly what the key-down pressed even if SHIFT changed in
// between. The matrix lines are then rebuilt from the held keys + modifiers on every report and
// only the CHANGES are written, so two host keys on the same C64 key (Esc and Pause = RUN/STOP)
// don't release each other, and keys the on-screen keyboard holds are left alone.
struct C64Held { uint8_t kc, code; };
static C64Held  c64Held[6];
static uint64_t c64Down = 0;   // matrix keys this keyboard holds; bit = col*8 + row

static void c64KeyDown(uint8_t kc, bool shift)
{
  const uint8_t code = c64KeymapLookup(kc, shift);
  if (code == C64K_RESTORE) { c64Restore(); return; }   // a pulse on the NMI line, not a matrix key
  if (!c64kIsMatrix(code)) return;
  for (auto &h : c64Held) if (!h.kc) { h.kc = kc; h.code = code; return; }
}
static void c64KeyUp(uint8_t kc)
{
  for (auto &h : c64Held) if (h.kc == kc) h.kc = 0;
}

// Which keys drive the joystick comes from c64joymap.cpp: by default the arrows + Space, and on the
// PicoCalc either SHIFT too (the cursor cluster sits at the bottom right and the space bar is a long
// reach from it, while the two shifts flank that same bottom row). The keyboard reports both shifts
// as one modifier bit (input_picocalc.cpp), so SHIFT is one host key, C64K_HID_SHIFT.

// A key in the report that is not on the joystick: a keystroke, not steering.
static bool c64HasTypingKey(const uint8_t *keys)
{
  for (int i = 0; i < 6; i++)
    if (keys[i] && !c64jIsAction(c64JoymapLookup(keys[i]))) return true;
  return false;
}

// JOYSTICK mode takes NOTHING away from the C64 keyboard: the arrows, Space and (on the PicoCalc)
// SHIFT drive the stick AND still reach the matrix, the double duty the Apple II paddles already
// give their arrows. It used to hold the arrows exclusively, which left the C64 with no cursor keys
// at the BASIC prompt -- and since JOYSTICK is ON by default, the only way to get them back was to
// open the menu and turn it off. SHIFT was withheld for longer, which cost the PicoCalc every
// shifted character: no quotes for LOAD"$",8 and no upper case, with the same menu round trip as
// the only way out. It now does both jobs too (see c64ApplyModifiers).

// Rebuild this keyboard's matrix keys from the held keys + modifiers and write the changes.
static void c64ApplyModifiers(bool shift, bool ctrl, bool alt, const uint8_t *keys)
{
  bool wantShift = shift;
  // SHIFT on the joystick (the PicoCalc's default: fire) does that AND types. Both at once needs
  // one distinction, because the two jobs never happen in the same report: SHIFT pressed ALONE is a
  // shot, SHIFT pressed WITH a character key is a keystroke. So the C64's SHIFT line is asserted
  // only when the report also carries a typing key -- firing then shifts nothing, and the keys on
  // the joystick don't count, so holding fire while steering stays clean.
  //
  // This is exact rather than a guess, because of how the keyboard reports shift
  // (hidModifiers, input_picocalc.cpp): the bit is set from the CHARACTER (a vs A, which is also
  // the only thing that gets caps lock right) as well as from the physical key, so a shifted
  // character is never in flight without its own key in the same report.
  //
  // Dropped first, so the SHIFT a held key's rule asks for (CRSR LEFT/UP, F2...) still gets applied.
  if (shift && joystick && c64jIsAction(c64JoymapLookup(C64K_HID_SHIFT)) && !c64HasTypingKey(keys)) wantShift = false;

  uint64_t want = 0;
  auto hold = [&](uint8_t code) {
    if (!c64kIsMatrix(code)) return;
    want |= 1ULL << c64kIndex(code);
    if (c64kShift(code) == C64K_SHIFT_ON)  wantShift = true;    // the key's rule decides SHIFT
    if (c64kShift(code) == C64K_SHIFT_OFF) wantShift = false;
  };
  if (ctrl) hold(c64KeymapLookup(C64K_HID_CTRL, false));   // CTRL by default
  if (alt)  hold(c64KeymapLookup(C64K_HID_ALT, false));    // Commodore by default
  for (auto &h : c64Held) {
    if (!h.kc) continue;
    if (!kbContains(keys, h.kc)) { h.kc = 0; continue; }   // its key-up was lost (menu open): drop it
    hold(h.code);
  }
  if (wantShift) want |= 1ULL << C64K_LSHIFT_INDEX;

  const uint64_t changed = want ^ c64Down;
  for (int i = 0; i < 64; i++)
    if ((changed >> i) & 1) c64KeyMatrix(i & 7, i >> 3, (want >> i) & 1);   // (row, col)
  c64Down = want;
}

// Release every matrix key this keyboard holds.
static void c64ReleaseAll()
{
  for (auto &h : c64Held) h.kc = 0;
  for (int i = 0; i < 64; i++)
    if ((c64Down >> i) & 1) c64KeyMatrix(i & 7, i >> 3, false);
  c64Down = 0;
}
// C64 keyboard-as-joystick. With the JOYSTICK option ON, the keys on the joystick map (c64joymap.cpp:
// the arrows + Space by default, + either SHIFT on the PicoCalc) drive the C64 joystick, routed to
// port 1/2 per joyPort. This runs alongside the keyboard matrix rather than replacing it, so the
// same arrows still move the BASIC cursor.
// CIA bits are active-low: bit0=up, 1=down, 2=left, 3=right, 4=fire (0xff = idle); FIRE2 / FIRE3
// go to the paddle lines (SID POTX / POTY).
static void c64ApplyJoystick(const uint8_t *keys, bool shift, bool ctrl, bool alt)
{
  uint8_t pull = 0;
  auto press = [&](uint8_t hid) { const uint8_t c = c64JoymapLookup(hid); if (c64jIsAction(c)) pull |= c; };
  for (int i = 0; i < 6; i++) if (keys[i]) press(keys[i]);
  if (shift) press(C64K_HID_SHIFT);
  if (ctrl)  press(C64K_HID_CTRL);
  if (alt)   press(C64K_HID_ALT);
  c64SetJoystick((uint8_t)~(pull & C64J_LINES));
  c64SetJoyButtons((pull & C64J_FIRE2 ? 1 : 0) | (pull & C64J_FIRE3 ? 2 : 0));
}

// ============================ NES controller 1 ===========================================
// Active-high bits: bit0=A bit1=B bit2=Select bit3=Start bit4=Up bit5=Down bit6=Left bit7=Right.
// Rebuilt from the held keys + CTRL / ALT on every report (neskeymap.cpp: a built-in default,
// overridden per key by /nes/keymap.txt), so two keys on the same button don't release each other.
static uint8_t nesKbBits = 0;
static void nesApplyKeyboard(bool ctrl, bool alt, const uint8_t *keys)
{
  uint8_t bits = 0;
  auto press = [&](uint8_t hid) { const uint8_t c = nesKeymapLookup(hid); if (nesKIsButton(c)) bits |= 1 << c; };
  if (ctrl) press(C64K_HID_CTRL);
  if (alt)  press(C64K_HID_ALT);
  for (int i = 0; i < 6; i++) if (keys[i]) press(keys[i]);
  if (bits != nesKbBits) { nesKbBits = bits; nesSetController(bits); }
}

// ============================ Atari 2600 ================================================
// dirBits: bit0=Up bit1=Down bit2=Left bit3=Right, plus Fire / Select / Reset switches. Rebuilt
// from the held keys + CTRL / ALT on every report (atarikeymap.cpp: a built-in default, overridden
// per key by /atari/keymap.txt), so two keys on the same line don't release each other.
static uint8_t atariKbBits = 0;
static void atariApplyBits(uint8_t bits)
{
  atariSetInput(bits & ATARIK_DIRS, bits & ATARIK_FIRE, bits & ATARIK_SELECT, bits & ATARIK_RESET);
}
static void atariApplyKeyboard(bool ctrl, bool alt, const uint8_t *keys)
{
  uint8_t bits = 0;
  auto press = [&](uint8_t hid) { const uint8_t c = atariKeymapLookup(hid); if (atariKIsAction(c)) bits |= c; };
  if (ctrl) press(C64K_HID_CTRL);
  if (alt)  press(C64K_HID_ALT);
  for (int i = 0; i < 6; i++) if (keys[i]) press(keys[i]);
  if (bits != atariKbBits) { atariKbBits = bits; atariApplyBits(bits); }
}

// ============================ MSX keyboard matrix ========================================
// Each host key presses the MSX key msxkeymap.cpp names (a built-in default, overridden per key by
// /msx/keymap.txt), fed to msxKeyMatrix(row, bit, down); see the matrix table in src/msx/msx_ppi.cpp.
// Same scheme as the C64 above: the rule is resolved on key-down with that moment's SHIFT and
// remembered with the key, the matrix is rebuilt from the held keys + modifiers on every report and
// only the CHANGES are written, so two host keys on the same MSX key don't release each other.
struct MsxHeld { uint8_t kc, code; };
static MsxHeld  msxHeld[6];
static uint8_t  msxDown[MSXK_KEYS / 8];   // matrix keys this keyboard holds; bit = row*8 + bit
static inline void msxJoy(uint8_t m) { msxSetInput(m); }

static void msxKeyDown(uint8_t kc, bool shift)
{
  const uint8_t code = msxKeymapLookup(kc, shift);
  if (!msxkIsKey(code)) return;
  for (auto &h : msxHeld) if (!h.kc) { h.kc = kc; h.code = code; return; }
}
static void msxKeyUp(uint8_t kc)
{
  for (auto &h : msxHeld) if (h.kc == kc) h.kc = 0;
}

// A key in the report that is not on the joystick: a keystroke, not steering.
static bool msxHasTypingKey(const uint8_t *keys)
{
  for (int i = 0; i < 6; i++)
    if (keys[i] && !c64jIsAction(msxJoymapLookup(keys[i]))) return true;
  return false;
}

// Rebuild this keyboard's matrix keys from the held keys + modifiers and write the changes.
static void msxApplyModifiers(bool shift, bool ctrl, bool alt, const uint8_t *keys)
{
  bool wantShift = shift;
  // SHIFT on the joystick (the PicoCalc's default: TRIG A) fires when pressed alone and shifts
  // when pressed with a character key -- the C64's rule, see c64ApplyModifiers.
  if (shift && joystick && c64jIsAction(msxJoymapLookup(C64K_HID_SHIFT)) && !msxHasTypingKey(keys)) wantShift = false;

  uint8_t want[MSXK_KEYS / 8] = {0};
  auto hold = [&](uint8_t code) {
    if (!msxkIsKey(code)) return;
    const uint8_t i = msxkIndex(code);
    want[i >> 3] |= 1 << (i & 7);
    if (msxkShift(code) == MSXK_SHIFT_ON)  wantShift = true;    // the key's rule decides SHIFT
    if (msxkShift(code) == MSXK_SHIFT_OFF) wantShift = false;
  };
  if (ctrl) hold(msxKeymapLookup(C64K_HID_CTRL, false));   // CTRL by default
  if (alt)  hold(msxKeymapLookup(C64K_HID_ALT, false));    // GRAPH by default
  for (auto &h : msxHeld) {
    if (!h.kc) continue;
    if (!kbContains(keys, h.kc)) { h.kc = 0; continue; }   // its key-up was lost (menu open): drop it
    hold(h.code);
  }
  if (wantShift) want[MSXK_SHIFT_INDEX >> 3] |= 1 << (MSXK_SHIFT_INDEX & 7);

  for (int r = 0; r < MSXK_KEYS / 8; r++) {
    const uint8_t changed = want[r] ^ msxDown[r];
    for (int b = 0; b < 8; b++)
      if ((changed >> b) & 1) msxKeyMatrix(r, b, (want[r] >> b) & 1);
    msxDown[r] = want[r];
  }
}

// Release every matrix key this keyboard holds.
static void msxReleaseAll()
{
  for (auto &h : msxHeld) h.kc = 0;
  for (int r = 0; r < MSXK_KEYS / 8; r++) {
    for (int b = 0; b < 8; b++)
      if ((msxDown[r] >> b) & 1) msxKeyMatrix(r, b, false);
    msxDown[r] = 0;
  }
}

// With JOYSTICK on, the keys on the joystick map (msxjoymap.cpp: the arrows + Space by default, +
// either SHIFT on the PicoCalc) also drive port 1. Active-low: b0 up b1 down b2 left b3 right
// b4 trigger A b5 trigger B.
static void msxApplyJoystick(const uint8_t *keys, bool shift, bool ctrl, bool alt)
{
  uint8_t pull = 0;
  auto press = [&](uint8_t hid) { const uint8_t c = msxJoymapLookup(hid); if (c64jIsAction(c)) pull |= c; };
  for (int i = 0; i < 6; i++) if (keys[i]) press(keys[i]);
  if (shift) press(C64K_HID_SHIFT);
  if (ctrl)  press(C64K_HID_CTRL);
  if (alt)   press(C64K_HID_ALT);
  msxJoy((uint8_t)~(pull & MSXJ_LINES));
}
// SMS has no keyboard: the keys press control pad 1's buttons (active-low, b0 up b1 down b2 left
// b3 right b4 button 1 b5 button 2). Rebuilt from the held keys + CTRL / ALT on every report
// (smskeymap.cpp: a built-in default, overridden per key by /sms/keymap.txt).
static void smsApplyJoystick(bool ctrl, bool alt, const uint8_t *keys)
{
  uint8_t m = 0xFF;
  auto press = [&](uint8_t hid) { const uint8_t c = smsKeymapLookup(hid); if (smsKIsButton(c)) m &= ~(1 << c); };
  if (ctrl) press(C64K_HID_CTRL);
  if (alt)  press(C64K_HID_ALT);
  for (int i = 0; i < 6; i++) if (keys[i]) press(keys[i]);
  smsSetInput(m);
}
// ColecoVision: the keys press controller 1's stick, side buttons and keypad (colecokeymap.cpp: a
// built-in default - arrows, Space/Z/F4 + X/F5 fire, the digit row / numeric keypad, '*' / '#' on
// Shift-8 / Shift-3 and '-' / '=' - overridden per key by /coleco/keymap.txt). Rebuilt from the held
// keys + CTRL / ALT on every report. The keypad holds a single key, so the first one found wins.
static void colecoApplyJoystick(bool shift, bool ctrl, bool alt, const uint8_t *keys)
{
  uint8_t m = 0xFF;
  int key = -1;
  auto press = [&](uint8_t hid, bool sh) {
    const uint8_t c = colecoKeymapLookup(hid, sh);
    if (!colKIsButton(c)) return;
    if (colKIsKeypad(c)) { if (key < 0) key = c - COLK_KEY0; }
    else m &= ~(1 << c);
  };
  if (ctrl) press(C64K_HID_CTRL, false);
  if (alt)  press(C64K_HID_ALT, false);
  for (int i = 0; i < 6; i++) if (keys[i]) press(keys[i], shift);
  colecoSetInput(m);
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
// The Spectrum's 8 half-rows x 5 keys; zxkeymap.cpp's rules give each key as half-row*5 + bit.
// The keyboard map says which key each host key presses and what it does to CAPS / SYMBOL SHIFT;
// with JOYSTICK on, a key the joystick map gives a Kempston line to only moves the stick.
static void zxApplyKeyboard(uint8_t modifier, const uint8_t *keys)
{
  const bool shift = modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
  uint8_t rows[8] = {0, 0, 0, 0, 0, 0, 0, 0};          // 1 = held
  auto hold = [&](uint8_t idx) { if (idx < ZXK_KEYS) rows[idx / 5] |= (uint8_t)(1 << (idx % 5)); };
  bool forceCaps = false, noCaps = false;
  uint8_t kemp = 0;
  auto press = [&](uint8_t hid) {
    if (joystick) {
      const uint8_t j = zxJoymapLookup(hid);
      if (zxjIsAction(j)) { kemp |= j; return; }
    }
    const uint8_t c = zxKeymapLookup(hid, shift);
    if (!zxkIsKey(c)) return;
    hold(zxkIndex(c));
    switch (zxkMode(c)) {
      case ZXK_CAPS:  forceCaps = true; break;
      case ZXK_PLAIN: noCaps = true; break;
      case ZXK_SYM:   noCaps = true; hold(ZXK_SYM_INDEX); break;
    }
  };
  if (modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) press(C64K_HID_CTRL);
  if (modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT))   press(C64K_HID_ALT);
  for (int i = 0; i < 6; i++) if (keys[i]) press(keys[i]);
  if ((shift && !noCaps) || forceCaps) hold(ZXK_CAPS_INDEX);
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
  bool lalt  = modifier & (KEYBOARD_MODIFIER_LEFTALT   | KEYBOARD_MODIFIER_LEFTGUI);    // Apple: open-apple
  bool ralt  = modifier & (KEYBOARD_MODIFIER_RIGHTALT  | KEYBOARD_MODIFIER_RIGHTGUI);   // Apple: solid-apple

  // SD Manager mode has no core to type into and no settings menu to open (F10 would open one the
  // render loop never draws). Its one key, Ctrl-F6 back to the system menu, is taken upstream.
  if (currentPlatform == PLATFORM_SDMANAGER) return;

  // The C64 / NES KEYBOARD page's "press a key to find it" (optionsUiKeyCapture) takes CTRL / ALT too;
  // they only show up as modifier bits, so catch the ones newly down.
  static uint8_t prevModifier = 0;
  const uint8_t newMods = modifier & ~prevModifier;
  prevModifier = modifier;
  if (OptionsWindow && newMods) {
    if (newMods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) optionsUiKeyCapture(C64K_HID_CTRL, false);
    else if (newMods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT |
                        KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI))   // the Apple tells the two apart
      optionsUiKeyCapture(currentPlatform == PLATFORM_APPLE2 &&
                          !(newMods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_LEFTGUI)) ? A2K_HID_RALT : C64K_HID_ALT, false);
    if (newMods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) optionsUiKeyCapture(C64K_HID_SHIFT, true);
  }

  // --- new key-down events ---
  for (int i = 0; i < 6; i++) {
    uint8_t kc = keys[i];
    if (!kc || kbContains(last, kc)) continue;

    // Settings menu: F10 toggles; while open, the keyboard drives the menu (all platforms).
    // All four arrows browse; Enter flips a toggle, or opens VOLUME / the file list so the
    // arrows then change the value inside it; Escape backs out of whatever Enter opened, and
    // Escape with nothing open closes the window. The joystick keeps its own simpler scheme
    // (optionsUiNav/Adjust/Activate) -- see the note in optionsui.cpp.
    // Ctrl-Shift-F5: the snapshot page (save / load the machine's state), from anywhere but the
    // system menu; again closes it. (The PicoCalc keyboards take it before it gets here.)
    if (kc == HID_KEY_F5 && ctrl && shift) { snapshotWindowToggle(); continue; }

    if (OptionsWindow) {
      if (optionsUiKeyCapture(kc, shift)) continue;   // C64 / NES KEYBOARD page: finding a key
      switch (kc) {
        case HID_KEY_ARROW_LEFT:   optionsUiKeyArrow(-1,  0); break;
        case HID_KEY_ARROW_RIGHT:  optionsUiKeyArrow(+1,  0); break;
        case HID_KEY_ARROW_UP:     optionsUiKeyArrow( 0, -1); break;
        case HID_KEY_ARROW_DOWN:   optionsUiKeyArrow( 0, +1); break;
        case HID_KEY_ENTER:
        case HID_KEY_KEYPAD_ENTER: optionsUiKeyEnter(ctrl, shift); break;
        case HID_KEY_ESCAPE:       if (!optionsUiKeyEscape()) showHideOptionsWindow(); break;
        case HID_KEY_DELETE:
        case HID_KEY_BACKSPACE:    optionsUiKeyUnmount(); break;   // file list: unmount the loaded entry
        case HID_KEY_F10:          showHideOptionsWindow(); break;
#if defined(BOARD_PICOCALC_P4)
        default: {                                         // letters / digits
          char c = 0;
          if (kc >= HID_KEY_A && kc <= HID_KEY_Z) c = (char)('a' + (kc - HID_KEY_A));
          else if (kc >= HID_KEY_1 && kc <= HID_KEY_9) c = (char)('1' + (kc - HID_KEY_1));
          else if (kc == HID_KEY_0) c = '0';
          // snapshot page: S save / L load / D delete; anywhere else the file list jumps to the letter
          if (c && !optionsUiSnapKey(c)) optionsUiKeyLetter(c);
          break;
        }
#else
        case HID_KEY_S:            optionsUiSnapKey('s'); break;   // snapshot page: SAVE (S) / LOAD (L) / DELETE (D)
        case HID_KEY_L:            optionsUiSnapKey('l'); break;
        case HID_KEY_D:            optionsUiSnapKey('d'); break;
#endif
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
#if !UI_PICOCALC   // the PicoCalc's bare F3 arrives as F12 and sits by F4/F5 (buttons)
      if (kc == HID_KEY_F12) { smsHardReset();   continue; }   // soft power-cycle
#endif
    }
    if (currentPlatform == PLATFORM_PCXT && kc == HID_KEY_F12) { pcxtHardReset(); continue; }  // soft reboot
    if (currentPlatform == PLATFORM_COLECO && kc == HID_KEY_F12) { colecoHardReset(); continue; } // power-cycle
    if (currentPlatform == PLATFORM_ZX && kc == HID_KEY_F12) { zxHardReset(); continue; }         // power-cycle

    switch (currentPlatform) {
      case PLATFORM_APPLE2: appleKeyDown(kc, shift, ctrl, lalt, ralt, keys); break;   // apple2keymap.cpp
      case PLATFORM_C64:   c64KeyDown(kc, shift); break;   // every key types; the stick reads the arrows too
      // Arrows and Space are keyboard keys on a real MSX (the BIOS reads them as STICK(0)/STRIG(0)),
      // so they always type; with JOY on they also drive port 1 (msxApplyJoystick).
      case PLATFORM_MSX:   msxKeyDown(kc, shift); break;   // msxkeymap.cpp
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
      case PLATFORM_MSX:   msxKeyUp(kc); break;
      case PLATFORM_PCXT:  pcxtKeyUp(kc); break;   // USB key -> XT break scancode
      default: break;   // Apple keystrokes are edge-triggered (keymem); nothing to release
    }
  }

  // --- continuous modifier state ---
  if (currentPlatform == PLATFORM_C64) {
    c64ApplyModifiers(shift, ctrl, alt, keys);
    if (joystick) c64ApplyJoystick(keys, shift, ctrl, alt);   // the joystick map's keys -> joystick
    else        { c64SetJoystick(0xff); c64SetJoyButtons(0); }   // typing mode: keep the joystick released
  } else if (currentPlatform == PLATFORM_NES) {
    nesApplyKeyboard(ctrl, alt, keys);      // held keys -> controller 1 buttons (neskeymap.cpp)
  } else if (currentPlatform == PLATFORM_ATARI) {
    atariApplyKeyboard(ctrl, alt, keys);    // held keys -> stick + Fire / Select / Reset (atarikeymap.cpp)
  } else if (currentPlatform == PLATFORM_MSX) {
    msxApplyModifiers(shift, ctrl, alt, keys);
    if (joystick) msxApplyJoystick(keys, shift, ctrl, alt);   // the joystick map's keys -> joystick
    else          msxJoy(0xFF);                               // typing mode: joystick released
  } else if (currentPlatform == PLATFORM_SMS) {
    smsApplyJoystick(ctrl, alt, keys);      // held keys -> control pad 1 buttons (smskeymap.cpp)
  } else if (currentPlatform == PLATFORM_COLECO) {
    colecoApplyJoystick(shift, ctrl, alt, keys);   // held keys -> controller 1 (colecokeymap.cpp)
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
    // held keys -> paddles + open-apple / solid-apple (apple2joymap.cpp): by default the arrows,
    // Space / Alt / F5 button 0, right Alt / F4 button 1 (the PicoCalc has one Alt).
    appleApplyJoystick(shift, ctrl, lalt, ralt, keys);
  }
}

// Release everything a keyboard was holding (called when the USB device disconnects).
void usbKeyboardReset()
{
  if (currentPlatform == PLATFORM_C64) {
    c64ReleaseAll();
    c64SetJoystick(0xff);   // release the keyboard joystick
    c64SetJoyButtons(0);
  }
  nesKbBits = 0;
  if (currentPlatform == PLATFORM_NES) nesSetController(0);
  atariKbBits = 0;
  if (currentPlatform == PLATFORM_ATARI) atariApplyBits(0);
  if (currentPlatform == PLATFORM_MSX) {
    msxReleaseAll();
    msxJoy(0xFF);
  }
  if (currentPlatform == PLATFORM_SMS) smsSetInput(0xFF);
  if (currentPlatform == PLATFORM_COLECO) { colecoSetInput(0xFF); colecoSetKeypad(-1); }
  if (currentPlatform == PLATFORM_ZX) { zxKeysReleaseAll(); zxSetKempston(0); }
}

#endif // BOARD_INPUT_USB
