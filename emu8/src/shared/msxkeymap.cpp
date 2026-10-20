// msxkeymap.cpp - which MSX key each host key presses (USB keyboard, PicoCalc keyboard, the
// desktop build's PC keyboard: everything that reaches usbKeyboardReport()).
//
// A built-in default map, plus overrides read from a text file on the SD card when the MSX boots:
// /emu8/msx/keymap.txt (or /msxkeymap.txt at the root). One rule per line, '#' starts a comment:
//
//     <host key> = <MSX key> [+SHIFT | -SHIFT]
//
//     PAGE_DOWN = STOP             # PgDn is STOP too
//     SHIFT+2 = 2 -SHIFT           # Shift-2 types '2'
//     F9 = NONE                    # F9 does nothing
//
// A SHIFT+ rule is used when the key is typed with SHIFT and wins over the plain rule; it releases
// the MSX's SHIFT unless +SHIFT says otherwise. A plain rule passes SHIFT through as typed.
// +SHIFT / -SHIFT hold or release it whatever the host keyboard says. The full default map, with
// every name, is in docs/msx-keymap.txt.
//
// The default: the MSX international layout is the PC's US layout, so every character key, shifted
// or not, sits on the key with the same keycap; F6-F10 are SHIFT + F1-F5 as on the MSX (F6..F9 on
// F6..F9, F10 opens the menu so SHIFT+F5 is F11 too); CLS = SHIFT+HOME on End, INS / DEL / HOME on
// their own keys, STOP = Pause, SELECT = Page Up, CODE = Page Down, GRAPH = Alt, the dead key = the
// ISO key next to the left Shift.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "msxkeymap.h"

// MSX keys by matrix index (row*8 + bit, msx_ppi.cpp). The enum gives the default table readable names.
enum : uint8_t {
  MK_0, MK_1, MK_2, MK_3, MK_4, MK_5, MK_6, MK_7,
  MK_8, MK_9, MK_MINUS, MK_EQUAL, MK_BACKSLASH, MK_LBRACKET, MK_RBRACKET, MK_SEMICOLON,
  MK_APOSTROPHE, MK_GRAVE, MK_COMMA, MK_PERIOD, MK_SLASH, MK_DEAD, MK_A, MK_B,
  MK_C, MK_D, MK_E, MK_F, MK_G, MK_H, MK_I, MK_J,
  MK_K, MK_L, MK_M, MK_N, MK_O, MK_P, MK_Q, MK_R,
  MK_S, MK_T, MK_U, MK_V, MK_W, MK_X, MK_Y, MK_Z,
  MK_SHIFT, MK_CTRL, MK_GRAPH, MK_CAPS, MK_CODE, MK_F1, MK_F2, MK_F3,
  MK_F4, MK_F5, MK_ESC, MK_TAB, MK_STOP, MK_BS, MK_SELECT, MK_RETURN,
  MK_SPACE, MK_HOME, MK_INS, MK_DEL, MK_LEFT, MK_UP, MK_DOWN, MK_RIGHT,
};
static const char *const MSX_NAMES[MSXK_KEYS] = {
  "0", "1", "2", "3", "4", "5", "6", "7",
  "8", "9", "MINUS", "EQUAL", "BACKSLASH", "LBRACKET", "RBRACKET", "SEMICOLON",
  "APOSTROPHE", "GRAVE", "COMMA", "PERIOD", "SLASH", "DEAD", "A", "B",
  "C", "D", "E", "F", "G", "H", "I", "J",
  "K", "L", "M", "N", "O", "P", "Q", "R",
  "S", "T", "U", "V", "W", "X", "Y", "Z",
  "SHIFT", "CTRL", "GRAPH", "CAPS", "CODE", "F1", "F2", "F3",
  "F4", "F5", "ESC", "TAB", "STOP", "BS", "SELECT", "RETURN",
  "SPACE", "HOME", "INS", "DEL", "LEFT", "UP", "DOWN", "RIGHT",
};

#define KEEP(k) msxkMake(k, MSXK_SHIFT_KEEP)
#define SON(k)  msxkMake(k, MSXK_SHIFT_ON)
#define SOFF(k) msxkMake(k, MSXK_SHIFT_OFF)

// HID usage codes (the standard USB HID keyboard page), numeric so every board's header works.
enum : uint8_t {
  H_A = 0x04, H_ENTER = 0x28, H_ESC, H_BACKSPACE, H_TAB, H_SPACE, H_MINUS, H_EQUAL,
  H_LBRACKET, H_RBRACKET, H_BACKSLASH, H_NONUS_HASH, H_SEMICOLON, H_APOSTROPHE, H_GRAVE,
  H_COMMA, H_PERIOD, H_SLASH, H_CAPSLOCK, H_F1, /* .. H_F1+11 = F12 */
  H_PRINTSCREEN = 0x46, H_SCROLLLOCK, H_PAUSE, H_INSERT, H_HOME, H_PAGE_UP, H_DELETE, H_END,
  H_PAGE_DOWN, H_RIGHT, H_LEFT, H_DOWN, H_UP, H_NUMLOCK, H_KP_DIVIDE, H_KP_MULTIPLY,
  H_KP_MINUS, H_KP_PLUS, H_KP_ENTER, H_KP_1, /* .. H_KP_1+8 = KP_9 */ H_KP_0 = 0x62, H_KP_PERIOD,
  H_NONUS_BACKSLASH = 0x64,
};
#define H_1 0x1E   // 1..9, then 0 at 0x27

struct KeyRule { uint8_t hid, shifted, code; };

static const KeyRule kDefault[] = {
  // letters
  {H_A+0,0,MK_A}, {H_A+1,0,MK_B}, {H_A+2,0,MK_C}, {H_A+3,0,MK_D}, {H_A+4,0,MK_E}, {H_A+5,0,MK_F},
  {H_A+6,0,MK_G}, {H_A+7,0,MK_H}, {H_A+8,0,MK_I}, {H_A+9,0,MK_J}, {H_A+10,0,MK_K}, {H_A+11,0,MK_L},
  {H_A+12,0,MK_M}, {H_A+13,0,MK_N}, {H_A+14,0,MK_O}, {H_A+15,0,MK_P}, {H_A+16,0,MK_Q}, {H_A+17,0,MK_R},
  {H_A+18,0,MK_S}, {H_A+19,0,MK_T}, {H_A+20,0,MK_U}, {H_A+21,0,MK_V}, {H_A+22,0,MK_W}, {H_A+23,0,MK_X},
  {H_A+24,0,MK_Y}, {H_A+25,0,MK_Z},
  // digits: the shifted symbols are the PC's (! @ # $ % ^ & * ( ))
  {H_1+0,0,MK_1}, {H_1+1,0,MK_2}, {H_1+2,0,MK_3}, {H_1+3,0,MK_4}, {H_1+4,0,MK_5},
  {H_1+5,0,MK_6}, {H_1+6,0,MK_7}, {H_1+7,0,MK_8}, {H_1+8,0,MK_9}, {H_1+9,0,MK_0},
  // punctuation, shifted ones included: - _  = +  \ |  [ {  ] }  ; :  ' "  ` ~  , <  . >  / ?
  {H_MINUS,0,MK_MINUS}, {H_EQUAL,0,MK_EQUAL}, {H_BACKSLASH,0,MK_BACKSLASH},
  {H_LBRACKET,0,MK_LBRACKET}, {H_RBRACKET,0,MK_RBRACKET}, {H_SEMICOLON,0,MK_SEMICOLON},
  {H_APOSTROPHE,0,MK_APOSTROPHE}, {H_GRAVE,0,MK_GRAVE}, {H_COMMA,0,MK_COMMA},
  {H_PERIOD,0,MK_PERIOD}, {H_SLASH,0,MK_SLASH},
  {H_NONUS_HASH,0,MK_DEAD}, {H_NONUS_BACKSLASH,0,MK_DEAD},      // the ISO keys: the dead key
  {H_SPACE,0,MK_SPACE},
  // editing / control
  {H_ENTER,0,MK_RETURN}, {H_KP_ENTER,0,MK_RETURN},
  {H_BACKSPACE,0,MK_BS}, {H_DELETE,0,MK_DEL}, {H_INSERT,0,MK_INS},
  {H_HOME,0,MK_HOME}, {H_END,0,SON(MK_HOME)},                      // HOME, CLS
  {H_ESC,0,MK_ESC}, {H_TAB,0,MK_TAB},
  {H_PAUSE,0,MK_STOP}, {H_PAGE_UP,0,MK_SELECT}, {H_PAGE_DOWN,0,MK_CODE},
  {H_CAPSLOCK,0,MK_CAPS},
  {C64K_HID_CTRL,0,MK_CTRL}, {C64K_HID_ALT,0,MK_GRAPH},
  {H_RIGHT,0,MK_RIGHT}, {H_LEFT,0,MK_LEFT}, {H_DOWN,0,MK_DOWN}, {H_UP,0,MK_UP},
  // function keys: F6-F10 are SHIFT + F1-F5 on the MSX. The PicoCalc's bare F3 arrives as F12 and
  // its Shift-F1..F4 as F6..F9 (input_picocalc.cpp); F10 is the menu everywhere, so F11 = SHIFT+F5.
  {H_F1+0,0,MK_F1}, {H_F1+1,0,MK_F2},
#if UI_PICOCALC
  {H_F1+11,0,MK_F3},
#else
  {H_F1+2,0,MK_F3},
#endif
  {H_F1+3,0,MK_F4}, {H_F1+4,0,MK_F5},
  {H_F1+5,0,SON(MK_F1)}, {H_F1+6,0,SON(MK_F2)}, {H_F1+7,0,SON(MK_F3)}, {H_F1+8,0,SON(MK_F4)},
  {H_F1+10,0,SON(MK_F5)},
  // keypad: the main-row characters (the MSX1 has no keypad on most models)
  {H_KP_1+0,0,SOFF(MK_1)}, {H_KP_1+1,0,SOFF(MK_2)}, {H_KP_1+2,0,SOFF(MK_3)}, {H_KP_1+3,0,SOFF(MK_4)},
  {H_KP_1+4,0,SOFF(MK_5)}, {H_KP_1+5,0,SOFF(MK_6)}, {H_KP_1+6,0,SOFF(MK_7)}, {H_KP_1+7,0,SOFF(MK_8)},
  {H_KP_1+8,0,SOFF(MK_9)}, {H_KP_0,0,SOFF(MK_0)},
  {H_KP_PLUS,0,SON(MK_EQUAL)}, {H_KP_MINUS,0,SOFF(MK_MINUS)}, {H_KP_MULTIPLY,0,SON(MK_8)},
  {H_KP_DIVIDE,0,SOFF(MK_SLASH)}, {H_KP_PERIOD,0,SOFF(MK_PERIOD)},
};

// Overrides from the SD card. Rules equal to the default are not stored, so a copy of the whole
// default file costs nothing; only real changes take a slot.
#define MSXK_MAX_OVERRIDES 64
static KeyRule kOver[MSXK_MAX_OVERRIDES];
static uint8_t kOverCount = 0;

static uint8_t findRule(const KeyRule *t, int n, uint8_t hid, uint8_t shifted)
{
  for (int i = 0; i < n; i++) if (t[i].hid == hid && t[i].shifted == shifted) return t[i].code;
  return MSXK_NONE;
}
static uint8_t findDefault(uint8_t hid, uint8_t shifted)
{
  return findRule(kDefault, sizeof(kDefault) / sizeof(kDefault[0]), hid, shifted);
}

uint8_t msxKeymapLookup(uint8_t hid, bool shift)
{
  uint8_t c;
  if (shift) {
    if ((c = findRule(kOver, kOverCount, hid, 1)) != MSXK_NONE) return c;
    if ((c = findDefault(hid, 1)) != MSXK_NONE) return c;
  }
  if ((c = findRule(kOver, kOverCount, hid, 0)) != MSXK_NONE) return c;
  return findDefault(hid, 0);
}

// ---- file parsing ---------------------------------------------------------------------------
// MSX key name -> matrix index, MSXK_UNMAP for NONE, MSXK_NONE when unknown.
static uint8_t msxKey(const char *s)
{
  if (!strcmp(s, "NONE")) return MSXK_UNMAP;
  static const struct { const char *sym; uint8_t k; } kSym[] = {
    {"-", MK_MINUS}, {"=", MK_EQUAL}, {"\\", MK_BACKSLASH}, {"[", MK_LBRACKET}, {"]", MK_RBRACKET},
    {";", MK_SEMICOLON}, {"'", MK_APOSTROPHE}, {"`", MK_GRAVE}, {",", MK_COMMA}, {".", MK_PERIOD},
    {"/", MK_SLASH}, {"GRPH", MK_GRAPH}, {"ENTER", MK_RETURN}, {"BACKSPACE", MK_BS},
    {"INSERT", MK_INS}, {"DELETE", MK_DEL}, {"ESCAPE", MK_ESC}, {"CAPSLOCK", MK_CAPS},
    {"CLR_HOME", MK_HOME}, {"KANA", MK_CODE},
  };
  for (auto &e : kSym) if (!strcmp(s, e.sym)) return e.k;
  for (int i = 0; i < MSXK_KEYS; i++) if (!strcmp(s, MSX_NAMES[i])) return (uint8_t)i;
  return MSXK_NONE;
}

static void keymapError(int line, const char *what, const char *tok)
{
  snprintf(buf, sizeof(buf), "MSX keymap line %d: %s '%s' - ignored", line, what, tok);
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
  if (n < 3 || n > 4 || strcmp(tok[1], "=")) { keymapError(line, "expected 'KEY = MSXKEY', got", tok[0]); return; }

  const char *src = tok[0];
  uint8_t shifted = 0;
  if (!strncmp(src, "SHIFT+", 6)) { shifted = 1; src += 6; }
  uint8_t hid = c64KeymapHostParse(src);
  if (!hid) { keymapError(line, "unknown host key", tok[0]); return; }

  uint8_t code = msxKey(tok[2]);
  if (code == MSXK_NONE) { keymapError(line, "unknown MSX key", tok[2]); return; }
  if (code != MSXK_UNMAP) {
    int mode = shifted ? MSXK_SHIFT_OFF : MSXK_SHIFT_KEEP;
    if (n == 4) {
      if (!strcmp(tok[3], "+SHIFT"))      mode = MSXK_SHIFT_ON;
      else if (!strcmp(tok[3], "-SHIFT")) mode = MSXK_SHIFT_OFF;
      else { keymapError(line, "expected +SHIFT or -SHIFT, got", tok[3]); return; }
    }
    code = msxkMake(code, mode);
  }

  if (findDefault(hid, shifted) == code) return;   // same as the built-in rule
  for (int i = 0; i < kOverCount; i++)              // a later line replaces an earlier one
    if (kOver[i].hid == hid && kOver[i].shifted == shifted) { kOver[i].code = code; return; }
  if (kOverCount >= MSXK_MAX_OVERRIDES) { keymapError(line, "too many changed keys, at", tok[0]); return; }
  kOver[kOverCount++] = {hid, shifted, code};
}

void msxKeymapLoad()
{
  kOverCount = 0;
  static const char *const kPaths[] = { "/emu8/msx/keymap.txt", "/msx/keymap.txt", "/msxkeymap.txt" };
  for (const char *path : kPaths) {
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("MSX keymap: out of heap opening the file"); return; }
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
    snprintf(buf, sizeof(buf), "MSX keymap: %s, %d key(s) changed from the default", path, kOverCount);
    printLog(buf);
    return;
  }
}

// ---- the KEYBOARD page (optionsui.cpp) ------------------------------------------------------
const char *msxKeymapMsxName(uint8_t index) { return MSX_NAMES[index % MSXK_KEYS]; }

uint8_t msxKeymapMsxAt(int i)
{
  static const uint8_t kPick[MSXK_KEYS] = {
    MK_A, MK_B, MK_C, MK_D, MK_E, MK_F, MK_G, MK_H, MK_I, MK_J, MK_K, MK_L, MK_M, MK_N, MK_O, MK_P, MK_Q, MK_R,
    MK_S, MK_T, MK_U, MK_V, MK_W, MK_X, MK_Y, MK_Z,
    MK_1, MK_2, MK_3, MK_4, MK_5, MK_6, MK_7, MK_8, MK_9, MK_0,
    MK_F1, MK_F2, MK_F3, MK_F4, MK_F5, MK_RETURN, MK_SPACE, MK_BS, MK_DEL, MK_INS, MK_HOME, MK_ESC, MK_TAB,
    MK_STOP, MK_SELECT, MK_LEFT, MK_UP, MK_DOWN, MK_RIGHT,
    MK_CTRL, MK_SHIFT, MK_GRAPH, MK_CODE, MK_CAPS,
    MK_MINUS, MK_EQUAL, MK_BACKSLASH, MK_LBRACKET, MK_RBRACKET, MK_SEMICOLON, MK_APOSTROPHE, MK_GRAVE,
    MK_COMMA, MK_PERIOD, MK_SLASH, MK_DEAD,
  };
  return kPick[i % MSXK_KEYS];
}

void msxKeymapDescribe(uint8_t code, bool shifted, char *out, size_t n)
{
  if (code == MSXK_NONE)  { snprintf(out, n, "-");    return; }
  if (code == MSXK_UNMAP) { snprintf(out, n, "NONE"); return; }
  const int mode = msxkShift(code);
  const char *sfx = mode == MSXK_SHIFT_ON ? " +SHIFT" : mode == MSXK_SHIFT_OFF ? " -SHIFT" : "";
  (void)shifted;   // a SHIFT+ rule is never KEEP (msxKeymapSet), so its suffix is always written
  snprintf(out, n, "%s%s", MSX_NAMES[msxkIndex(code)], sfx);
}

uint8_t msxKeymapDefault(uint8_t hid, bool shifted) { return findDefault(hid, shifted ? 1 : 0); }

uint8_t msxKeymapRule(uint8_t hid, bool shifted, bool *over)
{
  uint8_t c = findRule(kOver, kOverCount, hid, shifted ? 1 : 0);
  if (over) *over = c != MSXK_NONE;
  return c != MSXK_NONE ? c : msxKeymapDefault(hid, shifted);
}

bool msxKeymapSet(uint8_t hid, bool shifted, uint8_t code)
{
  const uint8_t sh = shifted ? 1 : 0;
  // A SHIFT+ rule has no "as typed": SHIFT is down whenever it applies, so that is +SHIFT.
  if (sh && msxkIsKey(code) && msxkShift(code) == MSXK_SHIFT_KEEP) code = msxkMake(msxkIndex(code), MSXK_SHIFT_ON);
  int at = -1;
  for (int i = 0; i < kOverCount; i++) if (kOver[i].hid == hid && kOver[i].shifted == sh) at = i;
  if (code == MSXK_NONE || code == findDefault(hid, sh)) {          // back to the default
    if (at >= 0) kOver[at] = kOver[--kOverCount];
    return true;
  }
  if (at >= 0) { kOver[at].code = code; return true; }
  if (kOverCount >= MSXK_MAX_OVERRIDES) return false;
  kOver[kOverCount++] = {hid, sh, code};
  return true;
}

void msxKeymapResetAll()     { kOverCount = 0; }
int  msxKeymapChangedCount() { return kOverCount; }

bool msxKeymapSave()
{
  static const char *const kPath = "/emu8/msx/keymap.txt";
  // Timed lock, not busTake(): this runs in the render task (the menu).
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  bool ok = false;
  try {
    if (!FSTYPE.exists("/emu8")) FSTYPE.mkdir("/emu8");
    if (!FSTYPE.exists("/emu8/msx")) FSTYPE.mkdir("/emu8/msx");
    if (FSTYPE.exists(kPath)) FSTYPE.remove(kPath);     // FILE_WRITE appends on arduino-pico
    File f = FSTYPE.open(kPath, FILE_WRITE);
    if (f) {
      static const char kHead[] =
        "# emu8 MSX keyboard map, saved from the MSX menu (CONFIG > KEYBOARD), which rewrites it.\n"
        "# Only the keys changed from the built-in default are listed; the syntax and the whole\n"
        "# default map are in docs/msx-keymap.txt.\n";
      ok = f.write((const uint8_t *)kHead, sizeof(kHead) - 1) == sizeof(kHead) - 1;
      char line[64], name[16], rule[24];
      for (int i = 0; ok && i < kOverCount; i++) {
        c64KeymapHostName(kOver[i].hid, name, sizeof(name));
        msxKeymapDescribe(kOver[i].code, kOver[i].shifted, rule, sizeof(rule));
        int len = snprintf(line, sizeof(line), "%s%s = %s\n", kOver[i].shifted ? "SHIFT+" : "", name, rule);
        ok = f.write((const uint8_t *)line, len) == (size_t)len;
      }
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "MSX keymap: %s %s, %d key(s) changed", kPath, ok ? "saved" : "NOT saved", kOverCount);
  printLog(buf);
  return ok;
}

#endif // BOARD_INPUT_USB
