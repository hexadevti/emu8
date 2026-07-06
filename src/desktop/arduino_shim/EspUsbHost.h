// EspUsbHost.h — desktop shim providing the HID usage-code constants, keyboard-modifier bits, and
// the HID_KEYCODE_TO_ASCII table that src/shared/usbkeyboard.cpp expands. On the device these come
// from the EspUsbHost library (the real USB-host driver, usbgamepad.cpp, is NOT compiled on desktop;
// the SDL input backend translates SDL keysyms -> these HID usage codes -> usbKeyboardReport()).
#pragma once

#include <cstdint>

// --- keyboard modifier bits (USB HID boot protocol, byte 0) ---
#define KEYBOARD_MODIFIER_LEFTCTRL   (1 << 0)
#define KEYBOARD_MODIFIER_LEFTSHIFT  (1 << 1)
#define KEYBOARD_MODIFIER_LEFTALT    (1 << 2)
#define KEYBOARD_MODIFIER_LEFTGUI    (1 << 3)
#define KEYBOARD_MODIFIER_RIGHTCTRL  (1 << 4)
#define KEYBOARD_MODIFIER_RIGHTSHIFT (1 << 5)
#define KEYBOARD_MODIFIER_RIGHTALT   (1 << 6)
#define KEYBOARD_MODIFIER_RIGHTGUI   (1 << 7)

// --- HID usage IDs, keyboard/keypad page 0x07 (USB HID Usage Tables) ---
enum {
  HID_KEY_NONE = 0x00,
  HID_KEY_A = 0x04, HID_KEY_B, HID_KEY_C, HID_KEY_D, HID_KEY_E, HID_KEY_F, HID_KEY_G, HID_KEY_H,
  HID_KEY_I, HID_KEY_J, HID_KEY_K, HID_KEY_L, HID_KEY_M, HID_KEY_N, HID_KEY_O, HID_KEY_P,
  HID_KEY_Q, HID_KEY_R, HID_KEY_S, HID_KEY_T, HID_KEY_U, HID_KEY_V, HID_KEY_W, HID_KEY_X,
  HID_KEY_Y, HID_KEY_Z,                                   // 0x04..0x1D
  HID_KEY_1, HID_KEY_2, HID_KEY_3, HID_KEY_4, HID_KEY_5,
  HID_KEY_6, HID_KEY_7, HID_KEY_8, HID_KEY_9, HID_KEY_0,  // 0x1E..0x27
  HID_KEY_ENTER, HID_KEY_ESCAPE, HID_KEY_BACKSPACE, HID_KEY_TAB, HID_KEY_SPACE,   // 0x28..0x2C
  HID_KEY_MINUS, HID_KEY_EQUAL, HID_KEY_BRACKET_LEFT, HID_KEY_BRACKET_RIGHT,      // 0x2D..0x30
  HID_KEY_BACKSLASH, HID_KEY_EUROPE_1, HID_KEY_SEMICOLON, HID_KEY_APOSTROPHE,     // 0x31..0x34
  HID_KEY_GRAVE, HID_KEY_COMMA, HID_KEY_PERIOD, HID_KEY_SLASH, HID_KEY_CAPS_LOCK, // 0x35..0x39
  HID_KEY_F1, HID_KEY_F2, HID_KEY_F3, HID_KEY_F4, HID_KEY_F5, HID_KEY_F6,
  HID_KEY_F7, HID_KEY_F8, HID_KEY_F9, HID_KEY_F10, HID_KEY_F11, HID_KEY_F12,      // 0x3A..0x45
  HID_KEY_PRINT_SCREEN, HID_KEY_SCROLL_LOCK, HID_KEY_PAUSE, HID_KEY_INSERT,       // 0x46..0x49
  HID_KEY_HOME, HID_KEY_PAGE_UP, HID_KEY_DELETE, HID_KEY_END, HID_KEY_PAGE_DOWN,  // 0x4A..0x4E
  HID_KEY_ARROW_RIGHT, HID_KEY_ARROW_LEFT, HID_KEY_ARROW_DOWN, HID_KEY_ARROW_UP,  // 0x4F..0x52
  HID_KEY_NUM_LOCK, HID_KEY_KEYPAD_DIVIDE, HID_KEY_KEYPAD_MULTIPLY,               // 0x53..0x55
  HID_KEY_KEYPAD_SUBTRACT, HID_KEY_KEYPAD_ADD, HID_KEY_KEYPAD_ENTER,              // 0x56..0x58
  HID_KEY_KEYPAD_1, HID_KEY_KEYPAD_2, HID_KEY_KEYPAD_3, HID_KEY_KEYPAD_4,
  HID_KEY_KEYPAD_5, HID_KEY_KEYPAD_6, HID_KEY_KEYPAD_7, HID_KEY_KEYPAD_8,
  HID_KEY_KEYPAD_9, HID_KEY_KEYPAD_0, HID_KEY_KEYPAD_DECIMAL,                     // 0x59..0x63
  // modifier-key usage IDs (PC-XT path emits make/break for these)
  HID_KEY_CONTROL_LEFT = 0xE0, HID_KEY_SHIFT_LEFT, HID_KEY_ALT_LEFT, HID_KEY_GUI_LEFT,
  HID_KEY_CONTROL_RIGHT, HID_KEY_SHIFT_RIGHT, HID_KEY_ALT_RIGHT, HID_KEY_GUI_RIGHT,
};

// US-layout HID keycode -> { unshifted, shifted } ASCII. Matches the TinyUSB table; consumed as
//   static const uint8_t conv[128][2] = { HID_KEYCODE_TO_ASCII };
// (fewer than 128 entries is fine — the remainder zero-fills; kbToAscii() guards kc < 128.)
#define HID_KEYCODE_TO_ASCII \
  {0,0},{0,0},{0,0},{0,0},                                   /* 0x00..0x03 */ \
  {'a','A'},{'b','B'},{'c','C'},{'d','D'},{'e','E'},{'f','F'},{'g','G'},{'h','H'}, \
  {'i','I'},{'j','J'},{'k','K'},{'l','L'},{'m','M'},{'n','N'},{'o','O'},{'p','P'}, \
  {'q','Q'},{'r','R'},{'s','S'},{'t','T'},{'u','U'},{'v','V'},{'w','W'},{'x','X'}, \
  {'y','Y'},{'z','Z'},                                       /* 0x04..0x1D */ \
  {'1','!'},{'2','@'},{'3','#'},{'4','$'},{'5','%'},{'6','^'},{'7','&'},{'8','*'}, \
  {'9','('},{'0',')'},                                       /* 0x1E..0x27 */ \
  {'\r','\r'},{'\x1b','\x1b'},{'\b','\b'},{'\t','\t'},{' ',' '},  /* 0x28..0x2C */ \
  {'-','_'},{'=','+'},{'[','{'},{']','}'},{'\\','|'},{'#','~'},   /* 0x2D..0x32 */ \
  {';',':'},{'\'','"'},{'`','~'},{',','<'},{'.','>'},{'/','?'},   /* 0x33..0x38 */ \
  {0,0},                                                     /* 0x39 caps lock */ \
  {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0}, /* F1-F12 0x3A..0x45 */ \
  {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},      /* 0x46..0x4E */ \
  {0,0},{0,0},{0,0},{0,0},                                   /* arrows 0x4F..0x52 */ \
  {0,0},                                                     /* 0x53 num lock */ \
  {'/','/'},{'*','*'},{'-','-'},{'+','+'},{'\r','\r'},        /* keypad 0x54..0x58 */ \
  {'1','1'},{'2','2'},{'3','3'},{'4','4'},{'5','5'},{'6','6'},{'7','7'},{'8','8'}, \
  {'9','9'},{'0','0'},{'.','.'}                              /* keypad 0x59..0x63 */
