// apple2joymap.cpp - what each host key does to the Apple II joystick (USB keyboard, PicoCalc
// keyboard, the desktop build's PC keyboard: everything that reaches usbKeyboardReport()).
//
// A built-in default map, plus overrides read from a text file on the SD card when the Apple boots:
// /emu8/apple2/joymap.txt (or /apple2joymap.txt at the root). One rule per line, '#' starts a comment:
//
//     <host key> = <joystick action>
//
//     W = UP                       # WASD steers
//     CTRL = BUTTON1               # Ctrl is button 1
//     SPACE = NONE                 # Space only types
//
// Host keys are named as in the Apple keyboard map (apple2keymap.cpp), plus SHIFT (either one). The
// actions are UP DOWN LEFT RIGHT, BUTTON0 (open-apple, PB0), BUTTON1 (solid-apple, PB1), or NONE. Any number of keys may do the same thing.
// The stick is read where the Apple options say (JOYSTICK = JOY / BOTH); the buttons always are.
// The full default map is in docs/apple2-joymap.txt.
//
// The default: the arrows steer; the PicoCalc's third row gives the buttons: ] button 0, [ button 1.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "apple2joymap.h"
#include "apple2keymap.h"
#include "c64keymap.h"

struct JoyRule { uint8_t hid, code; };

// HID usage codes (the standard USB HID keyboard page), numeric so every board's header works.
static const JoyRule kDefault[] = {
  {0x52, A2J_UP}, {0x51, A2J_DOWN}, {0x50, A2J_LEFT}, {0x4F, A2J_RIGHT},   // arrows
  {0x30, A2J_BTN0}, {0x2F, A2J_BTN1},                                     // ]  [
};

// The picker's order: the stick (UP RIGHT DOWN LEFT), then the buttons.
static const struct { uint8_t code; const char *name; } kActions[] = {
  {A2J_UP, "UP"}, {A2J_RIGHT, "RIGHT"}, {A2J_DOWN, "DOWN"}, {A2J_LEFT, "LEFT"},
  {A2J_BTN0, "BUTTON0"}, {A2J_BTN1, "BUTTON1"},
};
#define JOY_NACT ((int)(sizeof(kActions) / sizeof(kActions[0])))

// Overrides from the SD card. Rules equal to the default are not stored.
#define A2J_MAX_OVERRIDES 32
static JoyRule kOver[A2J_MAX_OVERRIDES];
static uint8_t kOverCount = 0;

static uint8_t findRule(const JoyRule *t, int n, uint8_t hid)
{
  for (int i = 0; i < n; i++) if (t[i].hid == hid) return t[i].code;
  return A2J_NONE;
}

uint8_t apple2JoymapDefault(uint8_t hid) { return findRule(kDefault, sizeof(kDefault) / sizeof(kDefault[0]), hid); }

uint8_t apple2JoymapLookup(uint8_t hid)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  return c != A2J_NONE ? c : apple2JoymapDefault(hid);
}

// ---- names ----------------------------------------------------------------------------------
static uint8_t hostKey(const char *s)
{
  if (!strcmp(s, "SHIFT")) return C64K_HID_SHIFT;
  return apple2KeymapHostParse(s);
}

void apple2JoymapHostName(uint8_t hid, char *out, size_t n)
{
  if (hid == C64K_HID_SHIFT) snprintf(out, n, "SHIFT");
  else apple2KeymapHostName(hid, out, n);
}

int     apple2JoymapHostCount()   { return apple2KeymapHostCount() + 1; }
uint8_t apple2JoymapHostAt(int i) { return i < apple2KeymapHostCount() ? apple2KeymapHostAt(i) : (uint8_t)C64K_HID_SHIFT; }

int     apple2JoymapActionCount()   { return JOY_NACT; }
uint8_t apple2JoymapActionAt(int i) { return kActions[i % JOY_NACT].code; }

const char *apple2JoymapActionName(uint8_t code)
{
  for (auto &a : kActions) if (a.code == code) return a.name;
  return "?";
}

// Action name -> code, A2J_UNMAP for NONE, A2J_NONE when unknown.
static uint8_t joyAction(const char *s)
{
  if (!strcmp(s, "NONE")) return A2J_UNMAP;
  if (!strcmp(s, "PB0") || !strcmp(s, "BUTTON0") || !strcmp(s, "OPEN-APPLE"))  return A2J_BTN0;
  if (!strcmp(s, "PB1") || !strcmp(s, "BUTTON1") || !strcmp(s, "SOLID-APPLE")) return A2J_BTN1;
  for (auto &a : kActions) if (!strcmp(s, a.name)) return a.code;
  return A2J_NONE;
}

// ---- file parsing ---------------------------------------------------------------------------
static void joymapError(int line, const char *what, const char *tok)
{
  snprintf(buf, sizeof(buf), "Apple joymap line %d: %s '%s' - ignored", line, what, tok);
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
  if (code == A2J_NONE) { joymapError(line, "unknown joystick action", tok[2]); return; }

  if (apple2JoymapDefault(hid) == code) return;     // same as the built-in rule
  for (int i = 0; i < kOverCount; i++)              // a later line replaces an earlier one
    if (kOver[i].hid == hid) { kOver[i].code = code; return; }
  if (kOverCount >= A2J_MAX_OVERRIDES) { joymapError(line, "too many changed keys, at", tok[0]); return; }
  kOver[kOverCount++] = {hid, code};
}

void apple2JoymapLoad()
{
  kOverCount = 0;
  static const char *const kPaths[] = { "/emu8/apple2/joymap.txt", "/apple2/joymap.txt", "/apple2joymap.txt" };
  for (const char *path : kPaths) {
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("Apple joymap: out of heap opening the file"); return; }
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
    snprintf(buf, sizeof(buf), "Apple joymap: %s, %d key(s) changed from the default", path, kOverCount);
    printLog(buf);
    return;
  }
}

// ---- the JOYSTICK page (optionsui.cpp) ------------------------------------------------------
void apple2JoymapDescribe(uint8_t code, char *out, size_t n)
{
  if (code == A2J_NONE)       snprintf(out, n, "-");
  else if (code == A2J_UNMAP) snprintf(out, n, "NONE");
  else                        snprintf(out, n, "%s", apple2JoymapActionName(code));
}

uint8_t apple2JoymapRule(uint8_t hid, bool *over)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  if (over) *over = c != A2J_NONE;
  return c != A2J_NONE ? c : apple2JoymapDefault(hid);
}

bool apple2JoymapSet(uint8_t hid, uint8_t code)
{
  int at = -1;
  for (int i = 0; i < kOverCount; i++) if (kOver[i].hid == hid) at = i;
  if (code == A2J_NONE || code == apple2JoymapDefault(hid)) {     // back to the default
    if (at >= 0) kOver[at] = kOver[--kOverCount];
    return true;
  }
  if (at >= 0) { kOver[at].code = code; return true; }
  if (kOverCount >= A2J_MAX_OVERRIDES) return false;
  kOver[kOverCount++] = {hid, code};
  return true;
}

void apple2JoymapResetAll()     { kOverCount = 0; }
int  apple2JoymapChangedCount() { return kOverCount; }

bool apple2JoymapSave()
{
  static const char *const kPath = "/emu8/apple2/joymap.txt";
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
        "# emu8 Apple II joystick map, saved from the Apple menu (CONFIG > JOYSTICK), which rewrites it.\n"
        "# Only the keys changed from the built-in default are listed; the syntax and the whole\n"
        "# default map are in docs/apple2-joymap.txt.\n";
      ok = f.write((const uint8_t *)kHead, sizeof(kHead) - 1) == sizeof(kHead) - 1;
      char line[48], name[16], rule[16];
      for (int i = 0; ok && i < kOverCount; i++) {
        apple2JoymapHostName(kOver[i].hid, name, sizeof(name));
        apple2JoymapDescribe(kOver[i].code, rule, sizeof(rule));
        int len = snprintf(line, sizeof(line), "%s = %s\n", name, rule);
        ok = f.write((const uint8_t *)line, len) == (size_t)len;
      }
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "Apple joymap: %s %s, %d key(s) changed", kPath, ok ? "saved" : "NOT saved", kOverCount);
  printLog(buf);
  return ok;
}

#endif // BOARD_INPUT_USB
