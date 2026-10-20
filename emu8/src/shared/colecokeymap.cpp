// colecokeymap.cpp - what each host key does on ColecoVision controller 1 (USB keyboard, PicoCalc
// keyboard, the desktop build's PC keyboard: everything that reaches usbKeyboardReport()).
//
// A built-in default map, plus overrides read from a text file on the SD card when the Coleco
// boots: /coleco/keymap.txt (or /colecokeymap.txt at the root). One rule per line, '#' starts a
// comment:
//
//     [SHIFT+]<host key> = <Coleco button>
//
//     ENTER = LFIRE                # Enter is the left fire button too
//     SHIFT+1 = *                  # Shift-1 is the keypad's '*'
//     SPACE = NONE                 # Space does nothing
//
// Host keys are named as in the C64 map (c64keymap.cpp, docs/c64-keymap.txt); the Coleco buttons
// are UP DOWN LEFT RIGHT LFIRE RFIRE, the keypad's 0..9 * HASH, or NONE ('#' is a comment, so the
// file spells it HASH). Any number of keys may press the same button. A SHIFT+ rule is used when the
// key is typed with SHIFT and wins over the plain one; without one a shifted key is the plain key.
// The full default map is in docs/coleco-keymap.txt. Reset (F12 / PicoCalc F3) is the console's,
// not the controller's: not here.
//
// The default: the arrows are the stick, Space / Z / F4 the left fire, X / F5 the right fire, the
// digit row and the numeric keypad the keypad, with '*' / '#' on Shift-8 / Shift-3 (as printed;
// it is what the PicoCalc's own '*' / '#' send) and on '-' / '='.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "colecokeymap.h"
#include "c64keymap.h"

static const char *const COL_NAMES[COLK_COUNT] = {
  "UP", "DOWN", "LEFT", "RIGHT", "LFIRE", "RFIRE",
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#",
};

struct ColRule { uint8_t hid, shifted, code; };

// HID usage codes (the standard USB HID keyboard page), numeric so every board's header works.
static const ColRule kDefault[] = {
  {0x52, 0, COLK_UP}, {0x51, 0, COLK_DOWN}, {0x50, 0, COLK_LEFT}, {0x4F, 0, COLK_RIGHT},   // arrows
  {0x2C, 0, COLK_LFIRE}, {0x1D, 0, COLK_LFIRE}, {0x3D, 0, COLK_LFIRE},                    // Space  Z  F4
  {0x1B, 0, COLK_RFIRE}, {0x3E, 0, COLK_RFIRE},                                           // X  F5
  {0x27, 0, COLK_KEY0 + 0}, {0x1E, 0, COLK_KEY0 + 1}, {0x1F, 0, COLK_KEY0 + 2},           // 0 .. 9
  {0x20, 0, COLK_KEY0 + 3}, {0x21, 0, COLK_KEY0 + 4}, {0x22, 0, COLK_KEY0 + 5},
  {0x23, 0, COLK_KEY0 + 6}, {0x24, 0, COLK_KEY0 + 7}, {0x25, 0, COLK_KEY0 + 8},
  {0x26, 0, COLK_KEY0 + 9},
  {0x62, 0, COLK_KEY0 + 0}, {0x59, 0, COLK_KEY0 + 1}, {0x5A, 0, COLK_KEY0 + 2},           // KP_0 .. KP_9
  {0x5B, 0, COLK_KEY0 + 3}, {0x5C, 0, COLK_KEY0 + 4}, {0x5D, 0, COLK_KEY0 + 5},
  {0x5E, 0, COLK_KEY0 + 6}, {0x5F, 0, COLK_KEY0 + 7}, {0x60, 0, COLK_KEY0 + 8},
  {0x61, 0, COLK_KEY0 + 9},
  {0x2D, 0, COLK_STAR}, {0x55, 0, COLK_STAR}, {0x2E, 0, COLK_HASH},                       // -  KP_*  =
  {0x25, 1, COLK_STAR}, {0x20, 1, COLK_HASH},                                             // Shift-8  Shift-3
};
#define COLK_NDEFAULT ((int)(sizeof(kDefault) / sizeof(kDefault[0])))

// Overrides from the SD card. Rules equal to the default are not stored.
#define COLK_MAX_OVERRIDES 64
static ColRule kOver[COLK_MAX_OVERRIDES];
static uint8_t kOverCount = 0;

static uint8_t findRule(const ColRule *t, int n, uint8_t hid, bool shifted)
{
  for (int i = 0; i < n; i++) if (t[i].hid == hid && t[i].shifted == shifted) return t[i].code;
  return COLK_NONE;
}

uint8_t colecoKeymapDefault(uint8_t hid, bool shifted) { return findRule(kDefault, COLK_NDEFAULT, hid, shifted); }

uint8_t colecoKeymapRule(uint8_t hid, bool shifted, bool *over)
{
  const uint8_t c = findRule(kOver, kOverCount, hid, shifted);
  if (over) *over = c != COLK_NONE;
  return c != COLK_NONE ? c : colecoKeymapDefault(hid, shifted);
}

uint8_t colecoKeymapLookup(uint8_t hid, bool shifted)
{
  if (shifted) {
    const uint8_t c = colecoKeymapRule(hid, true, nullptr);
    if (c != COLK_NONE) return c;
  }
  return colecoKeymapRule(hid, false, nullptr);
}

// ---- file parsing ---------------------------------------------------------------------------
// Coleco button name -> code, COLK_UNMAP for NONE, COLK_NONE when unknown. FIRE / FIRE1 / FIRE2,
// STAR and HASH are taken too.
static uint8_t colButton(const char *s)
{
  if (!strcmp(s, "NONE")) return COLK_UNMAP;
  if (!strcmp(s, "FIRE") || !strcmp(s, "FIRE1")) return COLK_LFIRE;
  if (!strcmp(s, "FIRE2")) return COLK_RFIRE;
  if (!strcmp(s, "STAR"))  return COLK_STAR;
  if (!strcmp(s, "HASH"))  return COLK_HASH;
  for (int i = 0; i < COLK_COUNT; i++) if (!strcmp(s, COL_NAMES[i])) return (uint8_t)i;
  return COLK_NONE;
}

static void keymapError(int line, const char *what, const char *tok)
{
  snprintf(buf, sizeof(buf), "Coleco keymap line %d: %s '%s' - ignored", line, what, tok);
  printLog(buf);
}

static void parseLine(char *s, int line)
{
  if (char *h = strchr(s, '#')) *h = 0;
  char *tok[4];
  int n = 0;
  for (char *p = strtok(s, " \t\r"); p && n < 4; p = strtok(nullptr, " \t\r")) {
    for (char *q = p; *q; q++) *q = (char)toupper((unsigned char)*q);
    tok[n++] = p;
  }
  if (n == 0) return;
  if (n != 3 || strcmp(tok[1], "=")) { keymapError(line, "expected 'KEY = BUTTON', got", tok[0]); return; }
  const char *key = tok[0];
  const bool shifted = !strncmp(key, "SHIFT+", 6);
  if (shifted) key += 6;
  const uint8_t hid = c64KeymapHostParse(key);
  if (!hid) { keymapError(line, "unknown host key", tok[0]); return; }
  if (shifted && (hid == C64K_HID_CTRL || hid == C64K_HID_ALT)) { keymapError(line, "no SHIFT+ rule for", tok[0]); return; }
  const uint8_t code = colButton(tok[2]);
  if (code == COLK_NONE) { keymapError(line, "unknown Coleco button", tok[2]); return; }

  if (colecoKeymapDefault(hid, shifted) == code) return;   // same as the built-in rule
  for (int i = 0; i < kOverCount; i++)                     // a later line replaces an earlier one
    if (kOver[i].hid == hid && kOver[i].shifted == shifted) { kOver[i].code = code; return; }
  if (kOverCount >= COLK_MAX_OVERRIDES) { keymapError(line, "too many changed keys, at", tok[0]); return; }
  kOver[kOverCount++] = {hid, (uint8_t)shifted, code};
}

void colecoKeymapLoad()
{
  kOverCount = 0;
  static const char *const kPaths[] = { "/coleco/keymap.txt", "/colecokeymap.txt" };
  for (const char *path : kPaths) {
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("Coleco keymap: out of heap opening the file"); return; }
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
    snprintf(buf, sizeof(buf), "Coleco keymap: %s, %d key(s) changed from the default", path, kOverCount);
    printLog(buf);
    return;
  }
}

// ---- the KEYBOARD page (optionsui.cpp) ------------------------------------------------------
const char *colecoKeymapButtonName(uint8_t b) { return b < COLK_COUNT ? COL_NAMES[b] : "?"; }

uint8_t colecoKeymapButtonAt(int i)
{
  static const uint8_t kPick[COLK_COUNT] = {
    COLK_UP, COLK_DOWN, COLK_LEFT, COLK_RIGHT, COLK_LFIRE, COLK_RFIRE,
    COLK_KEY0 + 1, COLK_KEY0 + 2, COLK_KEY0 + 3, COLK_KEY0 + 4, COLK_KEY0 + 5, COLK_KEY0 + 6,
    COLK_KEY0 + 7, COLK_KEY0 + 8, COLK_KEY0 + 9, COLK_STAR, COLK_KEY0 + 0, COLK_HASH,
  };
  return kPick[(unsigned)i < COLK_COUNT ? i : 0];
}

void colecoKeymapDescribe(uint8_t code, char *out, size_t n)
{
  if (code == COLK_NONE)       snprintf(out, n, "-");
  else if (code == COLK_UNMAP) snprintf(out, n, "NONE");
  else                         snprintf(out, n, "%s", colecoKeymapButtonName(code));
}

bool colecoKeymapSet(uint8_t hid, bool shifted, uint8_t code)
{
  int at = -1;
  for (int i = 0; i < kOverCount; i++) if (kOver[i].hid == hid && kOver[i].shifted == shifted) at = i;
  if (code == COLK_NONE || code == colecoKeymapDefault(hid, shifted)) {   // back to the default
    if (at >= 0) kOver[at] = kOver[--kOverCount];
    return true;
  }
  if (at >= 0) { kOver[at].code = code; return true; }
  if (kOverCount >= COLK_MAX_OVERRIDES) return false;
  kOver[kOverCount++] = {hid, (uint8_t)shifted, code};
  return true;
}

void colecoKeymapResetAll()     { kOverCount = 0; }
int  colecoKeymapChangedCount() { return kOverCount; }

bool colecoKeymapSave()
{
  static const char *const kPath = "/coleco/keymap.txt";
  // Timed lock, not busTake(): this runs in the render task (the menu).
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  bool ok = false;
  try {
    if (!FSTYPE.exists("/coleco")) FSTYPE.mkdir("/coleco");
    if (FSTYPE.exists(kPath)) FSTYPE.remove(kPath);     // FILE_WRITE appends on arduino-pico
    File f = FSTYPE.open(kPath, FILE_WRITE);
    if (f) {
      static const char kHead[] =
        "# emu8 ColecoVision keyboard map, saved from the Coleco menu (CONFIG > KEYBOARD), which\n"
        "# rewrites it. Only the keys changed from the built-in default are listed; the syntax and\n"
        "# the whole default map are in docs/coleco-keymap.txt.\n";
      ok = f.write((const uint8_t *)kHead, sizeof(kHead) - 1) == sizeof(kHead) - 1;
      char line[48], name[16], rule[16];
      for (int i = 0; ok && i < kOverCount; i++) {
        c64KeymapHostName(kOver[i].hid, name, sizeof(name));
        if (kOver[i].code == COLK_HASH) snprintf(rule, sizeof(rule), "HASH");   // '#' would be a comment
        else colecoKeymapDescribe(kOver[i].code, rule, sizeof(rule));
        int len = snprintf(line, sizeof(line), "%s%s = %s\n", kOver[i].shifted ? "SHIFT+" : "", name, rule);
        ok = f.write((const uint8_t *)line, len) == (size_t)len;
      }
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "Coleco keymap: %s %s, %d key(s) changed", kPath, ok ? "saved" : "NOT saved", kOverCount);
  printLog(buf);
  return ok;
}

#endif // BOARD_INPUT_USB
