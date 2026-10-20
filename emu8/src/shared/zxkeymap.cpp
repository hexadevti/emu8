// zxkeymap.cpp - which ZX Spectrum key each host key presses (USB keyboard, PicoCalc keyboard, the
// desktop build's PC keyboard: everything that reaches usbKeyboardReport()).
//
// A built-in default map, plus overrides read from a text file on the SD card when the Spectrum
// boots: /emu8/zx/keymap.txt (or /zxkeymap.txt at the root). One rule per line, '#' starts a comment:
//
//     <host key> = <Spectrum key> [+CAPS | -CAPS | +SYMBOL]
//
//     PAGE_DOWN = SPACE +CAPS      # PgDn is BREAK too
//     SHIFT+2 = P +SYMBOL          # Shift-2 types '"' instead of CAPS LOCK
//     F9 = NONE                    # F9 does nothing
//
// The Spectrum has 40 keys and gets the rest from chords with CAPS SHIFT and SYMBOL SHIFT, so a
// rule names a key and what it does to the two shifts: nothing (CAPS SHIFT as the host Shift has
// it), +CAPS (CAPS SHIFT held: DELETE is 0 +CAPS), -CAPS (CAPS SHIFT released), +SYMBOL (SYMBOL
// SHIFT held, CAPS SHIFT released: ',' is N +SYMBOL). A SHIFT+ rule is used when the key is typed
// with Shift and wins over the plain rule; it releases CAPS SHIFT unless +CAPS says otherwise. The
// host keys are named as in the C64 map (docs/c64-keymap.txt); the full default is in
// docs/zx-keymap.txt.
//
// The default: Shift = CAPS SHIFT, Ctrl / Alt = SYMBOL SHIFT, letters and digits where they sit,
// Backspace / Delete = DELETE, the arrows = the cursor keys, Esc = BREAK, Tab = EXTENDED MODE, and
// PC punctuation types the Spectrum symbol printed on the keycap.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "zxkeymap.h"
#include "c64keymap.h"

// Spectrum keys by matrix index (half-row*5 + bit; bit 0 = the key nearest the keyboard's edge).
enum : uint8_t {
  Z_CAPS, Z_Z, Z_X, Z_C, Z_V,
  Z_A, Z_S, Z_D, Z_F, Z_G,
  Z_Q, Z_W, Z_E, Z_R, Z_T,
  Z_1, Z_2, Z_3, Z_4, Z_5,
  Z_0, Z_9, Z_8, Z_7, Z_6,
  Z_P, Z_O, Z_I, Z_U, Z_Y,
  Z_ENTER, Z_L, Z_K, Z_J, Z_H,
  Z_SPACE, Z_SYM, Z_M, Z_N, Z_B,
};
static const char *const ZX_NAMES[ZXK_KEYS] = {
  "CAPS", "Z", "X", "C", "V",
  "A", "S", "D", "F", "G",
  "Q", "W", "E", "R", "T",
  "1", "2", "3", "4", "5",
  "0", "9", "8", "7", "6",
  "P", "O", "I", "U", "Y",
  "ENTER", "L", "K", "J", "H",
  "SPACE", "SYMBOL", "M", "N", "B",
};

#define KEEP(k)  (uint8_t)(k)
#define CAPS(k)  (uint8_t)((ZXK_CAPS  << 6) | (k))
#define PLAIN(k) (uint8_t)((ZXK_PLAIN << 6) | (k))
#define SYM(k)   (uint8_t)((ZXK_SYM   << 6) | (k))

// HID usage codes (the standard USB HID keyboard page), numeric so every board's header works.
enum : uint8_t {
  H_A = 0x04, H_ENTER = 0x28, H_ESC, H_BACKSPACE, H_TAB, H_SPACE, H_MINUS, H_EQUAL,
  H_LBRACKET, H_RBRACKET, H_BACKSLASH, H_NONUS_HASH, H_SEMICOLON, H_APOSTROPHE, H_GRAVE,
  H_COMMA, H_PERIOD, H_SLASH, H_CAPSLOCK, H_F1,
  H_PAUSE = 0x48, H_INSERT, H_HOME, H_PAGE_UP, H_DELETE, H_END,
  H_PAGE_DOWN, H_RIGHT, H_LEFT, H_DOWN, H_UP, H_NUMLOCK, H_KP_DIVIDE, H_KP_MULTIPLY,
  H_KP_MINUS, H_KP_PLUS, H_KP_ENTER, H_KP_1, H_KP_0 = 0x62, H_KP_PERIOD,
};
#define H_1 0x1E   // 1..9, then 0 at 0x27

struct KeyRule { uint8_t hid, shifted, code; };

static const KeyRule kDefault[] = {
  // letters
  {H_A+0,0,Z_A}, {H_A+1,0,Z_B}, {H_A+2,0,Z_C}, {H_A+3,0,Z_D}, {H_A+4,0,Z_E}, {H_A+5,0,Z_F},
  {H_A+6,0,Z_G}, {H_A+7,0,Z_H}, {H_A+8,0,Z_I}, {H_A+9,0,Z_J}, {H_A+10,0,Z_K}, {H_A+11,0,Z_L},
  {H_A+12,0,Z_M}, {H_A+13,0,Z_N}, {H_A+14,0,Z_O}, {H_A+15,0,Z_P}, {H_A+16,0,Z_Q}, {H_A+17,0,Z_R},
  {H_A+18,0,Z_S}, {H_A+19,0,Z_T}, {H_A+20,0,Z_U}, {H_A+21,0,Z_V}, {H_A+22,0,Z_W}, {H_A+23,0,Z_X},
  {H_A+24,0,Z_Y}, {H_A+25,0,Z_Z},
  // digits; Shift-digit stays CAPS+digit (EDIT, CAPS LOCK, ..., GRAPHICS, DELETE), as on the Spectrum
  {H_1+0,0,Z_1}, {H_1+1,0,Z_2}, {H_1+2,0,Z_3}, {H_1+3,0,Z_4}, {H_1+4,0,Z_5},
  {H_1+5,0,Z_6}, {H_1+6,0,Z_7}, {H_1+7,0,Z_8}, {H_1+8,0,Z_9}, {H_1+9,0,Z_0},
  // punctuation: the SYMBOL SHIFT chord printed in red on the Spectrum key
  {H_COMMA,0,SYM(Z_N)},      {H_COMMA,1,SYM(Z_R)},        // ,  <
  {H_PERIOD,0,SYM(Z_M)},     {H_PERIOD,1,SYM(Z_T)},       // .  >
  {H_SEMICOLON,0,SYM(Z_O)},  {H_SEMICOLON,1,SYM(Z_Z)},    // ;  :
  {H_APOSTROPHE,0,SYM(Z_7)}, {H_APOSTROPHE,1,SYM(Z_P)},   // '  "
  {H_MINUS,0,SYM(Z_J)},      {H_MINUS,1,SYM(Z_0)},        // -  _
  {H_EQUAL,0,SYM(Z_L)},      {H_EQUAL,1,SYM(Z_K)},        // =  +
  {H_SLASH,0,SYM(Z_V)},      {H_SLASH,1,SYM(Z_C)},        // /  ?
  {H_SPACE,0,Z_SPACE},
  // editing / control
  {H_ENTER,0,Z_ENTER}, {H_KP_ENTER,0,Z_ENTER},
  {H_BACKSPACE,0,CAPS(Z_0)}, {H_DELETE,0,CAPS(Z_0)},             // DELETE
  {H_ESC,0,CAPS(Z_SPACE)}, {H_PAUSE,0,CAPS(Z_SPACE)},            // BREAK
  {H_TAB,0,CAPS(Z_SYM)},                                         // EXTENDED MODE
  {C64K_HID_CTRL,0,Z_SYM}, {C64K_HID_ALT,0,Z_SYM},               // SYMBOL SHIFT
  // cursor: CAPS + 5 / 6 / 7 / 8
  {H_LEFT,0,CAPS(Z_5)}, {H_DOWN,0,CAPS(Z_6)}, {H_UP,0,CAPS(Z_7)}, {H_RIGHT,0,CAPS(Z_8)},
  // keypad
  {H_KP_1+0,0,Z_1}, {H_KP_1+1,0,Z_2}, {H_KP_1+2,0,Z_3}, {H_KP_1+3,0,Z_4}, {H_KP_1+4,0,Z_5},
  {H_KP_1+5,0,Z_6}, {H_KP_1+6,0,Z_7}, {H_KP_1+7,0,Z_8}, {H_KP_1+8,0,Z_9}, {H_KP_0,0,Z_0},
  {H_KP_PLUS,0,SYM(Z_K)}, {H_KP_MINUS,0,SYM(Z_J)}, {H_KP_MULTIPLY,0,SYM(Z_B)},
  {H_KP_DIVIDE,0,SYM(Z_V)}, {H_KP_PERIOD,0,SYM(Z_M)},
};

// Overrides from the SD card. Rules equal to the default are not stored.
#define ZXK_MAX_OVERRIDES 64
static KeyRule kOver[ZXK_MAX_OVERRIDES];
static uint8_t kOverCount = 0;

static uint8_t findRule(const KeyRule *t, int n, uint8_t hid, uint8_t shifted)
{
  for (int i = 0; i < n; i++) if (t[i].hid == hid && t[i].shifted == shifted) return t[i].code;
  return ZXK_NONE;
}
static uint8_t findDefault(uint8_t hid, uint8_t shifted)
{
  return findRule(kDefault, sizeof(kDefault) / sizeof(kDefault[0]), hid, shifted);
}

uint8_t zxKeymapLookup(uint8_t hid, bool shift)
{
  uint8_t c;
  if (shift) {
    if ((c = findRule(kOver, kOverCount, hid, 1)) != ZXK_NONE) return c;
    if ((c = findDefault(hid, 1)) != ZXK_NONE) return c;
  }
  if ((c = findRule(kOver, kOverCount, hid, 0)) != ZXK_NONE) return c;
  return findDefault(hid, 0);
}

// ---- file parsing ---------------------------------------------------------------------------
// Spectrum key name -> matrix index, ZXK_UNMAP, or ZXK_NONE when unknown.
static uint8_t parseZxKey(const char *s)
{
  if (!strcmp(s, "NONE")) return ZXK_UNMAP;
  static const struct { const char *alias; uint8_t k; } kAlias[] = {
    {"CAPS_SHIFT", Z_CAPS}, {"SYMBOL_SHIFT", Z_SYM}, {"SYM", Z_SYM}, {"RETURN", Z_ENTER},
    {"BREAK", Z_SPACE},
  };
  for (auto &e : kAlias) if (!strcmp(s, e.alias)) return e.k;
  for (int i = 0; i < ZXK_KEYS; i++) if (!strcmp(s, ZX_NAMES[i])) return (uint8_t)i;
  return ZXK_NONE;
}

static void keymapError(int line, const char *what, const char *tok)
{
  snprintf(buf, sizeof(buf), "ZX keymap line %d: %s '%s' - ignored", line, what, tok);
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
  if (n < 3 || n > 4 || strcmp(tok[1], "=")) { keymapError(line, "expected 'KEY = ZXKEY', got", tok[0]); return; }

  const char *src = tok[0];
  uint8_t shifted = 0;
  if (!strncmp(src, "SHIFT+", 6)) { shifted = 1; src += 6; }
  const uint8_t hid = c64KeymapHostParse(src);
  if (!hid) { keymapError(line, "unknown host key", tok[0]); return; }

  uint8_t code = parseZxKey(tok[2]);
  if (code == ZXK_NONE) { keymapError(line, "unknown Spectrum key", tok[2]); return; }
  if (zxkIsKey(code)) {
    int mode = shifted ? ZXK_PLAIN : ZXK_KEEP;
    if (n == 4) {
      if (!strcmp(tok[3], "+CAPS"))        mode = ZXK_CAPS;
      else if (!strcmp(tok[3], "-CAPS"))   mode = ZXK_PLAIN;
      else if (!strcmp(tok[3], "+SYMBOL") || !strcmp(tok[3], "+SYM")) mode = ZXK_SYM;
      else { keymapError(line, "expected +CAPS, -CAPS or +SYMBOL, got", tok[3]); return; }
    }
    code = (uint8_t)((mode << 6) | code);
  }

  if (findDefault(hid, shifted) == code) return;   // same as the built-in rule
  for (int i = 0; i < kOverCount; i++)              // a later line replaces an earlier one
    if (kOver[i].hid == hid && kOver[i].shifted == shifted) { kOver[i].code = code; return; }
  if (kOverCount >= ZXK_MAX_OVERRIDES) { keymapError(line, "too many changed keys, at", tok[0]); return; }
  kOver[kOverCount++] = {hid, shifted, code};
}

void zxKeymapLoad()
{
  kOverCount = 0;
  static const char *const kPaths[] = { "/emu8/zx/keymap.txt", "/zx/keymap.txt", "/zxkeymap.txt" };
  for (const char *path : kPaths) {
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("ZX keymap: out of heap opening the file"); return; }
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
    snprintf(buf, sizeof(buf), "ZX keymap: %s, %d key(s) changed from the default", path, kOverCount);
    printLog(buf);
    return;
  }
}

// ---- the KEYBOARD page (optionsui.cpp) ------------------------------------------------------
const char *zxKeymapZxName(uint8_t index) { return index < ZXK_KEYS ? ZX_NAMES[index] : "?"; }

uint8_t zxKeymapZxAt(int i)
{
  // As on the keyboard: the four rows left to right.
  static const uint8_t kPick[ZXK_KEYS] = {
    Z_1, Z_2, Z_3, Z_4, Z_5, Z_6, Z_7, Z_8, Z_9, Z_0,
    Z_Q, Z_W, Z_E, Z_R, Z_T, Z_Y, Z_U, Z_I, Z_O, Z_P,
    Z_A, Z_S, Z_D, Z_F, Z_G, Z_H, Z_J, Z_K, Z_L, Z_ENTER,
    Z_CAPS, Z_Z, Z_X, Z_C, Z_V, Z_B, Z_N, Z_M, Z_SYM, Z_SPACE,
  };
  return kPick[(unsigned)i % ZXK_KEYS];
}

void zxKeymapDescribe(uint8_t code, bool shifted, char *out, size_t n)
{
  (void)shifted;   // a SHIFT+ rule is never KEEP (zxKeymapSet), so its suffix is always written
  if (code == ZXK_NONE)  { snprintf(out, n, "-");    return; }
  if (code == ZXK_UNMAP) { snprintf(out, n, "NONE"); return; }
  static const char *const kSfx[4] = { "", " +CAPS", " -CAPS", " +SYMBOL" };
  snprintf(out, n, "%s%s", ZX_NAMES[zxkIndex(code) % ZXK_KEYS], kSfx[zxkMode(code)]);
}

uint8_t zxKeymapDefault(uint8_t hid, bool shifted) { return findDefault(hid, shifted ? 1 : 0); }

uint8_t zxKeymapRule(uint8_t hid, bool shifted, bool *over)
{
  uint8_t c = findRule(kOver, kOverCount, hid, shifted ? 1 : 0);
  if (over) *over = c != ZXK_NONE;
  return c != ZXK_NONE ? c : zxKeymapDefault(hid, shifted);
}

bool zxKeymapSet(uint8_t hid, bool shifted, uint8_t code)
{
  const uint8_t sh = shifted ? 1 : 0;
  // A SHIFT+ rule has no "as typed": Shift is down whenever it applies, so that is +CAPS. Keeps
  // the file unambiguous too (a SHIFT+ line without a suffix reads as -CAPS).
  if (sh && zxkIsKey(code) && zxkMode(code) == ZXK_KEEP) code = (uint8_t)((ZXK_CAPS << 6) | code);
  int at = -1;
  for (int i = 0; i < kOverCount; i++) if (kOver[i].hid == hid && kOver[i].shifted == sh) at = i;
  if (code == ZXK_NONE || code == findDefault(hid, sh)) {           // back to the default
    if (at >= 0) kOver[at] = kOver[--kOverCount];
    return true;
  }
  if (at >= 0) { kOver[at].code = code; return true; }
  if (kOverCount >= ZXK_MAX_OVERRIDES) return false;
  kOver[kOverCount++] = {hid, sh, code};
  return true;
}

void zxKeymapResetAll()     { kOverCount = 0; }
int  zxKeymapChangedCount() { return kOverCount; }

bool zxKeymapSave()
{
  static const char *const kPath = "/emu8/zx/keymap.txt";
  // Timed lock, not busTake(): this runs in the render task (the menu).
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  bool ok = false;
  try {
    if (!FSTYPE.exists("/emu8")) FSTYPE.mkdir("/emu8");
    if (!FSTYPE.exists("/emu8/zx")) FSTYPE.mkdir("/emu8/zx");
    if (FSTYPE.exists(kPath)) FSTYPE.remove(kPath);     // FILE_WRITE appends on arduino-pico
    File f = FSTYPE.open(kPath, FILE_WRITE);
    if (f) {
      static const char kHead[] =
        "# emu8 ZX Spectrum keyboard map, saved from the ZX menu (CONFIG > KEYBOARD), which rewrites it.\n"
        "# Only the keys changed from the built-in default are listed; the syntax and the whole\n"
        "# default map are in docs/zx-keymap.txt.\n";
      ok = f.write((const uint8_t *)kHead, sizeof(kHead) - 1) == sizeof(kHead) - 1;
      char line[64], name[16], rule[24];
      for (int i = 0; ok && i < kOverCount; i++) {
        c64KeymapHostName(kOver[i].hid, name, sizeof(name));
        zxKeymapDescribe(kOver[i].code, kOver[i].shifted, rule, sizeof(rule));
        int len = snprintf(line, sizeof(line), "%s%s = %s\n", kOver[i].shifted ? "SHIFT+" : "", name, rule);
        ok = f.write((const uint8_t *)line, len) == (size_t)len;
      }
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "ZX keymap: %s %s, %d key(s) changed", kPath, ok ? "saved" : "NOT saved", kOverCount);
  printLog(buf);
  return ok;
}

#endif // BOARD_INPUT_USB
