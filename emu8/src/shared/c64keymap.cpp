// c64keymap.cpp - which C64 key each host key presses (USB keyboard, PicoCalc keyboard, the
// desktop build's PC keyboard: everything that reaches usbKeyboardReport()).
//
// A built-in default map, plus overrides read from a text file on the SD card when the C64 boots:
// /c64/keymap.txt (or /c64keymap.txt at the root). One rule per line, '#' starts a comment:
//
//     <host key> = <C64 key> [+SHIFT | -SHIFT]
//
//     PAGE_DOWN = RUNSTOP          # PgDn is RUN/STOP too
//     SHIFT+2 = 2 +SHIFT           # Shift-2 types '"' (the C64's own Shift-2) instead of '@'
//     F9 = NONE                    # F9 does nothing
//
// A SHIFT+ rule is used when the key is typed with SHIFT and wins over the plain rule; it releases
// the C64's SHIFT unless +SHIFT says otherwise. A plain rule passes SHIFT through as typed.
// +SHIFT / -SHIFT hold or release it whatever the host keyboard says. The full default map, with
// every name, is in docs/c64-keymap.txt.
//
// The default: letters, digits, the cursor and function keys sit where they do on a C64; shifted
// punctuation types the character printed on the PC keycap (the PicoCalc sends characters, so a
// keycap '"' must give a '"'); the keys a PC lacks are £ = '\', up-arrow = Shift-6, left-arrow =
// '`', CLR/HOME = Home (CLR = End), INST = Insert, RESTORE = Page Up, CTRL = Tab or Ctrl, C= = Alt.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "c64keymap.h"

// C64 keys by matrix index (col*8 + row). The enum gives the default table readable names.
enum : uint8_t {
  C_DEL, C_RETURN, C_CRSR_LR, C_F7, C_F1, C_F3, C_F5, C_CRSR_UD,
  C_3, C_W, C_A, C_4, C_Z, C_S, C_E, C_LSHIFT,
  C_5, C_R, C_D, C_6, C_C, C_F, C_T, C_X,
  C_7, C_Y, C_G, C_8, C_B, C_H, C_U, C_V,
  C_9, C_I, C_J, C_0, C_M, C_K, C_O, C_N,
  C_PLUS, C_P, C_L, C_MINUS, C_PERIOD, C_COLON, C_AT, C_COMMA,
  C_POUND, C_ASTERISK, C_SEMICOLON, C_HOME, C_RSHIFT, C_EQUAL, C_UPARROW, C_SLASH,
  C_1, C_LEFTARROW, C_CTRL, C_2, C_SPACE, C_CBM, C_Q, C_RUNSTOP,
};
static const char *const C64_NAMES[64] = {
  "DEL", "RETURN", "CRSR_LR", "F7", "F1", "F3", "F5", "CRSR_UD",
  "3", "W", "A", "4", "Z", "S", "E", "LSHIFT",
  "5", "R", "D", "6", "C", "F", "T", "X",
  "7", "Y", "G", "8", "B", "H", "U", "V",
  "9", "I", "J", "0", "M", "K", "O", "N",
  "PLUS", "P", "L", "MINUS", "PERIOD", "COLON", "AT", "COMMA",
  "POUND", "ASTERISK", "SEMICOLON", "HOME", "RSHIFT", "EQUAL", "UPARROW", "SLASH",
  "1", "LEFTARROW", "CTRL", "2", "SPACE", "CBM", "Q", "RUNSTOP",
};

#define KEEP(k) (uint8_t)(k)
#define SON(k)  (uint8_t)((C64K_SHIFT_ON  << 6) | (k))
#define SOFF(k) (uint8_t)((C64K_SHIFT_OFF << 6) | (k))

// HID usage codes (the standard USB HID keyboard page), numeric so every board's header works.
enum : uint8_t {
  H_A = 0x04, H_ENTER = 0x28, H_ESC, H_BACKSPACE, H_TAB, H_SPACE, H_MINUS, H_EQUAL,
  H_LBRACKET, H_RBRACKET, H_BACKSLASH, H_NONUS_HASH, H_SEMICOLON, H_APOSTROPHE, H_GRAVE,
  H_COMMA, H_PERIOD, H_SLASH, H_CAPSLOCK, H_F1, /* .. H_F1+11 = F12 */
  H_PRINTSCREEN = 0x46, H_SCROLLLOCK, H_PAUSE, H_INSERT, H_HOME, H_PAGE_UP, H_DELETE, H_END,
  H_PAGE_DOWN, H_RIGHT, H_LEFT, H_DOWN, H_UP, H_NUMLOCK, H_KP_DIVIDE, H_KP_MULTIPLY,
  H_KP_MINUS, H_KP_PLUS, H_KP_ENTER, H_KP_1, /* .. H_KP_1+8 = KP_9 */ H_KP_0 = 0x62, H_KP_PERIOD,
};
#define H_1 0x1E   // 1..9, then 0 at 0x27

struct KeyRule { uint8_t hid, shifted, code; };

static const KeyRule kDefault[] = {
  // letters
  {H_A+0,0,C_A}, {H_A+1,0,C_B}, {H_A+2,0,C_C}, {H_A+3,0,C_D}, {H_A+4,0,C_E}, {H_A+5,0,C_F},
  {H_A+6,0,C_G}, {H_A+7,0,C_H}, {H_A+8,0,C_I}, {H_A+9,0,C_J}, {H_A+10,0,C_K}, {H_A+11,0,C_L},
  {H_A+12,0,C_M}, {H_A+13,0,C_N}, {H_A+14,0,C_O}, {H_A+15,0,C_P}, {H_A+16,0,C_Q}, {H_A+17,0,C_R},
  {H_A+18,0,C_S}, {H_A+19,0,C_T}, {H_A+20,0,C_U}, {H_A+21,0,C_V}, {H_A+22,0,C_W}, {H_A+23,0,C_X},
  {H_A+24,0,C_Y}, {H_A+25,0,C_Z},
  // digits; Shift-1/3/4/5 ('!' '#' '$' '%') are the same on both, the rest are the keycap's symbol
  {H_1+0,0,C_1}, {H_1+1,0,C_2}, {H_1+2,0,C_3}, {H_1+3,0,C_4}, {H_1+4,0,C_5},
  {H_1+5,0,C_6}, {H_1+6,0,C_7}, {H_1+7,0,C_8}, {H_1+8,0,C_9}, {H_1+9,0,C_0},
  {H_1+1,1,SOFF(C_AT)},          // Shift-2 @
  {H_1+5,1,SOFF(C_UPARROW)},     // Shift-6 ^ -> up-arrow
  {H_1+6,1,SON(C_6)},            // Shift-7 &
  {H_1+7,1,SOFF(C_ASTERISK)},    // Shift-8 *
  {H_1+8,1,SON(C_8)},            // Shift-9 (
  {H_1+9,1,SON(C_9)},            // Shift-0 )
  // punctuation
  {H_MINUS,0,C_MINUS},       {H_MINUS,1,SOFF(C_LEFTARROW)},     // -  _ -> left-arrow
  {H_EQUAL,0,C_EQUAL},       {H_EQUAL,1,SOFF(C_PLUS)},          // =  +
  {H_LBRACKET,0,SON(C_COLON)},                                   // [  (C64: Shift-:)
  {H_RBRACKET,0,SON(C_SEMICOLON)},                               // ]  (C64: Shift-;)
  {H_BACKSLASH,0,C_POUND},                                       // \ -> pound
  {H_SEMICOLON,0,C_SEMICOLON}, {H_SEMICOLON,1,SOFF(C_COLON)},   // ;  :
  {H_APOSTROPHE,0,SON(C_7)}, {H_APOSTROPHE,1,SON(C_2)},         // '  "
  {H_GRAVE,0,C_LEFTARROW},                                       // ` -> left-arrow
  {H_COMMA,0,C_COMMA}, {H_PERIOD,0,C_PERIOD}, {H_SLASH,0,C_SLASH},   // , . /  (< > ? shifted)
  {H_SPACE,0,C_SPACE},
  // editing / control
  {H_ENTER,0,C_RETURN}, {H_KP_ENTER,0,C_RETURN},
  {H_BACKSPACE,0,C_DEL}, {H_DELETE,0,C_DEL},
  {H_INSERT,0,SON(C_DEL)},                                       // INST
  {H_HOME,0,C_HOME}, {H_END,0,SON(C_HOME)},                      // HOME, CLR
  {H_ESC,0,C_RUNSTOP}, {H_PAUSE,0,C_RUNSTOP},
  {H_PAGE_UP,0,C64K_RESTORE},
  {H_TAB,0,C_CTRL},                                              // where the C64's CTRL sits
  {C64K_HID_CTRL,0,C_CTRL}, {C64K_HID_ALT,0,C_CBM},
  // cursor: LEFT/UP are SHIFT + CRSR on the C64
  {H_RIGHT,0,C_CRSR_LR}, {H_LEFT,0,SON(C_CRSR_LR)}, {H_DOWN,0,C_CRSR_UD}, {H_UP,0,SON(C_CRSR_UD)},
  // function keys, number for number: F2/F4/F6/F8 are SHIFT + F1/F3/F5/F7. The PicoCalc's bare F3
  // arrives as F12 and its Shift-F1..F3 as F6..F8 (input_picocalc.cpp).
  {H_F1+0,0,SOFF(C_F1)}, {H_F1+1,0,SON(C_F1)},
#if UI_PICOCALC
  {H_F1+11,0,SOFF(C_F3)},
#else
  {H_F1+2,0,SOFF(C_F3)},
#endif
  {H_F1+3,0,SON(C_F3)}, {H_F1+4,0,SOFF(C_F5)}, {H_F1+5,0,SON(C_F5)},
  {H_F1+6,0,SOFF(C_F7)}, {H_F1+7,0,SON(C_F7)},
  // keypad
  {H_KP_1+0,0,C_1}, {H_KP_1+1,0,C_2}, {H_KP_1+2,0,C_3}, {H_KP_1+3,0,C_4}, {H_KP_1+4,0,C_5},
  {H_KP_1+5,0,C_6}, {H_KP_1+6,0,C_7}, {H_KP_1+7,0,C_8}, {H_KP_1+8,0,C_9}, {H_KP_0,0,C_0},
  {H_KP_PLUS,0,SOFF(C_PLUS)}, {H_KP_MINUS,0,SOFF(C_MINUS)}, {H_KP_MULTIPLY,0,SOFF(C_ASTERISK)},
  {H_KP_DIVIDE,0,SOFF(C_SLASH)}, {H_KP_PERIOD,0,SOFF(C_PERIOD)},
};

// Overrides from the SD card. Rules equal to the default are not stored, so a copy of the whole
// default file costs nothing; only real changes take a slot.
#define C64K_MAX_OVERRIDES 64
static KeyRule kOver[C64K_MAX_OVERRIDES];
static uint8_t kOverCount = 0;

static uint8_t findRule(const KeyRule *t, int n, uint8_t hid, uint8_t shifted)
{
  for (int i = 0; i < n; i++) if (t[i].hid == hid && t[i].shifted == shifted) return t[i].code;
  return C64K_NONE;
}
static uint8_t findDefault(uint8_t hid, uint8_t shifted)
{
  return findRule(kDefault, sizeof(kDefault) / sizeof(kDefault[0]), hid, shifted);
}

uint8_t c64KeymapLookup(uint8_t hid, bool shift)
{
  uint8_t c;
  if (shift) {
    if ((c = findRule(kOver, kOverCount, hid, 1)) != C64K_NONE) return c;
    if ((c = findDefault(hid, 1)) != C64K_NONE) return c;
  }
  if ((c = findRule(kOver, kOverCount, hid, 0)) != C64K_NONE) return c;
  return findDefault(hid, 0);
}

// ---- file parsing ---------------------------------------------------------------------------
static const struct { const char *name; uint8_t hid; } kHostNames[] = {
  {"ENTER", H_ENTER}, {"RETURN", H_ENTER}, {"ESC", H_ESC}, {"ESCAPE", H_ESC},
  {"BACKSPACE", H_BACKSPACE}, {"TAB", H_TAB}, {"SPACE", H_SPACE}, {"MINUS", H_MINUS},
  {"EQUAL", H_EQUAL}, {"LBRACKET", H_LBRACKET}, {"RBRACKET", H_RBRACKET},
  {"BACKSLASH", H_BACKSLASH}, {"NONUS_HASH", H_NONUS_HASH}, {"SEMICOLON", H_SEMICOLON},
  {"APOSTROPHE", H_APOSTROPHE}, {"GRAVE", H_GRAVE}, {"COMMA", H_COMMA}, {"PERIOD", H_PERIOD},
  {"SLASH", H_SLASH}, {"CAPSLOCK", H_CAPSLOCK}, {"PRINTSCREEN", H_PRINTSCREEN},
  {"SCROLLLOCK", H_SCROLLLOCK}, {"PAUSE", H_PAUSE}, {"BREAK", H_PAUSE}, {"INSERT", H_INSERT},
  {"HOME", H_HOME}, {"PAGE_UP", H_PAGE_UP}, {"DELETE", H_DELETE}, {"END", H_END},
  {"PAGE_DOWN", H_PAGE_DOWN}, {"RIGHT", H_RIGHT}, {"LEFT", H_LEFT}, {"DOWN", H_DOWN},
  {"UP", H_UP}, {"NUMLOCK", H_NUMLOCK}, {"KP_DIVIDE", H_KP_DIVIDE},
  {"KP_MULTIPLY", H_KP_MULTIPLY}, {"KP_MINUS", H_KP_MINUS}, {"KP_PLUS", H_KP_PLUS},
  {"KP_ENTER", H_KP_ENTER}, {"KP_PERIOD", H_KP_PERIOD},
  {"CTRL", C64K_HID_CTRL}, {"ALT", C64K_HID_ALT},
};

// Host key name -> HID usage code; 0 = unknown. Also A..Z, 0..9, F1..F12, KP_0..KP_9, and 0xNN.
static uint8_t hostKey(const char *s)
{
  if (s[0] && !s[1]) {
    if (s[0] >= 'A' && s[0] <= 'Z') return H_A + (s[0] - 'A');
    if (s[0] >= '1' && s[0] <= '9') return H_1 + (s[0] - '1');
    if (s[0] == '0') return H_1 + 9;
  }
  if (s[0] == 'F' && s[1] >= '1' && s[1] <= '9') {
    int n = atoi(s + 1);
    if (n >= 1 && n <= 12 && (s[2] == 0 || (s[3] == 0 && n >= 10))) return H_F1 + n - 1;
  }
  if (!strncmp(s, "KP_", 3) && s[3] >= '0' && s[3] <= '9' && !s[4])
    return s[3] == '0' ? H_KP_0 : H_KP_1 + (s[3] - '1');
  if (s[0] == '0' && s[1] == 'X') {
    long v = strtol(s + 2, nullptr, 16);
    return (v > 0 && v < 256) ? (uint8_t)v : 0;
  }
  for (auto &e : kHostNames) if (!strcmp(s, e.name)) return e.hid;
  return 0;
}

uint8_t c64KeymapHostParse(const char *s) { return hostKey(s); }

// C64 key name -> matrix index, C64K_UNMAP / C64K_RESTORE, or C64K_NONE when unknown.
static uint8_t c64Key(const char *s)
{
  if (!strcmp(s, "NONE"))    return C64K_UNMAP;
  if (!strcmp(s, "RESTORE")) return C64K_RESTORE;
  static const struct { const char *sym; uint8_t k; } kSym[] = {
    {"+", C_PLUS}, {"-", C_MINUS}, {"*", C_ASTERISK}, {"@", C_AT}, {":", C_COLON},
    {";", C_SEMICOLON}, {"=", C_EQUAL}, {",", C_COMMA}, {".", C_PERIOD}, {"/", C_SLASH},
    {"SHIFT", C_LSHIFT}, {"COMMODORE", C_CBM}, {"RUN_STOP", C_RUNSTOP}, {"CLR_HOME", C_HOME},
    {"INST_DEL", C_DEL},
  };
  for (auto &e : kSym) if (!strcmp(s, e.sym)) return e.k;
  for (int i = 0; i < 64; i++) if (!strcmp(s, C64_NAMES[i])) return (uint8_t)i;
  return C64K_NONE;
}

static void keymapError(int line, const char *what, const char *tok)
{
  snprintf(buf, sizeof(buf), "C64 keymap line %d: %s '%s' - ignored", line, what, tok);
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
  if (n < 3 || n > 4 || strcmp(tok[1], "=")) { keymapError(line, "expected 'KEY = C64KEY', got", tok[0]); return; }

  const char *src = tok[0];
  uint8_t shifted = 0;
  if (!strncmp(src, "SHIFT+", 6)) { shifted = 1; src += 6; }
  uint8_t hid = hostKey(src);
  if (!hid) { keymapError(line, "unknown host key", tok[0]); return; }

  uint8_t code = c64Key(tok[2]);
  if (code == C64K_NONE) { keymapError(line, "unknown C64 key", tok[2]); return; }
  if (c64kIsMatrix(code)) {
    int mode = shifted ? C64K_SHIFT_OFF : C64K_SHIFT_KEEP;
    if (n == 4) {
      if (!strcmp(tok[3], "+SHIFT"))      mode = C64K_SHIFT_ON;
      else if (!strcmp(tok[3], "-SHIFT")) mode = C64K_SHIFT_OFF;
      else { keymapError(line, "expected +SHIFT or -SHIFT, got", tok[3]); return; }
    }
    code = (uint8_t)((mode << 6) | code);
  }

  if (findDefault(hid, shifted) == code) return;   // same as the built-in rule
  for (int i = 0; i < kOverCount; i++)              // a later line replaces an earlier one
    if (kOver[i].hid == hid && kOver[i].shifted == shifted) { kOver[i].code = code; return; }
  if (kOverCount >= C64K_MAX_OVERRIDES) { keymapError(line, "too many changed keys, at", tok[0]); return; }
  kOver[kOverCount++] = {hid, shifted, code};
}

void c64KeymapLoad()
{
  kOverCount = 0;
  static const char *const kPaths[] = { "/c64/keymap.txt", "/c64keymap.txt" };
  for (const char *path : kPaths) {
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("C64 keymap: out of heap opening the file"); return; }
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
    snprintf(buf, sizeof(buf), "C64 keymap: %s, %d key(s) changed from the default", path, kOverCount);
    printLog(buf);
    return;
  }
}

// ---- the KEYBOARD page (optionsui.cpp) ------------------------------------------------------
// Host keys in the page's order: letters, digits, the editing / cursor keys, punctuation, the
// function keys, the modifiers, the rest, the keypad.
static const uint8_t kUiMid[] = {
  H_SPACE, H_ENTER, H_ESC, H_BACKSPACE, H_TAB, H_DELETE, H_INSERT, H_HOME, H_END, H_PAGE_UP,
  H_PAGE_DOWN, H_UP, H_DOWN, H_LEFT, H_RIGHT, H_PAUSE,
  H_MINUS, H_EQUAL, H_LBRACKET, H_RBRACKET, H_BACKSLASH, H_SEMICOLON, H_APOSTROPHE, H_GRAVE,
  H_COMMA, H_PERIOD, H_SLASH, H_NONUS_HASH,
};
static const uint8_t kUiEnd[] = {
  C64K_HID_CTRL, C64K_HID_ALT, H_CAPSLOCK, H_PRINTSCREEN, H_SCROLLLOCK, H_NUMLOCK,
  H_KP_1, H_KP_1+1, H_KP_1+2, H_KP_1+3, H_KP_1+4, H_KP_1+5, H_KP_1+6, H_KP_1+7, H_KP_1+8, H_KP_0,
  H_KP_PLUS, H_KP_MINUS, H_KP_MULTIPLY, H_KP_DIVIDE, H_KP_PERIOD, H_KP_ENTER,
};
#define UI_MID_N ((int)sizeof(kUiMid))
#define UI_END_N ((int)sizeof(kUiEnd))

int c64KeymapHostCount() { return 26 + 10 + UI_MID_N + 12 + UI_END_N; }

uint8_t c64KeymapHostAt(int i)
{
  if (i < 26) return H_A + i;
  if ((i -= 26) < 10) return H_1 + i;
  if ((i -= 10) < UI_MID_N) return kUiMid[i];
  if ((i -= UI_MID_N) < 12) return H_F1 + i;
  if ((i -= 12) < UI_END_N) return kUiEnd[i];
  return 0;
}

void c64KeymapHostName(uint8_t hid, char *out, size_t n)
{
  if (hid >= H_A && hid < H_A + 26)          snprintf(out, n, "%c", 'A' + (hid - H_A));
  else if (hid >= H_1 && hid < H_1 + 10)     snprintf(out, n, "%c", hid == H_1 + 9 ? '0' : '1' + (hid - H_1));
  else if (hid >= H_F1 && hid < H_F1 + 12)   snprintf(out, n, "F%d", hid - H_F1 + 1);
  else if (hid >= H_KP_1 && hid < H_KP_1 + 9) snprintf(out, n, "KP_%d", hid - H_KP_1 + 1);
  else if (hid == H_KP_0)                    snprintf(out, n, "KP_0");
  else {
    for (auto &e : kHostNames) if (e.hid == hid) { snprintf(out, n, "%s", e.name); return; }   // first = canonical
    snprintf(out, n, "0x%02X", hid);
  }
}

const char *c64KeymapC64Name(uint8_t index) { return C64_NAMES[index & 63]; }

uint8_t c64KeymapC64At(int i)
{
  static const uint8_t kPick[64] = {
    C_A, C_B, C_C, C_D, C_E, C_F, C_G, C_H, C_I, C_J, C_K, C_L, C_M, C_N, C_O, C_P, C_Q, C_R,
    C_S, C_T, C_U, C_V, C_W, C_X, C_Y, C_Z,
    C_1, C_2, C_3, C_4, C_5, C_6, C_7, C_8, C_9, C_0,
    C_F1, C_F3, C_F5, C_F7, C_RETURN, C_SPACE, C_DEL, C_HOME, C_RUNSTOP, C_CRSR_LR, C_CRSR_UD,
    C_CTRL, C_CBM, C_LSHIFT, C_RSHIFT,
    C_PLUS, C_MINUS, C_ASTERISK, C_SLASH, C_EQUAL, C_AT, C_COLON, C_SEMICOLON, C_COMMA, C_PERIOD,
    C_POUND, C_UPARROW, C_LEFTARROW,
  };
  return kPick[i & 63];
}

void c64KeymapDescribe(uint8_t code, bool shifted, char *out, size_t n)
{
  if (code == C64K_NONE)    { snprintf(out, n, "-");       return; }
  if (code == C64K_UNMAP)   { snprintf(out, n, "NONE");    return; }
  if (code == C64K_RESTORE) { snprintf(out, n, "RESTORE"); return; }
  const int mode = c64kShift(code);
  const char *sfx = mode == C64K_SHIFT_ON ? " +SHIFT" : mode == C64K_SHIFT_OFF ? " -SHIFT" : "";
  (void)shifted;   // a SHIFT+ rule is never KEEP (c64KeymapSet), so its suffix is always written
  snprintf(out, n, "%s%s", C64_NAMES[c64kIndex(code)], sfx);
}

uint8_t c64KeymapDefault(uint8_t hid, bool shifted) { return findDefault(hid, shifted ? 1 : 0); }

uint8_t c64KeymapRule(uint8_t hid, bool shifted, bool *over)
{
  uint8_t c = findRule(kOver, kOverCount, hid, shifted ? 1 : 0);
  if (over) *over = c != C64K_NONE;
  return c != C64K_NONE ? c : c64KeymapDefault(hid, shifted);
}

bool c64KeymapSet(uint8_t hid, bool shifted, uint8_t code)
{
  const uint8_t sh = shifted ? 1 : 0;
  // A SHIFT+ rule has no "as typed": SHIFT is down whenever it applies, so that is +SHIFT. Keeps
  // the file unambiguous too (a SHIFT+ line without a suffix reads as -SHIFT).
  if (sh && c64kIsMatrix(code) && c64kShift(code) == C64K_SHIFT_KEEP) code = (uint8_t)((C64K_SHIFT_ON << 6) | code);
  int at = -1;
  for (int i = 0; i < kOverCount; i++) if (kOver[i].hid == hid && kOver[i].shifted == sh) at = i;
  if (code == C64K_NONE || code == findDefault(hid, sh)) {          // back to the default
    if (at >= 0) kOver[at] = kOver[--kOverCount];
    return true;
  }
  if (at >= 0) { kOver[at].code = code; return true; }
  if (kOverCount >= C64K_MAX_OVERRIDES) return false;
  kOver[kOverCount++] = {hid, sh, code};
  return true;
}

void c64KeymapResetAll()   { kOverCount = 0; }
int  c64KeymapChangedCount() { return kOverCount; }

bool c64KeymapSave()
{
  static const char *const kPath = "/c64/keymap.txt";
  // Timed lock, not busTake(): this runs in the render task (the menu).
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  bool ok = false;
  try {
    if (!FSTYPE.exists("/c64")) FSTYPE.mkdir("/c64");
    if (FSTYPE.exists(kPath)) FSTYPE.remove(kPath);     // FILE_WRITE appends on arduino-pico
    File f = FSTYPE.open(kPath, FILE_WRITE);
    if (f) {
      static const char kHead[] =
        "# emu8 C64 keyboard map, saved from the C64 menu (CONFIG > KEYBOARD), which rewrites it.\n"
        "# Only the keys changed from the built-in default are listed; the syntax and the whole\n"
        "# default map are in docs/c64-keymap.txt.\n";
      ok = f.write((const uint8_t *)kHead, sizeof(kHead) - 1) == sizeof(kHead) - 1;
      char line[64], name[16], rule[24];
      for (int i = 0; ok && i < kOverCount; i++) {
        c64KeymapHostName(kOver[i].hid, name, sizeof(name));
        c64KeymapDescribe(kOver[i].code, kOver[i].shifted, rule, sizeof(rule));
        int len = snprintf(line, sizeof(line), "%s%s = %s\n", kOver[i].shifted ? "SHIFT+" : "", name, rule);
        ok = f.write((const uint8_t *)line, len) == (size_t)len;
      }
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "C64 keymap: %s %s, %d key(s) changed", kPath, ok ? "saved" : "NOT saved", kOverCount);
  printLog(buf);
  return ok;
}

#endif // BOARD_INPUT_USB
