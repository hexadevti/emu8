// c64joymap.cpp - which C64 joystick line each host key pulls while JOYSTICK is on (USB keyboard,
// PicoCalc keyboard, the desktop build's PC keyboard: everything that reaches usbKeyboardReport()).
//
// A built-in default map, plus overrides read from a text file on the SD card when the C64 boots:
// /c64/joymap.txt (or /c64joymap.txt at the root). One rule per line, '#' starts a comment:
//
//     <host key> = <joystick action>
//
//     W = UP                       # WASD steers
//     Q = UP+LEFT                  # a diagonal on one key
//     CTRL = FIRE                  # Ctrl fires too
//     SPACE = NONE                 # Space only types
//
// Host keys are named as in the C64 keyboard map (c64keymap.cpp, docs/c64-keymap.txt), plus SHIFT
// (either one). The actions are UP DOWN LEFT RIGHT, the diagonals UP+LEFT UP+RIGHT DOWN+LEFT
// DOWN+RIGHT, FIRE, FIRE2 / FIRE3 (a 2/3-button joystick's extra buttons, read on SID POTX / POTY),
// or NONE. Any number of keys may pull the same line. The joystick map does not
// replace the keyboard map: a key on the stick still types (c64keymap.cpp), as the arrows always
// have. The full default map is in docs/c64-joymap.txt.
//
// The default: the arrows steer and Space fires; on the PicoCalc either SHIFT fires as well.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "c64joymap.h"
#include "c64keymap.h"

struct JoyRule { uint8_t hid, code; };

// HID usage codes (the standard USB HID keyboard page), numeric so every board's header works.
static const JoyRule kDefault[] = {
  {0x52, C64J_UP}, {0x51, C64J_DOWN}, {0x50, C64J_LEFT}, {0x4F, C64J_RIGHT},   // arrows
  {0x2C, C64J_FIRE},                                                           // Space
#if UI_PICOCALC
  // The cursor cluster sits at the bottom right and the space bar is a long reach from it, while
  // the two shifts flank that same bottom row.
  {C64K_HID_SHIFT, C64J_FIRE},
#endif
};

// The picker's order: the stick clockwise from UP, then the buttons.
static const struct { uint8_t code; const char *name; } kActions[] = {
  {C64J_UP, "UP"}, {C64J_UP | C64J_RIGHT, "UP+RIGHT"}, {C64J_RIGHT, "RIGHT"},
  {C64J_DOWN | C64J_RIGHT, "DOWN+RIGHT"}, {C64J_DOWN, "DOWN"}, {C64J_DOWN | C64J_LEFT, "DOWN+LEFT"},
  {C64J_LEFT, "LEFT"}, {C64J_UP | C64J_LEFT, "UP+LEFT"}, {C64J_FIRE, "FIRE"},
  {C64J_FIRE2, "FIRE2"}, {C64J_FIRE3, "FIRE3"},
};
#define JOY_NACT ((int)(sizeof(kActions) / sizeof(kActions[0])))

// Overrides from the SD card. Rules equal to the default are not stored.
#define C64J_MAX_OVERRIDES 64
static JoyRule kOver[C64J_MAX_OVERRIDES];
static uint8_t kOverCount = 0;

static uint8_t findRule(const JoyRule *t, int n, uint8_t hid)
{
  for (int i = 0; i < n; i++) if (t[i].hid == hid) return t[i].code;
  return C64J_NONE;
}

uint8_t c64JoymapDefault(uint8_t hid) { return findRule(kDefault, sizeof(kDefault) / sizeof(kDefault[0]), hid); }

uint8_t c64JoymapLookup(uint8_t hid)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  return c != C64J_NONE ? c : c64JoymapDefault(hid);
}

// ---- names ----------------------------------------------------------------------------------
static uint8_t hostKey(const char *s)
{
  if (!strcmp(s, "SHIFT")) return C64K_HID_SHIFT;
  return c64KeymapHostParse(s);
}

void c64JoymapHostName(uint8_t hid, char *out, size_t n)
{
  if (hid == C64K_HID_SHIFT) snprintf(out, n, "SHIFT");
  else c64KeymapHostName(hid, out, n);
}

int     c64JoymapHostCount()   { return c64KeymapHostCount() + 1; }
uint8_t c64JoymapHostAt(int i) { return i < c64KeymapHostCount() ? c64KeymapHostAt(i) : (uint8_t)C64K_HID_SHIFT; }

int     c64JoymapActionCount()   { return JOY_NACT; }
uint8_t c64JoymapActionAt(int i) { return kActions[i % JOY_NACT].code; }

const char *c64JoymapActionName(uint8_t code)
{
  for (auto &a : kActions) if (a.code == code) return a.name;
  return "?";
}

// Action name -> code, C64J_UNMAP for NONE, C64J_NONE when unknown. LEFT+UP is UP+LEFT.
static uint8_t joyAction(const char *s)
{
  if (!strcmp(s, "NONE")) return C64J_UNMAP;
  for (auto &a : kActions) if (!strcmp(s, a.name)) return a.code;
  if (const char *p = strchr(s, '+')) {
    char a[12];
    const size_t n = (size_t)(p - s);
    if (n >= sizeof(a)) return C64J_NONE;
    memcpy(a, s, n); a[n] = 0;
    uint8_t x = joyAction(a), y = joyAction(p + 1);
    if (!c64jIsAction(x) || !c64jIsAction(y) || ((x | y) & ~0x0F)) return C64J_NONE;   // directions only
    for (auto &e : kActions) if (e.code == (x | y)) return e.code;
  }
  return C64J_NONE;
}

// ---- file parsing ---------------------------------------------------------------------------
static void joymapError(int line, const char *what, const char *tok)
{
  snprintf(buf, sizeof(buf), "C64 joymap line %d: %s '%s' - ignored", line, what, tok);
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
  if (n != 3 || strcmp(tok[1], "=")) { joymapError(line, "expected 'KEY = ACTION', got", tok[0]); return; }
  const uint8_t hid = hostKey(tok[0]);
  if (!hid) { joymapError(line, "unknown host key", tok[0]); return; }
  const uint8_t code = joyAction(tok[2]);
  if (code == C64J_NONE) { joymapError(line, "unknown joystick action", tok[2]); return; }

  if (c64JoymapDefault(hid) == code) return;        // same as the built-in rule
  for (int i = 0; i < kOverCount; i++)              // a later line replaces an earlier one
    if (kOver[i].hid == hid) { kOver[i].code = code; return; }
  if (kOverCount >= C64J_MAX_OVERRIDES) { joymapError(line, "too many changed keys, at", tok[0]); return; }
  kOver[kOverCount++] = {hid, code};
}

void c64JoymapLoad()
{
  kOverCount = 0;
  static const char *const kPaths[] = { "/c64/joymap.txt", "/c64joymap.txt" };
  for (const char *path : kPaths) {
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("C64 joymap: out of heap opening the file"); return; }
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
    snprintf(buf, sizeof(buf), "C64 joymap: %s, %d key(s) changed from the default", path, kOverCount);
    printLog(buf);
    return;
  }
}

// ---- the JOYSTICK page (optionsui.cpp) ------------------------------------------------------
void c64JoymapDescribe(uint8_t code, char *out, size_t n)
{
  if (code == C64J_NONE)       snprintf(out, n, "-");
  else if (code == C64J_UNMAP) snprintf(out, n, "NONE");
  else                         snprintf(out, n, "%s", c64JoymapActionName(code));
}

uint8_t c64JoymapRule(uint8_t hid, bool *over)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  if (over) *over = c != C64J_NONE;
  return c != C64J_NONE ? c : c64JoymapDefault(hid);
}

bool c64JoymapSet(uint8_t hid, uint8_t code)
{
  int at = -1;
  for (int i = 0; i < kOverCount; i++) if (kOver[i].hid == hid) at = i;
  if (code == C64J_NONE || code == c64JoymapDefault(hid)) {        // back to the default
    if (at >= 0) kOver[at] = kOver[--kOverCount];
    return true;
  }
  if (at >= 0) { kOver[at].code = code; return true; }
  if (kOverCount >= C64J_MAX_OVERRIDES) return false;
  kOver[kOverCount++] = {hid, code};
  return true;
}

void c64JoymapResetAll()     { kOverCount = 0; }
int  c64JoymapChangedCount() { return kOverCount; }

bool c64JoymapSave()
{
  static const char *const kPath = "/c64/joymap.txt";
  // Timed lock, not busTake(): this runs in the render task (the menu).
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  bool ok = false;
  try {
    if (!FSTYPE.exists("/c64")) FSTYPE.mkdir("/c64");
    if (FSTYPE.exists(kPath)) FSTYPE.remove(kPath);     // FILE_WRITE appends on arduino-pico
    File f = FSTYPE.open(kPath, FILE_WRITE);
    if (f) {
      static const char kHead[] =
        "# emu8 C64 joystick map, saved from the C64 menu (CONFIG > JOYSTICK), which rewrites it.\n"
        "# Only the keys changed from the built-in default are listed; the syntax and the whole\n"
        "# default map are in docs/c64-joymap.txt.\n";
      ok = f.write((const uint8_t *)kHead, sizeof(kHead) - 1) == sizeof(kHead) - 1;
      char line[48], name[16], rule[16];
      for (int i = 0; ok && i < kOverCount; i++) {
        c64JoymapHostName(kOver[i].hid, name, sizeof(name));
        c64JoymapDescribe(kOver[i].code, rule, sizeof(rule));
        int len = snprintf(line, sizeof(line), "%s = %s\n", name, rule);
        ok = f.write((const uint8_t *)line, len) == (size_t)len;
      }
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "C64 joymap: %s %s, %d key(s) changed", kPath, ok ? "saved" : "NOT saved", kOverCount);
  printLog(buf);
  return ok;
}

#endif // BOARD_INPUT_USB
