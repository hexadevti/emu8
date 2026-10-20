// apple2keymap.cpp - which Apple II+ key each host key presses (USB keyboard, PicoCalc keyboard,
// the desktop build's PC keyboard: everything that reaches usbKeyboardReport()).
//
// A built-in default map, plus overrides read from a text file on the SD card when the Apple boots:
// /emu8/apple2/keymap.txt (or /apple2keymap.txt at the root). One rule per line, '#' starts a comment:
//
//     <host key> = <Apple key> [+SHIFT | -SHIFT | +CTRL]
//
//     PAGE_UP = RESET              # PgUp is RESET
//     SHIFT+2 = 2 +SHIFT           # Shift-2 types '"' (the Apple's own Shift-2) instead of '@'
//     F9 = K +CTRL                 # F9 sends CTRL-K
//     GRAVE = NONE                 # ` does nothing
//
// A SHIFT+ rule is used when the key is typed with SHIFT and wins over the plain rule; it releases
// the Apple's SHIFT unless +SHIFT / +CTRL says otherwise. A plain rule passes SHIFT through as
// typed. +SHIFT / -SHIFT hold or release it whatever the host keyboard says; +CTRL holds CTRL. Host
// keys are named as in the C64 map (c64keymap.cpp), plus RALT. The full default map is in
// docs/apple2-keymap.txt. The IIe reads /emu8/apple2/keymap-iie.txt instead (docs/apple2e-keymap.txt).
//
// The keys are the II+'s 52 (POWER is a light, not a key): what a key sends comes from the II+
// keyboard encoder (apple2KeyCode), so Shift-M is ']', Shift-N '^', Shift-P '@', and CTRL changes
// the letters only. The default: letters, digits, Space, Return, Esc and the two arrows where they
// sit; shifted punctuation types the character printed on the PC keycap (the PicoCalc sends
// characters, so a keycap '"' must give a '"'); Up / Down / Tab send CTRL-K / CTRL-J / CTRL-I as the
// IIe's keys do; Backspace and Delete are the left arrow; CTRL is CTRL. The keys the II+ cannot type
// ([ \ _ ` { | } ~) do nothing.
//
// The IIe (AppleIIe) has the PC's own layout -- the same ASCII keys in the same places -- so its
// default is one to one: every PC key on the IIe key with the same keycap, SHIFT as typed (the IIe
// encoder makes Shift-2 '@', Shift-; ':' as the PC does). Plus its own keys: DELETE, TAB, the four
// arrows, CAPS LOCK (toggles apple2CapsLock), and OPEN-APPLE / SOLID-APPLE on Alt / right Alt
// (held, they are paddle buttons 0 / 1). Backspace is the left arrow, as Apple II programs expect.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "apple2keymap.h"
#include "c64keymap.h"

static const char *const A2_NAMES[A2K_NKEYS] = {
  "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
  "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
  "COLON", "MINUS", "SEMICOLON", "COMMA", "PERIOD", "SLASH",
  "SPACE", "RETURN", "ESC", "LEFT", "RIGHT", "REPT", "CTRL", "LSHIFT", "RSHIFT",
  "DELETE", "TAB", "UP", "DOWN", "EQUAL", "LBRACKET", "RBRACKET", "BACKSLASH", "QUOTE", "GRAVE",
  "CAPSLOCK", "OPEN-APPLE", "SOLID-APPLE",
};

// What the II+ keyboard encoder sends for each key: plain / with SHIFT (0 = sends nothing).
static const char kChars[A2K_NKEYS][2] = {
  {'A','A'}, {'B','B'}, {'C','C'}, {'D','D'}, {'E','E'}, {'F','F'}, {'G','G'}, {'H','H'}, {'I','I'},
  {'J','J'}, {'K','K'}, {'L','L'}, {'M',']'}, {'N','^'}, {'O','O'}, {'P','@'}, {'Q','Q'}, {'R','R'},
  {'S','S'}, {'T','T'}, {'U','U'}, {'V','V'}, {'W','W'}, {'X','X'}, {'Y','Y'}, {'Z','Z'},
  {'1','!'}, {'2','"'}, {'3','#'}, {'4','$'}, {'5','%'}, {'6','&'}, {'7','\''}, {'8','('}, {'9',')'},
  {'0','0'},
  {':','*'}, {'-','='}, {';','+'}, {',','<'}, {'.','>'}, {'/','?'},
  {' ',' '}, {0x0D,0x0D}, {0x1B,0x1B}, {0x08,0x08}, {0x15,0x15}, {0,0}, {0,0}, {0,0}, {0,0},
  // the IIe's keys (a II+ map naming one still gets the code; its own keyboard has none of them)
  {0x7F,0x7F}, {0x09,0x09}, {0x0B,0x0B}, {0x0A,0x0A}, {'=','+'}, {'[','{'}, {']','}'}, {'\\','|'},
  {'\'','"'}, {'`','~'}, {0,0}, {0,0}, {0,0},
};

// The IIe's encoder: the PC's US layout. Letters are listed capital; apple2KeyCode lowers them.
static const char kCharsE[A2K_NKEYS][2] = {
  {'A','A'}, {'B','B'}, {'C','C'}, {'D','D'}, {'E','E'}, {'F','F'}, {'G','G'}, {'H','H'}, {'I','I'},
  {'J','J'}, {'K','K'}, {'L','L'}, {'M','M'}, {'N','N'}, {'O','O'}, {'P','P'}, {'Q','Q'}, {'R','R'},
  {'S','S'}, {'T','T'}, {'U','U'}, {'V','V'}, {'W','W'}, {'X','X'}, {'Y','Y'}, {'Z','Z'},
  {'1','!'}, {'2','@'}, {'3','#'}, {'4','$'}, {'5','%'}, {'6','^'}, {'7','&'}, {'8','*'}, {'9','('},
  {'0',')'},
  {':','*'}, {'-','_'}, {';',':'}, {',','<'}, {'.','>'}, {'/','?'},
  {' ',' '}, {0x0D,0x0D}, {0x1B,0x1B}, {0x08,0x08}, {0x15,0x15}, {0,0}, {0,0}, {0,0}, {0,0},
  {0x7F,0x7F}, {0x09,0x09}, {0x0B,0x0B}, {0x0A,0x0A}, {'=','+'}, {'[','{'}, {']','}'}, {'\\','|'},
  {'\'','"'}, {'`','~'}, {0,0}, {0,0}, {0,0},
};

bool apple2CapsLock = true;

uint8_t apple2KeyCode(uint8_t index, bool shift, bool ctrl)
{
  if (index >= A2K_NKEYS) return 0;
  if (AppleIIe) {
    uint8_t c = (uint8_t)kCharsE[index][shift ? 1 : 0];
    if (!c) return 0;
    if (index <= A2K_Z && !shift && !apple2CapsLock) c |= 0x20;              // lower case
    if (ctrl && ((c >= '@' && c <= '_') || (c >= 'a' && c <= 'z'))) c &= 0x1F;   // CTRL-A..Z, -@ -[ -\ -] -^ -_
    return c | 0x80;
  }
  uint8_t c = (uint8_t)kChars[index][shift ? 1 : 0];
  if (!c) return 0;
  if (ctrl && index <= A2K_Z) c &= 0x1F;   // CTRL only reaches the letters (CTRL-SHIFT-P = $80)
  return c | 0x80;
}

#define KEEP(k) (uint8_t)(k)
#define SON(k)  (uint8_t)((A2K_SHIFT_ON  << 6) | (k))
#define SOFF(k) (uint8_t)((A2K_SHIFT_OFF << 6) | (k))
#define CTL(k)  (uint8_t)((A2K_CTRL_ON   << 6) | (k))

// HID usage codes (the standard USB HID keyboard page), numeric so every board's header works.
enum : uint8_t {
  H_A = 0x04, H_ENTER = 0x28, H_ESC, H_BACKSPACE, H_TAB, H_SPACE, H_MINUS, H_EQUAL,
  H_LBRACKET, H_RBRACKET, H_BACKSLASH, H_NONUS_HASH, H_SEMICOLON, H_APOSTROPHE, H_GRAVE,
  H_COMMA, H_PERIOD, H_SLASH,
  H_DELETE = 0x4C, H_RIGHT = 0x4F, H_LEFT, H_DOWN, H_UP, H_NUMLOCK, H_KP_DIVIDE, H_KP_MULTIPLY,
  H_KP_MINUS, H_KP_PLUS, H_KP_ENTER, H_KP_1, /* .. H_KP_1+8 = KP_9 */ H_KP_0 = 0x62, H_KP_PERIOD,
};
#define H_1 0x1E   // 1..9, then 0 at 0x27
#define H_CAPSLOCK 0x39

struct KeyRule { uint8_t hid, shifted, code; };

static const KeyRule kDefault[] = {
  // letters; the PC's Shift-M/N/P type the capital, not the II+'s ] ^ @
  {H_A+0,0,A2K_A+0}, {H_A+1,0,A2K_A+1}, {H_A+2,0,A2K_A+2}, {H_A+3,0,A2K_A+3}, {H_A+4,0,A2K_A+4},
  {H_A+5,0,A2K_A+5}, {H_A+6,0,A2K_A+6}, {H_A+7,0,A2K_A+7}, {H_A+8,0,A2K_A+8}, {H_A+9,0,A2K_A+9},
  {H_A+10,0,A2K_A+10}, {H_A+11,0,A2K_A+11}, {H_A+12,0,A2K_A+12}, {H_A+13,0,A2K_A+13},
  {H_A+14,0,A2K_A+14}, {H_A+15,0,A2K_A+15}, {H_A+16,0,A2K_A+16}, {H_A+17,0,A2K_A+17},
  {H_A+18,0,A2K_A+18}, {H_A+19,0,A2K_A+19}, {H_A+20,0,A2K_A+20}, {H_A+21,0,A2K_A+21},
  {H_A+22,0,A2K_A+22}, {H_A+23,0,A2K_A+23}, {H_A+24,0,A2K_A+24}, {H_A+25,0,A2K_A+25},
  {H_A+12,1,SOFF(A2K_A+12)}, {H_A+13,1,SOFF(A2K_A+13)}, {H_A+15,1,SOFF(A2K_A+15)},
  // digits; Shift-1/3/4/5 ('!' '#' '$' '%') are the same on both, the rest are the keycap's symbol
  {H_1+0,0,A2K_1+0}, {H_1+1,0,A2K_1+1}, {H_1+2,0,A2K_1+2}, {H_1+3,0,A2K_1+3}, {H_1+4,0,A2K_1+4},
  {H_1+5,0,A2K_1+5}, {H_1+6,0,A2K_1+6}, {H_1+7,0,A2K_1+7}, {H_1+8,0,A2K_1+8}, {H_1+9,0,A2K_0},
  {H_1+1,1,SON(A2K_A+15)},       // Shift-2 @   (II+: Shift-P)
  {H_1+5,1,SON(A2K_A+13)},       // Shift-6 ^   (II+: Shift-N)
  {H_1+6,1,SON(A2K_1+5)},        // Shift-7 &
  {H_1+7,1,SON(A2K_COLON)},      // Shift-8 *
  {H_1+8,1,SON(A2K_1+7)},        // Shift-9 (
  {H_1+9,1,SON(A2K_1+8)},        // Shift-0 )
  // punctuation
  {H_MINUS,0,A2K_MINUS},           {H_MINUS,1,A2K_UNMAP},                // -   _ (no _ on a II+)
  {H_EQUAL,0,SON(A2K_MINUS)},      {H_EQUAL,1,SON(A2K_SEMICOLON)},       // =   +
  {H_RBRACKET,0,SON(A2K_A+12)},                                          // ]   (II+: Shift-M)
  {H_SEMICOLON,0,A2K_SEMICOLON},   {H_SEMICOLON,1,SOFF(A2K_COLON)},      // ;   :
  {H_APOSTROPHE,0,SON(A2K_1+6)},   {H_APOSTROPHE,1,SON(A2K_1+1)},        // '   "
  {H_COMMA,0,A2K_COMMA}, {H_PERIOD,0,A2K_PERIOD}, {H_SLASH,0,A2K_SLASH}, // , . /  (< > ? shifted)
  {H_SPACE,0,A2K_SPACE},
  // editing / control
  {H_ENTER,0,A2K_RETURN}, {H_KP_ENTER,0,A2K_RETURN}, {H_ESC,0,A2K_ESC},
  {H_BACKSPACE,0,A2K_LEFT}, {H_DELETE,0,A2K_LEFT},
  {H_LEFT,0,A2K_LEFT}, {H_RIGHT,0,A2K_RIGHT},
  {H_UP,0,CTL(A2K_A+10)}, {H_DOWN,0,CTL(A2K_A+9)}, {H_TAB,0,CTL(A2K_A+8)},   // CTRL-K / -J / -I
  {C64K_HID_CTRL,0,A2K_CTRL},
  // keypad
  {H_KP_1+0,0,A2K_1+0}, {H_KP_1+1,0,A2K_1+1}, {H_KP_1+2,0,A2K_1+2}, {H_KP_1+3,0,A2K_1+3},
  {H_KP_1+4,0,A2K_1+4}, {H_KP_1+5,0,A2K_1+5}, {H_KP_1+6,0,A2K_1+6}, {H_KP_1+7,0,A2K_1+7},
  {H_KP_1+8,0,A2K_1+8}, {H_KP_0,0,A2K_0},
  {H_KP_PLUS,0,SON(A2K_SEMICOLON)}, {H_KP_MINUS,0,SOFF(A2K_MINUS)}, {H_KP_MULTIPLY,0,SON(A2K_COLON)},
  {H_KP_DIVIDE,0,SOFF(A2K_SLASH)}, {H_KP_PERIOD,0,SOFF(A2K_PERIOD)},
};

// The IIe's default: one to one (see the top of the file).
static const KeyRule kDefaultE[] = {
  {H_A+0,0,A2K_A+0}, {H_A+1,0,A2K_A+1}, {H_A+2,0,A2K_A+2}, {H_A+3,0,A2K_A+3}, {H_A+4,0,A2K_A+4},
  {H_A+5,0,A2K_A+5}, {H_A+6,0,A2K_A+6}, {H_A+7,0,A2K_A+7}, {H_A+8,0,A2K_A+8}, {H_A+9,0,A2K_A+9},
  {H_A+10,0,A2K_A+10}, {H_A+11,0,A2K_A+11}, {H_A+12,0,A2K_A+12}, {H_A+13,0,A2K_A+13},
  {H_A+14,0,A2K_A+14}, {H_A+15,0,A2K_A+15}, {H_A+16,0,A2K_A+16}, {H_A+17,0,A2K_A+17},
  {H_A+18,0,A2K_A+18}, {H_A+19,0,A2K_A+19}, {H_A+20,0,A2K_A+20}, {H_A+21,0,A2K_A+21},
  {H_A+22,0,A2K_A+22}, {H_A+23,0,A2K_A+23}, {H_A+24,0,A2K_A+24}, {H_A+25,0,A2K_A+25},
  {H_1+0,0,A2K_1+0}, {H_1+1,0,A2K_1+1}, {H_1+2,0,A2K_1+2}, {H_1+3,0,A2K_1+3}, {H_1+4,0,A2K_1+4},
  {H_1+5,0,A2K_1+5}, {H_1+6,0,A2K_1+6}, {H_1+7,0,A2K_1+7}, {H_1+8,0,A2K_1+8}, {H_1+9,0,A2K_0},
  {H_MINUS,0,A2K_MINUS}, {H_EQUAL,0,A2K_EQUAL}, {H_LBRACKET,0,A2K_LBRACKET}, {H_RBRACKET,0,A2K_RBRACKET},
  {H_BACKSLASH,0,A2K_BACKSLASH}, {H_SEMICOLON,0,A2K_SEMICOLON}, {H_APOSTROPHE,0,A2K_QUOTE},
  {H_GRAVE,0,A2K_GRAVE}, {H_COMMA,0,A2K_COMMA}, {H_PERIOD,0,A2K_PERIOD}, {H_SLASH,0,A2K_SLASH},
  {H_SPACE,0,A2K_SPACE},
  {H_ENTER,0,A2K_RETURN}, {H_KP_ENTER,0,A2K_RETURN}, {H_ESC,0,A2K_ESC}, {H_TAB,0,A2K_TAB},
  {H_BACKSPACE,0,A2K_LEFT}, {H_DELETE,0,A2K_DELETE},
  {H_LEFT,0,A2K_LEFT}, {H_RIGHT,0,A2K_RIGHT}, {H_UP,0,A2K_UP}, {H_DOWN,0,A2K_DOWN},
  {C64K_HID_CTRL,0,A2K_CTRL}, {H_CAPSLOCK,0,A2K_CAPS},
  {C64K_HID_ALT,0,A2K_OAPPLE}, {A2K_HID_RALT,0,A2K_SAPPLE},
  {H_KP_1+0,0,A2K_1+0}, {H_KP_1+1,0,A2K_1+1}, {H_KP_1+2,0,A2K_1+2}, {H_KP_1+3,0,A2K_1+3},
  {H_KP_1+4,0,A2K_1+4}, {H_KP_1+5,0,A2K_1+5}, {H_KP_1+6,0,A2K_1+6}, {H_KP_1+7,0,A2K_1+7},
  {H_KP_1+8,0,A2K_1+8}, {H_KP_0,0,A2K_0},
  {H_KP_PLUS,0,SON(A2K_EQUAL)}, {H_KP_MINUS,0,SOFF(A2K_MINUS)}, {H_KP_MULTIPLY,0,SON(A2K_1+7)},
  {H_KP_DIVIDE,0,SOFF(A2K_SLASH)}, {H_KP_PERIOD,0,SOFF(A2K_PERIOD)},
};

// Overrides from the SD card. Rules equal to the default are not stored, so a copy of the whole
// default file costs nothing; only real changes take a slot.
#define A2K_MAX_OVERRIDES 64
static KeyRule kOver[A2K_MAX_OVERRIDES];
static uint8_t kOverCount = 0;

static uint8_t findRule(const KeyRule *t, int n, uint8_t hid, uint8_t shifted)
{
  for (int i = 0; i < n; i++) if (t[i].hid == hid && t[i].shifted == shifted) return t[i].code;
  return A2K_NONE;
}
static uint8_t findDefault(uint8_t hid, uint8_t shifted)
{
  if (AppleIIe) return findRule(kDefaultE, sizeof(kDefaultE) / sizeof(kDefaultE[0]), hid, shifted);
  return findRule(kDefault, sizeof(kDefault) / sizeof(kDefault[0]), hid, shifted);
}

uint8_t apple2KeymapLookup(uint8_t hid, bool shift)
{
  uint8_t c;
  if (shift) {
    if ((c = findRule(kOver, kOverCount, hid, 1)) != A2K_NONE) return c;
    if ((c = findDefault(hid, 1)) != A2K_NONE) return c;
  }
  if ((c = findRule(kOver, kOverCount, hid, 0)) != A2K_NONE) return c;
  return findDefault(hid, 0);
}

// ---- names ----------------------------------------------------------------------------------
uint8_t apple2KeymapHostParse(const char *s)
{
  if (!strcmp(s, "RALT")) return A2K_HID_RALT;
  return c64KeymapHostParse(s);
}

void apple2KeymapHostName(uint8_t hid, char *out, size_t n)
{
  if (hid == A2K_HID_RALT) snprintf(out, n, "RALT");
  else c64KeymapHostName(hid, out, n);
}

int     apple2KeymapHostCount()   { return c64KeymapHostCount() + 1; }
uint8_t apple2KeymapHostAt(int i) { return i < c64KeymapHostCount() ? c64KeymapHostAt(i) : (uint8_t)A2K_HID_RALT; }

const char *apple2KeymapKeyName(uint8_t index) { return index < A2K_NKEYS ? A2_NAMES[index] : "?"; }
// The IIe's keys in the picker: the II+'s order, without COLON and REPT, with its own keys added.
static const uint8_t kPickE[] = {
  A2K_A+0, A2K_A+1, A2K_A+2, A2K_A+3, A2K_A+4, A2K_A+5, A2K_A+6, A2K_A+7, A2K_A+8, A2K_A+9, A2K_A+10,
  A2K_A+11, A2K_A+12, A2K_A+13, A2K_A+14, A2K_A+15, A2K_A+16, A2K_A+17, A2K_A+18, A2K_A+19, A2K_A+20,
  A2K_A+21, A2K_A+22, A2K_A+23, A2K_A+24, A2K_A+25,
  A2K_1+0, A2K_1+1, A2K_1+2, A2K_1+3, A2K_1+4, A2K_1+5, A2K_1+6, A2K_1+7, A2K_1+8, A2K_0,
  A2K_MINUS, A2K_EQUAL, A2K_LBRACKET, A2K_RBRACKET, A2K_BACKSLASH, A2K_SEMICOLON, A2K_QUOTE,
  A2K_GRAVE, A2K_COMMA, A2K_PERIOD, A2K_SLASH,
  A2K_SPACE, A2K_RETURN, A2K_ESC, A2K_TAB, A2K_DELETE, A2K_LEFT, A2K_RIGHT, A2K_UP, A2K_DOWN,
  A2K_CTRL, A2K_LSHIFT, A2K_RSHIFT, A2K_CAPS, A2K_OAPPLE, A2K_SAPPLE,
};
#define A2K_NPICKE ((int)(sizeof(kPickE) / sizeof(kPickE[0])))
#define A2K_NKEYS_II A2K_DELETE   // the II+'s keys are the first ones
int     apple2KeymapKeyCount()   { return AppleIIe ? A2K_NPICKE : A2K_NKEYS_II; }
uint8_t apple2KeymapKeyAt(int i) { return AppleIIe ? kPickE[i % A2K_NPICKE] : (uint8_t)(i % A2K_NKEYS_II); }

// Apple key name -> key index, A2K_UNMAP / A2K_RESET, or A2K_NONE when unknown.
static uint8_t appleKey(const char *s)
{
  if (!strcmp(s, "NONE"))  return A2K_UNMAP;
  if (!strcmp(s, "RESET")) return A2K_RESET;
  static const struct { const char *sym; uint8_t k; } kSym[] = {
    {":", A2K_COLON}, {"-", A2K_MINUS}, {";", A2K_SEMICOLON}, {",", A2K_COMMA}, {".", A2K_PERIOD},
    {"/", A2K_SLASH}, {"SHIFT", A2K_LSHIFT}, {"REPEAT", A2K_REPT}, {"ESCAPE", A2K_ESC},
    {"=", A2K_EQUAL}, {"[", A2K_LBRACKET}, {"]", A2K_RBRACKET}, {"\\", A2K_BACKSLASH},
    {"'", A2K_QUOTE}, {"APOSTROPHE", A2K_QUOTE}, {"`", A2K_GRAVE}, {"CAPS", A2K_CAPS},
    {"OA", A2K_OAPPLE}, {"SA", A2K_SAPPLE}, {"CLOSED-APPLE", A2K_SAPPLE},
  };
  for (auto &e : kSym) if (!strcmp(s, e.sym)) return e.k;
  for (int i = 0; i < A2K_NKEYS; i++) if (!strcmp(s, A2_NAMES[i])) return (uint8_t)i;
  return A2K_NONE;
}

// ---- file parsing ---------------------------------------------------------------------------
static void keymapError(int line, const char *what, const char *tok)
{
  snprintf(buf, sizeof(buf), "Apple keymap line %d: %s '%s' - ignored", line, what, tok);
  printLog(buf);
}

static void parseLine(char *s, int line)
{
  if (char *h = strchr(s, '#')) *h = 0;
  char *tok[5];
  int n = 0;
  for (char *p = strtok(s, " \t\r"); p && n < 5; p = strtok(nullptr, " \t\r")) {
    for (char *q = p; *q; q++) *q = (char)toupper((unsigned char)*q);
    tok[n++] = p;
  }
  if (n == 0) return;
  if (n < 3 || n > 4 || strcmp(tok[1], "=")) { keymapError(line, "expected 'KEY = APPLEKEY', got", tok[0]); return; }

  const char *src = tok[0];
  uint8_t shifted = 0;
  if (!strncmp(src, "SHIFT+", 6)) { shifted = 1; src += 6; }
  uint8_t hid = apple2KeymapHostParse(src);
  if (!hid) { keymapError(line, "unknown host key", tok[0]); return; }

  uint8_t code = appleKey(tok[2]);
  if (code == A2K_NONE) { keymapError(line, "unknown Apple key", tok[2]); return; }
  if (a2kIsKey(code)) {
    int mode = shifted ? A2K_SHIFT_OFF : A2K_SHIFT_KEEP;
    if (n == 4) {
      if (!strcmp(tok[3], "+SHIFT"))      mode = A2K_SHIFT_ON;
      else if (!strcmp(tok[3], "-SHIFT")) mode = A2K_SHIFT_OFF;
      else if (!strcmp(tok[3], "+CTRL"))  mode = A2K_CTRL_ON;
      else { keymapError(line, "expected +SHIFT, -SHIFT or +CTRL, got", tok[3]); return; }
    }
    if (a2kIsPlainOnly(code)) mode = A2K_SHIFT_KEEP;   // held keys: no mode (see apple2keymap.h)
    code = (uint8_t)((mode << 6) | code);
  }

  if (findDefault(hid, shifted) == code) return;   // same as the built-in rule
  for (int i = 0; i < kOverCount; i++)              // a later line replaces an earlier one
    if (kOver[i].hid == hid && kOver[i].shifted == shifted) { kOver[i].code = code; return; }
  if (kOverCount >= A2K_MAX_OVERRIDES) { keymapError(line, "too many changed keys, at", tok[0]); return; }
  kOver[kOverCount++] = {hid, shifted, code};
}

void apple2KeymapLoad()
{
  kOverCount = 0;
  // The IIe has its own file: its default differs, so the II+ file read on it would be all changes.
  static const char *const kPathsII[] = { "/emu8/apple2/keymap.txt", "/apple2/keymap.txt", "/apple2keymap.txt" };
  static const char *const kPathsIIe[] = { "/emu8/apple2/keymap-iie.txt", "/apple2/keymap-iie.txt", "/apple2keymap-iie.txt" };
  const char *const *kPaths = AppleIIe ? kPathsIIe : kPathsII;
  for (int pi = 0; pi < 3; pi++) {
    const char *path = kPaths[pi];
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("Apple keymap: out of heap opening the file"); return; }
    if (!f) continue;
    char line[96];
    int len = 0, lineNo = 1;
    for (;;) {
      int c = f.read();
      if (c < 0 || c == '\n') {
        line[len] = 0;
        parseLine(line, lineNo++);
        len = 0;
        if (c < 0) break;
      } else if (len < (int)sizeof(line) - 1) {
        line[len++] = (char)c;
      }
    }
    f.close();
    snprintf(buf, sizeof(buf), "Apple keymap: %s, %d key(s) changed from the default", path, kOverCount);
    printLog(buf);
    return;
  }
}

// ---- the KEYBOARD page (optionsui.cpp) ------------------------------------------------------
void apple2KeymapDescribe(uint8_t code, char *out, size_t n)
{
  if (code == A2K_NONE)  { snprintf(out, n, "-");     return; }
  if (code == A2K_UNMAP) { snprintf(out, n, "NONE");  return; }
  if (code == A2K_RESET) { snprintf(out, n, "RESET"); return; }
  static const char *const kSfx[4] = { "", " +SHIFT", " -SHIFT", " +CTRL" };
  snprintf(out, n, "%s%s", apple2KeymapKeyName(a2kIndex(code)), kSfx[a2kMode(code)]);
}

uint8_t apple2KeymapDefault(uint8_t hid, bool shifted) { return findDefault(hid, shifted ? 1 : 0); }

uint8_t apple2KeymapRule(uint8_t hid, bool shifted, bool *over)
{
  uint8_t c = findRule(kOver, kOverCount, hid, shifted ? 1 : 0);
  if (over) *over = c != A2K_NONE;
  return c != A2K_NONE ? c : apple2KeymapDefault(hid, shifted);
}

bool apple2KeymapSet(uint8_t hid, bool shifted, uint8_t code)
{
  const uint8_t sh = shifted ? 1 : 0;
  // A SHIFT+ rule has no "as typed": SHIFT is down whenever it applies, so that is +SHIFT. Keeps
  // the file unambiguous too (a SHIFT+ line without a suffix reads as -SHIFT).
  if (a2kIsKey(code) && a2kIsPlainOnly(a2kIndex(code))) code = a2kIndex(code);   // held: no mode
  else if (sh && a2kIsKey(code) && a2kMode(code) == A2K_SHIFT_KEEP) code = (uint8_t)((A2K_SHIFT_ON << 6) | code);
  int at = -1;
  for (int i = 0; i < kOverCount; i++) if (kOver[i].hid == hid && kOver[i].shifted == sh) at = i;
  if (code == A2K_NONE || code == findDefault(hid, sh)) {          // back to the default
    if (at >= 0) kOver[at] = kOver[--kOverCount];
    return true;
  }
  if (at >= 0) { kOver[at].code = code; return true; }
  if (kOverCount >= A2K_MAX_OVERRIDES) return false;
  kOver[kOverCount++] = {hid, sh, code};
  return true;
}

void apple2KeymapResetAll()   { kOverCount = 0; }
int  apple2KeymapChangedCount() { return kOverCount; }

bool apple2KeymapSave()
{
  const char *const kPath = AppleIIe ? "/emu8/apple2/keymap-iie.txt" : "/emu8/apple2/keymap.txt";
  // Timed lock, not busTake(): this runs in the render task (the menu).
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  bool ok = false;
  try {
    if (!FSTYPE.exists("/emu8")) FSTYPE.mkdir("/emu8");
    if (!FSTYPE.exists("/emu8/apple2")) FSTYPE.mkdir("/emu8/apple2");
    if (FSTYPE.exists(kPath)) FSTYPE.remove(kPath);     // FILE_WRITE appends on arduino-pico
    File f = FSTYPE.open(kPath, FILE_WRITE);
    if (f) {
      static const char kHead[] =
        "# emu8 Apple II keyboard map (II+: keymap.txt, IIe: keymap-iie.txt), saved from the Apple menu (CONFIG > KEYBOARD), which rewrites it.\n"
        "# Only the keys changed from the built-in default are listed; the syntax and the whole\n"
        "# default map are in docs/apple2-keymap.txt.\n";
      ok = f.write((const uint8_t *)kHead, sizeof(kHead) - 1) == sizeof(kHead) - 1;
      char line[64], name[16], rule[24];
      for (int i = 0; ok && i < kOverCount; i++) {
        apple2KeymapHostName(kOver[i].hid, name, sizeof(name));
        apple2KeymapDescribe(kOver[i].code, rule, sizeof(rule));
        int len = snprintf(line, sizeof(line), "%s%s = %s\n", kOver[i].shifted ? "SHIFT+" : "", name, rule);
        ok = f.write((const uint8_t *)line, len) == (size_t)len;
      }
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "Apple keymap: %s %s, %d key(s) changed", kPath, ok ? "saved" : "NOT saved", kOverCount);
  printLog(buf);
  return ok;
}

#endif // BOARD_INPUT_USB
