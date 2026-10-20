// msxjoymap.cpp - which MSX joystick (port 1) line each host key pulls while JOYSTICK is on (USB
// keyboard, PicoCalc keyboard, the desktop build's PC keyboard: everything that reaches
// usbKeyboardReport()).
//
// A built-in default map, plus overrides read from a text file on the SD card when the MSX boots:
// /emu8/msx/joymap.txt (or /msxjoymap.txt at the root). One rule per line, '#' starts a comment:
//
//     <host key> = <joystick action>
//
//     W = UP                       # WASD steers
//     ALT = TRIG_B                 # Alt is the second button
//     SPACE = NONE                 # Space only types
//
// Host keys are named as in the keyboard map (docs/msx-keymap.txt), plus SHIFT (either one). The
// actions are UP DOWN LEFT RIGHT, TRIG_A,
// TRIG_B (FIRE / FIRE2 are accepted for them), or NONE. The joystick map does not replace the
// keyboard map: a key on the stick still types (msxkeymap.cpp), as the arrows and Space always have
// (the MSX BIOS reads them as STICK(0) / STRIG(0)). The full default map is in docs/msx-joymap.txt.
//
// The default: the arrows steer; the PicoCalc's third row gives the triggers: ] TRIG A, [ TRIG B.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "msxjoymap.h"
#include "c64keymap.h"

struct JoyRule { uint8_t hid, code; };

// HID usage codes (the standard USB HID keyboard page), numeric so every board's header works.
static const JoyRule kDefault[] = {
  {0x52, C64J_UP}, {0x51, C64J_DOWN}, {0x50, C64J_LEFT}, {0x4F, C64J_RIGHT},   // arrows
  {0x30, MSXJ_TRIG_A}, {0x2F, MSXJ_TRIG_B},                                   // ]  [
};

// The picker's order: the stick (UP RIGHT DOWN LEFT), then the buttons.
static const struct { uint8_t code; const char *name; } kActions[] = {
  {C64J_UP, "UP"}, {C64J_RIGHT, "RIGHT"}, {C64J_DOWN, "DOWN"}, {C64J_LEFT, "LEFT"},
  {MSXJ_TRIG_A, "TRIG_A"}, {MSXJ_TRIG_B, "TRIG_B"},
};
#define JOY_NACT ((int)(sizeof(kActions) / sizeof(kActions[0])))

// Overrides from the SD card. Rules equal to the default are not stored.
#define MSXJ_MAX_OVERRIDES 64
static JoyRule kOver[MSXJ_MAX_OVERRIDES];
static uint8_t kOverCount = 0;

static uint8_t findRule(const JoyRule *t, int n, uint8_t hid)
{
  for (int i = 0; i < n; i++) if (t[i].hid == hid) return t[i].code;
  return C64J_NONE;
}

uint8_t msxJoymapDefault(uint8_t hid) { return findRule(kDefault, sizeof(kDefault) / sizeof(kDefault[0]), hid); }

uint8_t msxJoymapLookup(uint8_t hid)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  return c != C64J_NONE ? c : msxJoymapDefault(hid);
}

// ---- names ----------------------------------------------------------------------------------
static uint8_t hostKey(const char *s)
{
  if (!strcmp(s, "SHIFT")) return C64K_HID_SHIFT;
  return c64KeymapHostParse(s);
}

int     msxJoymapActionCount()   { return JOY_NACT; }
uint8_t msxJoymapActionAt(int i) { return kActions[i % JOY_NACT].code; }

const char *msxJoymapActionName(uint8_t code)
{
  for (auto &a : kActions) if (a.code == code) return a.name;
  return "?";
}

// Action name -> code, C64J_UNMAP for NONE, C64J_NONE when unknown.
static uint8_t joyAction(const char *s)
{
  if (!strcmp(s, "NONE")) return C64J_UNMAP;
  if (!strcmp(s, "FIRE") || !strcmp(s, "TRIG_1") || !strcmp(s, "TRIGA"))  return MSXJ_TRIG_A;
  if (!strcmp(s, "FIRE2") || !strcmp(s, "TRIG_2") || !strcmp(s, "TRIGB")) return MSXJ_TRIG_B;
  for (auto &a : kActions) if (!strcmp(s, a.name)) return a.code;
  return C64J_NONE;
}

// ---- file parsing ---------------------------------------------------------------------------
static void joymapError(int line, const char *what, const char *tok)
{
  snprintf(buf, sizeof(buf), "MSX joymap line %d: %s '%s' - ignored", line, what, tok);
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

  if (msxJoymapDefault(hid) == code) return;        // same as the built-in rule
  for (int i = 0; i < kOverCount; i++)              // a later line replaces an earlier one
    if (kOver[i].hid == hid) { kOver[i].code = code; return; }
  if (kOverCount >= MSXJ_MAX_OVERRIDES) { joymapError(line, "too many changed keys, at", tok[0]); return; }
  kOver[kOverCount++] = {hid, code};
}

void msxJoymapLoad()
{
  kOverCount = 0;
  static const char *const kPaths[] = { "/emu8/msx/joymap.txt", "/msx/joymap.txt", "/msxjoymap.txt" };
  for (const char *path : kPaths) {
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("MSX joymap: out of heap opening the file"); return; }
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
    snprintf(buf, sizeof(buf), "MSX joymap: %s, %d key(s) changed from the default", path, kOverCount);
    printLog(buf);
    return;
  }
}

// ---- the JOYSTICK page (optionsui.cpp) ------------------------------------------------------
void msxJoymapDescribe(uint8_t code, char *out, size_t n)
{
  if (code == C64J_NONE)       snprintf(out, n, "-");
  else if (code == C64J_UNMAP) snprintf(out, n, "NONE");
  else                         snprintf(out, n, "%s", msxJoymapActionName(code));
}

uint8_t msxJoymapRule(uint8_t hid, bool *over)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  if (over) *over = c != C64J_NONE;
  return c != C64J_NONE ? c : msxJoymapDefault(hid);
}

bool msxJoymapSet(uint8_t hid, uint8_t code)
{
  int at = -1;
  for (int i = 0; i < kOverCount; i++) if (kOver[i].hid == hid) at = i;
  if (code == C64J_NONE || code == msxJoymapDefault(hid)) {        // back to the default
    if (at >= 0) kOver[at] = kOver[--kOverCount];
    return true;
  }
  if (at >= 0) { kOver[at].code = code; return true; }
  if (kOverCount >= MSXJ_MAX_OVERRIDES) return false;
  kOver[kOverCount++] = {hid, code};
  return true;
}

void msxJoymapResetAll()     { kOverCount = 0; }
int  msxJoymapChangedCount() { return kOverCount; }

bool msxJoymapSave()
{
  static const char *const kPath = "/emu8/msx/joymap.txt";
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
        "# emu8 MSX joystick map, saved from the MSX menu (CONFIG > JOYSTICK), which rewrites it.\n"
        "# Only the keys changed from the built-in default are listed; the syntax and the whole\n"
        "# default map are in docs/msx-joymap.txt.\n";
      ok = f.write((const uint8_t *)kHead, sizeof(kHead) - 1) == sizeof(kHead) - 1;
      char line[48], name[16], rule[16];
      for (int i = 0; ok && i < kOverCount; i++) {
        c64JoymapHostName(kOver[i].hid, name, sizeof(name));
        msxJoymapDescribe(kOver[i].code, rule, sizeof(rule));
        int len = snprintf(line, sizeof(line), "%s = %s\n", name, rule);
        ok = f.write((const uint8_t *)line, len) == (size_t)len;
      }
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "MSX joymap: %s %s, %d key(s) changed", kPath, ok ? "saved" : "NOT saved", kOverCount);
  printLog(buf);
  return ok;
}

#endif // BOARD_INPUT_USB
