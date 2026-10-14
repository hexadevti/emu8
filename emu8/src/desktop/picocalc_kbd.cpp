// picocalc_kbd.cpp — the PicoCalc keyboard for the desktop simulator (see picocalc_sim.h).
//
// The real keyboard is an STM32 that resolves shift / caps lock itself and reports the resulting
// character (src/picocalc/input_picocalc.cpp explains the protocol). pcSimPhysKey() plays the STM32:
// it turns a physical key plus the modifier state into that code. handleEvent() below is the device
// translation, ported from input_picocalc.cpp: the same HID codes, the same Ctrl hotkeys
// (Ctrl-F1 settings, Ctrl-F3 reset, Ctrl-Shift-F1 system menu, Ctrl-Shift-F3 hard reset) and the same
// keyboard-driven system menu. Only the transport differs.
#if defined(BOARD_DESKTOP) && defined(EMU_SIM_PICOCALC)

#include "../../emu.h"
#include "picocalc_sim.h"
#include "ui_imgui.h"
#include <SDL.h>
#include <string.h>
#include "EspUsbHost.h"   // the shim: HID_KEY_* usage codes + KEYBOARD_MODIFIER_*

enum { PCK_STATE_IDLE = 0, PCK_STATE_PRESSED = 1, PCK_STATE_HOLD = 2, PCK_STATE_RELEASED = 3 };

// ---------------------------------------------------------------------------------------------
// key code -> HID usage code (+ whether the character implies shift) — as input_picocalc.cpp
// ---------------------------------------------------------------------------------------------
struct HidKey { uint8_t hid; bool shift; };

static HidKey asciiToHid(uint8_t c)
{
  if (c >= 'a' && c <= 'z') return { (uint8_t)(HID_KEY_A + (c - 'a')), false };
  if (c >= 'A' && c <= 'Z') return { (uint8_t)(HID_KEY_A + (c - 'A')), true  };
  if (c >= '1' && c <= '9') return { (uint8_t)(HID_KEY_1 + (c - '1')), false };
  if (c == '0')             return { HID_KEY_0, false };
  switch (c) {
    case ' ':  return { HID_KEY_SPACE,         false };
    case '!':  return { HID_KEY_1,             true  };
    case '@':  return { HID_KEY_2,             true  };
    case '#':  return { HID_KEY_3,             true  };
    case '$':  return { HID_KEY_4,             true  };
    case '%':  return { HID_KEY_5,             true  };
    case '^':  return { HID_KEY_6,             true  };
    case '&':  return { HID_KEY_7,             true  };
    case '*':  return { HID_KEY_8,             true  };
    case '(':  return { HID_KEY_9,             true  };
    case ')':  return { HID_KEY_0,             true  };
    case '-':  return { HID_KEY_MINUS,         false };
    case '_':  return { HID_KEY_MINUS,         true  };
    case '=':  return { HID_KEY_EQUAL,         false };
    case '+':  return { HID_KEY_EQUAL,         true  };
    case '[':  return { HID_KEY_BRACKET_LEFT,  false };
    case '{':  return { HID_KEY_BRACKET_LEFT,  true  };
    case ']':  return { HID_KEY_BRACKET_RIGHT, false };
    case '}':  return { HID_KEY_BRACKET_RIGHT, true  };
    case '\\': return { HID_KEY_BACKSLASH,     false };
    case '|':  return { HID_KEY_BACKSLASH,     true  };
    case ';':  return { HID_KEY_SEMICOLON,     false };
    case ':':  return { HID_KEY_SEMICOLON,     true  };
    case '\'': return { HID_KEY_APOSTROPHE,    false };
    case '"':  return { HID_KEY_APOSTROPHE,    true  };
    case '`':  return { HID_KEY_GRAVE,         false };
    case '~':  return { HID_KEY_GRAVE,         true  };
    case ',':  return { HID_KEY_COMMA,         false };
    case '<':  return { HID_KEY_COMMA,         true  };
    case '.':  return { HID_KEY_PERIOD,        false };
    case '>':  return { HID_KEY_PERIOD,        true  };
    case '/':  return { HID_KEY_SLASH,         false };
    case '?':  return { HID_KEY_SLASH,         true  };
  }
  return { HID_KEY_NONE, false };
}

static HidKey pcToHid(uint8_t code)
{
  switch (code) {
    case PCK_BACKSPACE: return { HID_KEY_BACKSPACE,   false };
    case PCK_TAB:       return { HID_KEY_TAB,         false };
    case PCK_ENTER:
    case '\r':          return { HID_KEY_ENTER,       false };
    case PCK_ESC:       return { HID_KEY_ESCAPE,      false };
    case PCK_LEFT:      return { HID_KEY_ARROW_LEFT,  false };
    case PCK_UP:        return { HID_KEY_ARROW_UP,    false };
    case PCK_DOWN:      return { HID_KEY_ARROW_DOWN,  false };
    case PCK_RIGHT:     return { HID_KEY_ARROW_RIGHT, false };
    case PCK_F1:        return { HID_KEY_F1,          false };
    case PCK_F2:        return { HID_KEY_F2,          false };
    case PCK_F3:        return { HID_KEY_F12,         false };   // reset on Atari / PC-XT / Coleco / ZX
    case PCK_F4:        return { HID_KEY_F4,          false };
    case PCK_F5:        return { HID_KEY_F5,          false };
    case PCK_F6:        return { HID_KEY_F6,          false };
    case PCK_F7:        return { HID_KEY_F7,          false };
    case PCK_F8:        return { HID_KEY_F8,          false };
    case PCK_F9:        return { HID_KEY_F9,          false };
    case PCK_BREAK:     return { HID_KEY_PAUSE,       false };
    case PCK_INSERT:    return { HID_KEY_INSERT,      false };
    case PCK_HOME:      return { HID_KEY_HOME,        false };
    case PCK_DEL:       return { HID_KEY_DELETE,      false };
    case PCK_END:       return { HID_KEY_END,         false };
    case PCK_PAGE_UP:   return { HID_KEY_PAGE_UP,     false };
    case PCK_PAGE_DOWN: return { HID_KEY_PAGE_DOWN,   false };
    case PCK_CAPS_LOCK: return { HID_KEY_NONE,        false };
    default: break;
  }
  if (code < 0x80) return asciiToHid(code);
  return { HID_KEY_NONE, false };
}

// ---------------------------------------------------------------------------------------------
// live key state -> HID boot report — as input_picocalc.cpp
// ---------------------------------------------------------------------------------------------
static uint8_t g_keys[6]       = {0, 0, 0, 0, 0, 0};
static bool    g_keyShift[6]   = {false, false, false, false, false, false};
static uint8_t g_lastReport[6] = {0, 0, 0, 0, 0, 0};
static bool    g_shiftL = false, g_shiftR = false, g_ctrlHeld = false, g_altHeld = false, g_symHeld = false;
static bool    g_caps = false;

static bool shiftHeld() { return g_shiftL || g_shiftR; }

static void pressAdd(uint8_t hid, bool shift)
{
  if (!hid || hid >= 0xE0) return;
  for (int i = 0; i < 6; i++)
    if (g_keys[i] == hid) { g_keyShift[i] = shift; return; }
  for (int i = 0; i < 6; i++)
    if (!g_keys[i]) { g_keys[i] = hid; g_keyShift[i] = shift; return; }
}

static void pressRemove(uint8_t hid)
{
  for (int i = 0; i < 6; i++)
    if (g_keys[i] == hid) { g_keys[i] = 0; g_keyShift[i] = false; return; }
}

static uint8_t hidModifiers()
{
  bool shift = shiftHeld();
  for (int i = 0; i < 6; i++) if (g_keys[i] && g_keyShift[i]) shift = true;
  uint8_t mod = 0;
  if (shift)      mod |= KEYBOARD_MODIFIER_LEFTSHIFT;
  if (g_ctrlHeld) mod |= KEYBOARD_MODIFIER_LEFTCTRL;
  if (g_altHeld)  mod |= KEYBOARD_MODIFIER_LEFTALT;
  if (g_symHeld)  mod |= KEYBOARD_MODIFIER_RIGHTALT;
  return mod;
}

static void sendKeyboardReport()
{
  uint8_t keys[6] = {0, 0, 0, 0, 0, 0};
  int n = 0;
  for (int i = 0; i < 6 && n < 6; i++) if (g_keys[i]) keys[n++] = g_keys[i];
  usbKeyboardReport(hidModifiers(), keys, g_lastReport);
  memcpy(g_lastReport, keys, 6);
}

static void tapHid(uint8_t hid)
{
  pressAdd(hid, false);
  sendKeyboardReport();
  pressRemove(hid);
  sendKeyboardReport();
}

static bool applyModifier(uint8_t code, bool down)
{
  switch (code) {
    case PCK_MOD_SHL:  g_shiftL   = down; return true;
    case PCK_MOD_SHR:  g_shiftR   = down; return true;
    case PCK_MOD_CTRL: g_ctrlHeld = down; return true;
    case PCK_MOD_ALT:  g_altHeld  = down; return true;
    case PCK_MOD_SYM:  g_symHeld  = down; return true;
    default: return false;
  }
}

static void handleEvent(uint8_t state, uint8_t code)
{
  bool down;
  switch (state) {
    case PCK_STATE_PRESSED:
    case PCK_STATE_HOLD:     down = true;  break;
    case PCK_STATE_RELEASED: down = false; break;
    default: return;
  }

  if (down) bootHintDismiss();

  if (applyModifier(code, down)) { sendKeyboardReport(); return; }

  // --- Ctrl hotkeys (pressed edge only), as on the device ---
  if (state == PCK_STATE_PRESSED && g_ctrlHeld) {
    if (code == PCK_F8 || (code == PCK_F3 && shiftHeld())) {          // Ctrl-Shift-F3: hard reset
      printLog("Ctrl-Shift-F3: hard reset");
      desktopUiSaveState();
      ESP.restart();
    }
    if (code == PCK_PAGE_UP || (code == PCK_UP && shiftHeld())) {     // Ctrl-Shift-Up: UF2 Loader
      printLog("Ctrl-Shift-Up: the UF2 Loader menu only exists on the device");
      return;
    }
    if (code == PCK_F6 || (code == PCK_F1 && shiftHeld())) {          // Ctrl-Shift-F1: system menu
      printLog("Ctrl-Shift-F1: system menu");
      splashOpen();
      return;
    }
    if (code == PCK_F1) { tapHid(HID_KEY_F10); return; }              // settings menu
    if (code == PCK_F3) { tapHid(HID_KEY_F11); return; }              // CPU reset
  }

  // The system menu is driven from the keyboard (the PicoCalc has no touch panel).
  if (splashActive && !OptionsWindow) {
    if (!down) return;
    switch (code) {
      case PCK_LEFT:  splashKeyEvent = SPLASH_KEY_LEFT;   return;
      case PCK_RIGHT: splashKeyEvent = SPLASH_KEY_RIGHT;  return;
      case PCK_UP:    splashKeyEvent = SPLASH_KEY_UP;     return;
      case PCK_DOWN:  splashKeyEvent = SPLASH_KEY_DOWN;   return;
      case PCK_ENTER: splashKeyEvent = SPLASH_KEY_SELECT; return;
      case PCK_ESC:   splashKeyEvent = SPLASH_KEY_BACK;   return;
      default: break;
    }
  }

  HidKey k = pcToHid(code);
  if (!k.hid) return;

  if (down) {
    bool already = false;
    for (int i = 0; i < 6; i++) if (g_keys[i] == k.hid) { already = true; break; }
    if (already) { pressRemove(k.hid); sendKeyboardReport(); }
    pressAdd(k.hid, k.shift);
  } else {
    pressRemove(k.hid);
  }
  sendKeyboardReport();
}

// ---------------------------------------------------------------------------------------------
// the STM32's part: physical key + modifiers -> key code
// ---------------------------------------------------------------------------------------------
static uint8_t shiftedCode(uint8_t k)
{
  if (k >= 'a' && k <= 'z') return (uint8_t)(k - 'a' + 'A');
  if (k >= PCK_F1 && k <= PCK_F5) return (uint8_t)(k + 5);      // Shift-F1..F5 arrive as F6..F10
  switch (k) {
    case '1': return '!';  case '2': return '@';  case '3': return '#';  case '4': return '$';
    case '5': return '%';  case '6': return '^';  case '7': return '&';  case '8': return '*';
    case '9': return '(';  case '0': return ')';  case '`': return '~';  case '/': return '?';
    case '\\': return '|'; case '-': return '_';  case '=': return '+';  case '[': return '{';
    case ']': return '}';  case ';': return ':';  case '\'': return '"'; case ',': return '<';
    case '.': return '>';
    case PCK_ESC:  return PCK_BREAK;                               // the green legends on the keys
    case PCK_TAB:  return PCK_HOME;
    case PCK_DEL:  return PCK_END;
    case PCK_UP:   return PCK_PAGE_UP;
    case PCK_DOWN: return PCK_PAGE_DOWN;
  }
  return k;
}

static uint8_t g_sent[256];   // per physical key: the code its press sent (0 = up)

void pcSimPhysKey(uint8_t key, bool down, bool repeat)
{
  if (key == PCK_MOD_SHL || key == PCK_MOD_SHR || key == PCK_MOD_CTRL ||
      key == PCK_MOD_ALT || key == PCK_MOD_SYM) {
    if (repeat) return;
    handleEvent(down ? PCK_STATE_PRESSED : PCK_STATE_RELEASED, key);
    return;
  }
  if (down) {
    if (key == PCK_CAPS_LOCK) { if (!repeat) g_caps = !g_caps; return; }   // resolved "inside the STM32"
    uint8_t code;
    if (key >= 'a' && key <= 'z') code = (shiftHeld() != g_caps) ? shiftedCode(key) : key;
    else                          code = shiftHeld() ? shiftedCode(key) : key;
    if (g_altHeld && key == 'i') code = PCK_INSERT;                // the yellow "Ins" legend
    if (repeat && g_sent[key]) code = g_sent[key];
    g_sent[key] = code;
    handleEvent(repeat ? PCK_STATE_HOLD : PCK_STATE_PRESSED, code);
  } else if (g_sent[key]) {
    uint8_t code = g_sent[key];
    g_sent[key] = 0;
    handleEvent(PCK_STATE_RELEASED, code);
  }
}

bool pcSimCapsLock() { return g_caps; }

// Host keyboard -> PicoCalc physical keys. Keys the PicoCalc has no legend for (Insert, Home, End,
// Page Up/Down, Pause, F6..F10) are sent as the codes their PicoCalc combinations produce.
bool pcSimHostKey(int sc, bool down, bool repeat)
{
  uint8_t k = 0;
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) k = (uint8_t)('a' + (sc - SDL_SCANCODE_A));
  else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) k = (uint8_t)('1' + (sc - SDL_SCANCODE_1));
  else if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) k = (uint8_t)(PCK_F1 + (sc - SDL_SCANCODE_F1));
  else switch (sc) {
    case SDL_SCANCODE_0:            k = '0';  break;
    case SDL_SCANCODE_SPACE:        k = ' ';  break;
    case SDL_SCANCODE_GRAVE:        k = '`';  break;
    case SDL_SCANCODE_MINUS:        k = '-';  break;
    case SDL_SCANCODE_EQUALS:       k = '=';  break;
    case SDL_SCANCODE_LEFTBRACKET:  k = '[';  break;
    case SDL_SCANCODE_RIGHTBRACKET: k = ']';  break;
    case SDL_SCANCODE_BACKSLASH:    k = '\\'; break;
    case SDL_SCANCODE_SEMICOLON:    k = ';';  break;
    case SDL_SCANCODE_APOSTROPHE:   k = '\''; break;
    case SDL_SCANCODE_COMMA:        k = ',';  break;
    case SDL_SCANCODE_PERIOD:       k = '.';  break;
    case SDL_SCANCODE_SLASH:        k = '/';  break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:     k = PCK_ENTER;     break;
    case SDL_SCANCODE_BACKSPACE:    k = PCK_BACKSPACE; break;
    case SDL_SCANCODE_TAB:          k = PCK_TAB;       break;
    case SDL_SCANCODE_ESCAPE:       k = PCK_ESC;       break;
    case SDL_SCANCODE_DELETE:       k = PCK_DEL;       break;
    case SDL_SCANCODE_INSERT:       k = PCK_INSERT;    break;
    case SDL_SCANCODE_HOME:         k = PCK_HOME;      break;
    case SDL_SCANCODE_END:          k = PCK_END;       break;
    case SDL_SCANCODE_PAGEUP:       k = PCK_PAGE_UP;   break;
    case SDL_SCANCODE_PAGEDOWN:     k = PCK_PAGE_DOWN; break;
    case SDL_SCANCODE_PAUSE:        k = PCK_BREAK;     break;
    case SDL_SCANCODE_UP:           k = PCK_UP;        break;
    case SDL_SCANCODE_DOWN:         k = PCK_DOWN;      break;
    case SDL_SCANCODE_LEFT:         k = PCK_LEFT;      break;
    case SDL_SCANCODE_RIGHT:        k = PCK_RIGHT;     break;
    case SDL_SCANCODE_CAPSLOCK:     k = PCK_CAPS_LOCK; break;
    case SDL_SCANCODE_LSHIFT:       k = PCK_MOD_SHL;   break;
    case SDL_SCANCODE_RSHIFT:       k = PCK_MOD_SHR;   break;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:        k = PCK_MOD_CTRL;  break;
    case SDL_SCANCODE_LALT:         k = PCK_MOD_ALT;   break;
    case SDL_SCANCODE_RALT:         k = PCK_MOD_SYM;   break;
    default: return false;
  }
  pcSimPhysKey(k, down, repeat);
  return true;
}

// The STM32's battery register (input_picocalc.cpp): a fixed reading, so the gauge shows in the menus.
int  picocalcBatteryPercent()  { return 76; }
bool picocalcBatteryCharging() { return false; }

#endif // BOARD_DESKTOP && EMU_SIM_PICOCALC
