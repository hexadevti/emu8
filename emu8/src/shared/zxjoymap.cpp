// zxjoymap.cpp - which Kempston joystick line each host key pulls while JOYSTICK is on (USB keyboard,
// PicoCalc keyboard, the desktop build's PC keyboard: everything that reaches usbKeyboardReport()).
//
// A built-in default map, plus overrides read from a text file on the SD card when the Spectrum
// boots: /emu8/zx/joymap.txt (or /zxjoymap.txt at the root). One rule per line, '#' starts a comment:
//
//     <host key> = <joystick action>
//
//     Q = UP                       # QAOP steers
//     A = DOWN
//     M = FIRE
//     SPACE = NONE                 # Space types again
//
// Host keys are named as in the C64 keyboard map (docs/c64-keymap.txt). The actions are UP DOWN
// LEFT RIGHT, FIRE, or NONE. Any number of keys
// may pull the same line. While JOYSTICK is on a key with a joystick rule only moves the stick and
// does not type; the others type through the keyboard map (zxkeymap.cpp). The full default map is
// in docs/zx-joymap.txt.
//
// The default: the arrows steer and ] (the PicoCalc's third row) fires.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "zxjoymap.h"
#include "c64keymap.h"

struct JoyRule { uint8_t hid, code; };

// HID usage codes (the standard USB HID keyboard page), numeric so every board's header works.
static const JoyRule kDefault[] = {
  {0x52, ZXJ_UP}, {0x51, ZXJ_DOWN}, {0x50, ZXJ_LEFT}, {0x4F, ZXJ_RIGHT},   // arrows
  {0x30, ZXJ_FIRE},                                                        // ]
};

// The picker's order: the stick (UP RIGHT DOWN LEFT), then the button.
static const struct { uint8_t code; const char *name; } kActions[] = {
  {ZXJ_UP, "UP"}, {ZXJ_RIGHT, "RIGHT"}, {ZXJ_DOWN, "DOWN"}, {ZXJ_LEFT, "LEFT"},
  {ZXJ_FIRE, "FIRE"},
};
#define JOY_NACT ((int)(sizeof(kActions) / sizeof(kActions[0])))

// Overrides from the SD card. Rules equal to the default are not stored.
#define ZXJ_MAX_OVERRIDES 64
static JoyRule kOver[ZXJ_MAX_OVERRIDES];
static uint8_t kOverCount = 0;

static uint8_t findRule(const JoyRule *t, int n, uint8_t hid)
{
  for (int i = 0; i < n; i++) if (t[i].hid == hid) return t[i].code;
  return ZXJ_NONE;
}

uint8_t zxJoymapDefault(uint8_t hid) { return findRule(kDefault, sizeof(kDefault) / sizeof(kDefault[0]), hid); }

uint8_t zxJoymapLookup(uint8_t hid)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  return c != ZXJ_NONE ? c : zxJoymapDefault(hid);
}

// ---- names ----------------------------------------------------------------------------------
int     zxJoymapActionCount()   { return JOY_NACT; }
uint8_t zxJoymapActionAt(int i) { return kActions[i % JOY_NACT].code; }

const char *zxJoymapActionName(uint8_t code)
{
  for (auto &a : kActions) if (a.code == code) return a.name;
  return "?";
}

// Action name -> code, ZXJ_UNMAP for NONE, ZXJ_NONE when unknown.
static uint8_t joyAction(const char *s)
{
  if (!strcmp(s, "NONE")) return ZXJ_UNMAP;
  for (auto &a : kActions) if (!strcmp(s, a.name)) return a.code;
  return ZXJ_NONE;
}

// ---- file parsing ---------------------------------------------------------------------------
static void joymapError(int line, const char *what, const char *tok)
{
  snprintf(buf, sizeof(buf), "ZX joymap line %d: %s '%s' - ignored", line, what, tok);
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
  const uint8_t hid = c64KeymapHostParse(tok[0]);
  if (!hid) { joymapError(line, "unknown host key", tok[0]); return; }
  const uint8_t code = joyAction(tok[2]);
  if (code == ZXJ_NONE) { joymapError(line, "unknown joystick action", tok[2]); return; }

  if (zxJoymapDefault(hid) == code) return;         // same as the built-in rule
  for (int i = 0; i < kOverCount; i++)              // a later line replaces an earlier one
    if (kOver[i].hid == hid) { kOver[i].code = code; return; }
  if (kOverCount >= ZXJ_MAX_OVERRIDES) { joymapError(line, "too many changed keys, at", tok[0]); return; }
  kOver[kOverCount++] = {hid, code};
}

void zxJoymapLoad()
{
  kOverCount = 0;
  static const char *const kPaths[] = { "/emu8/zx/joymap.txt", "/zx/joymap.txt", "/zxjoymap.txt" };
  for (const char *path : kPaths) {
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("ZX joymap: out of heap opening the file"); return; }
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
    snprintf(buf, sizeof(buf), "ZX joymap: %s, %d key(s) changed from the default", path, kOverCount);
    printLog(buf);
    return;
  }
}

// ---- the JOYSTICK page (optionsui.cpp) ------------------------------------------------------
void zxJoymapDescribe(uint8_t code, char *out, size_t n)
{
  if (code == ZXJ_NONE)       snprintf(out, n, "-");
  else if (code == ZXJ_UNMAP) snprintf(out, n, "NONE");
  else                        snprintf(out, n, "%s", zxJoymapActionName(code));
}

uint8_t zxJoymapRule(uint8_t hid, bool *over)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  if (over) *over = c != ZXJ_NONE;
  return c != ZXJ_NONE ? c : zxJoymapDefault(hid);
}

bool zxJoymapSet(uint8_t hid, uint8_t code)
{
  int at = -1;
  for (int i = 0; i < kOverCount; i++) if (kOver[i].hid == hid) at = i;
  if (code == ZXJ_NONE || code == zxJoymapDefault(hid)) {          // back to the default
    if (at >= 0) kOver[at] = kOver[--kOverCount];
    return true;
  }
  if (at >= 0) { kOver[at].code = code; return true; }
  if (kOverCount >= ZXJ_MAX_OVERRIDES) return false;
  kOver[kOverCount++] = {hid, code};
  return true;
}

void zxJoymapResetAll()     { kOverCount = 0; }
int  zxJoymapChangedCount() { return kOverCount; }

bool zxJoymapSave()
{
  static const char *const kPath = "/emu8/zx/joymap.txt";
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
        "# emu8 ZX Spectrum joystick map, saved from the ZX menu (CONFIG > JOYSTICK), which rewrites it.\n"
        "# Only the keys changed from the built-in default are listed; the syntax and the whole\n"
        "# default map are in docs/zx-joymap.txt.\n";
      ok = f.write((const uint8_t *)kHead, sizeof(kHead) - 1) == sizeof(kHead) - 1;
      char line[48], name[16], rule[16];
      for (int i = 0; ok && i < kOverCount; i++) {
        c64KeymapHostName(kOver[i].hid, name, sizeof(name));
        zxJoymapDescribe(kOver[i].code, rule, sizeof(rule));
        int len = snprintf(line, sizeof(line), "%s = %s\n", name, rule);
        ok = f.write((const uint8_t *)line, len) == (size_t)len;
      }
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "ZX joymap: %s %s, %d key(s) changed", kPath, ok ? "saved" : "NOT saved", kOverCount);
  printLog(buf);
  return ok;
}

#endif // BOARD_INPUT_USB
