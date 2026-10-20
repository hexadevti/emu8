// atarikeymap.cpp - which Atari 2600 joystick direction / FIRE / console switch each host key
// presses (USB keyboard, PicoCalc keyboard, the desktop build's PC keyboard: everything that
// reaches usbKeyboardReport()).
//
// A built-in default map, plus overrides read from a text file on the SD card when the Atari boots:
// /emu8/atari/keymap.txt (or /atarikeymap.txt at the root). One rule per line, '#' starts a comment:
//
//     <host key> = <action>
//
//     W = UP                       # WASD steers
//     CTRL = FIRE                  # Ctrl fires too
//     F1 = RESET                   # the console's RESET switch
//     X = NONE                     # X does nothing
//
// Host keys are named as in the C64 map (c64keymap.cpp, docs/c64-keymap.txt); the actions are
// UP DOWN LEFT RIGHT, FIRE, SELECT, RESET, or NONE. Any number of keys may press the same one. The Atari has no SHIFT: a key is the same with
// or without it. The full default map is in docs/atari-keymap.txt.
//
// The default: the arrows steer; the PicoCalc's third row gives the buttons: ] FIRE, ` SELECT,
// / RESET.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "atarikeymap.h"
#include "c64keymap.h"

struct AtariRule { uint8_t hid, code; };

// HID usage codes (the standard USB HID keyboard page), numeric so every board's header works.
static const AtariRule kDefault[] = {
  {0x52, ATARIK_UP}, {0x51, ATARIK_DOWN}, {0x50, ATARIK_LEFT}, {0x4F, ATARIK_RIGHT},   // arrows
  {0x30, ATARIK_FIRE}, {0x35, ATARIK_SELECT}, {0x38, ATARIK_RESET},                  // ]  `  /
};

// The picker's order: the stick (UP RIGHT DOWN LEFT), then FIRE and the console switches.
static const struct { uint8_t code; const char *name; } kActions[] = {
  {ATARIK_UP, "UP"}, {ATARIK_RIGHT, "RIGHT"}, {ATARIK_DOWN, "DOWN"}, {ATARIK_LEFT, "LEFT"},
  {ATARIK_FIRE, "FIRE"},
  {ATARIK_SELECT, "SELECT"}, {ATARIK_RESET, "RESET"},
};
#define ATARI_NACT ((int)(sizeof(kActions) / sizeof(kActions[0])))

// Overrides from the SD card. Rules equal to the default are not stored.
#define ATARIK_MAX_OVERRIDES 64
static AtariRule kOver[ATARIK_MAX_OVERRIDES];
static uint8_t kOverCount = 0;

static uint8_t findRule(const AtariRule *t, int n, uint8_t hid)
{
  for (int i = 0; i < n; i++) if (t[i].hid == hid) return t[i].code;
  return ATARIK_NONE;
}

uint8_t atariKeymapDefault(uint8_t hid) { return findRule(kDefault, sizeof(kDefault) / sizeof(kDefault[0]), hid); }

uint8_t atariKeymapLookup(uint8_t hid)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  return c != ATARIK_NONE ? c : atariKeymapDefault(hid);
}

// ---- names ----------------------------------------------------------------------------------
int     atariKeymapActionCount()   { return ATARI_NACT; }
uint8_t atariKeymapActionAt(int i) { return kActions[i % ATARI_NACT].code; }

const char *atariKeymapActionName(uint8_t code)
{
  for (auto &a : kActions) if (a.code == code) return a.name;
  return "?";
}

// Action name -> code, ATARIK_UNMAP for NONE, ATARIK_NONE when unknown.
static uint8_t atariAction(const char *s)
{
  if (!strcmp(s, "NONE")) return ATARIK_UNMAP;
  for (auto &a : kActions) if (!strcmp(s, a.name)) return a.code;
  return ATARIK_NONE;
}

// ---- file parsing ---------------------------------------------------------------------------
static void keymapError(int line, const char *what, const char *tok)
{
  snprintf(buf, sizeof(buf), "Atari keymap line %d: %s '%s' - ignored", line, what, tok);
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
  if (n != 3 || strcmp(tok[1], "=")) { keymapError(line, "expected 'KEY = ACTION', got", tok[0]); return; }
  const uint8_t hid = c64KeymapHostParse(tok[0]);
  if (!hid) { keymapError(line, "unknown host key", tok[0]); return; }
  const uint8_t code = atariAction(tok[2]);
  if (code == ATARIK_NONE) { keymapError(line, "unknown Atari action", tok[2]); return; }

  if (atariKeymapDefault(hid) == code) return;      // same as the built-in rule
  for (int i = 0; i < kOverCount; i++)              // a later line replaces an earlier one
    if (kOver[i].hid == hid) { kOver[i].code = code; return; }
  if (kOverCount >= ATARIK_MAX_OVERRIDES) { keymapError(line, "too many changed keys, at", tok[0]); return; }
  kOver[kOverCount++] = {hid, code};
}

void atariKeymapLoad()
{
  kOverCount = 0;
  static const char *const kPaths[] = { "/emu8/atari/keymap.txt", "/atari/keymap.txt", "/atarikeymap.txt" };
  for (const char *path : kPaths) {
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("Atari keymap: out of heap opening the file"); return; }
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
    snprintf(buf, sizeof(buf), "Atari keymap: %s, %d key(s) changed from the default", path, kOverCount);
    printLog(buf);
    return;
  }
}

// ---- the KEYBOARD page (optionsui.cpp) ------------------------------------------------------
void atariKeymapDescribe(uint8_t code, char *out, size_t n)
{
  if (code == ATARIK_NONE)       snprintf(out, n, "-");
  else if (code == ATARIK_UNMAP) snprintf(out, n, "NONE");
  else                           snprintf(out, n, "%s", atariKeymapActionName(code));
}

uint8_t atariKeymapRule(uint8_t hid, bool *over)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  if (over) *over = c != ATARIK_NONE;
  return c != ATARIK_NONE ? c : atariKeymapDefault(hid);
}

bool atariKeymapSet(uint8_t hid, uint8_t code)
{
  int at = -1;
  for (int i = 0; i < kOverCount; i++) if (kOver[i].hid == hid) at = i;
  if (code == ATARIK_NONE || code == atariKeymapDefault(hid)) {      // back to the default
    if (at >= 0) kOver[at] = kOver[--kOverCount];
    return true;
  }
  if (at >= 0) { kOver[at].code = code; return true; }
  if (kOverCount >= ATARIK_MAX_OVERRIDES) return false;
  kOver[kOverCount++] = {hid, code};
  return true;
}

void atariKeymapResetAll()     { kOverCount = 0; }
int  atariKeymapChangedCount() { return kOverCount; }

bool atariKeymapSave()
{
  static const char *const kPath = "/emu8/atari/keymap.txt";
  // Timed lock, not busTake(): this runs in the render task (the menu).
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  bool ok = false;
  try {
    if (!FSTYPE.exists("/emu8")) FSTYPE.mkdir("/emu8");
    if (!FSTYPE.exists("/emu8/atari")) FSTYPE.mkdir("/emu8/atari");
    if (FSTYPE.exists(kPath)) FSTYPE.remove(kPath);     // FILE_WRITE appends on arduino-pico
    File f = FSTYPE.open(kPath, FILE_WRITE);
    if (f) {
      static const char kHead[] =
        "# emu8 Atari 2600 keyboard map, saved from the Atari menu (CONFIG > KEYBOARD), which rewrites it.\n"
        "# Only the keys changed from the built-in default are listed; the syntax and the whole\n"
        "# default map are in docs/atari-keymap.txt.\n";
      ok = f.write((const uint8_t *)kHead, sizeof(kHead) - 1) == sizeof(kHead) - 1;
      char line[48], name[16], rule[16];
      for (int i = 0; ok && i < kOverCount; i++) {
        c64KeymapHostName(kOver[i].hid, name, sizeof(name));
        atariKeymapDescribe(kOver[i].code, rule, sizeof(rule));
        int len = snprintf(line, sizeof(line), "%s = %s\n", name, rule);
        ok = f.write((const uint8_t *)line, len) == (size_t)len;
      }
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "Atari keymap: %s %s, %d key(s) changed", kPath, ok ? "saved" : "NOT saved", kOverCount);
  printLog(buf);
  return ok;
}

#endif // BOARD_INPUT_USB
