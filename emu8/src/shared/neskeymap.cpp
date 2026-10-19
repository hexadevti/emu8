// neskeymap.cpp - which NES controller button each host key presses (USB keyboard, PicoCalc
// keyboard, the desktop build's PC keyboard: everything that reaches usbKeyboardReport()).
//
// A built-in default map, plus overrides read from a text file on the SD card when the NES boots:
// /nes/keymap.txt (or /neskeymap.txt at the root). One rule per line, '#' starts a comment:
//
//     <host key> = <NES button>
//
//     SPACE = A                    # Space is A too
//     CTRL = B                     # so is Ctrl for B
//     F4 = NONE                    # F4 does nothing
//
// Host keys are named as in the C64 map (c64keymap.cpp, docs/c64-keymap.txt); the NES buttons are
// UP DOWN LEFT RIGHT SELECT START B A, or NONE. Any number of keys may press the same button.
// The NES has no SHIFT: a key is the same button with or without it. The full default map is in
// docs/nes-keymap.txt.
//
// The default: the arrows are the D-pad, X / Z are A / B, Enter / Tab are Start / Select, and the
// function row (the PicoCalc's buttons) F2 = Start, F3 = Select, F4 = A, F5 = B.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "neskeymap.h"
#include "c64keymap.h"

static const char *const NES_NAMES[8] = { "A", "B", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT" };

struct NesRule { uint8_t hid, code; };

// HID usage codes (the standard USB HID keyboard page), numeric so every board's header works.
static const NesRule kDefault[] = {
  {0x52, NESK_UP}, {0x51, NESK_DOWN}, {0x50, NESK_LEFT}, {0x4F, NESK_RIGHT},   // arrows
  {0x1B, NESK_A}, {0x1D, NESK_B},                                              // X  Z
  {0x28, NESK_START}, {0x2B, NESK_SELECT},                                     // Enter  Tab
  // the function row: the PicoCalc's bare F3 arrives as F12 (input_picocalc.cpp)
  {0x3B, NESK_START},                                                          // F2
#if UI_PICOCALC
  {0x45, NESK_SELECT},                                                         // F12 (= F3)
#else
  {0x3C, NESK_SELECT},                                                         // F3
#endif
  {0x3D, NESK_A}, {0x3E, NESK_B},                                              // F4  F5
};

// Overrides from the SD card. Rules equal to the default are not stored.
#define NESK_MAX_OVERRIDES 64
static NesRule kOver[NESK_MAX_OVERRIDES];
static uint8_t kOverCount = 0;

static uint8_t findRule(const NesRule *t, int n, uint8_t hid)
{
  for (int i = 0; i < n; i++) if (t[i].hid == hid) return t[i].code;
  return NESK_NONE;
}

uint8_t nesKeymapDefault(uint8_t hid) { return findRule(kDefault, sizeof(kDefault) / sizeof(kDefault[0]), hid); }

uint8_t nesKeymapLookup(uint8_t hid)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  return c != NESK_NONE ? c : nesKeymapDefault(hid);
}

// ---- file parsing ---------------------------------------------------------------------------
// NES button name -> code, NESK_UNMAP for NONE, NESK_NONE when unknown.
static uint8_t nesButton(const char *s)
{
  if (!strcmp(s, "NONE")) return NESK_UNMAP;
  for (int i = 0; i < 8; i++) if (!strcmp(s, NES_NAMES[i])) return (uint8_t)i;
  return NESK_NONE;
}

static void keymapError(int line, const char *what, const char *tok)
{
  snprintf(buf, sizeof(buf), "NES keymap line %d: %s '%s' - ignored", line, what, tok);
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
  const uint8_t hid = c64KeymapHostParse(tok[0]);
  if (!hid) { keymapError(line, "unknown host key", tok[0]); return; }
  const uint8_t code = nesButton(tok[2]);
  if (code == NESK_NONE) { keymapError(line, "unknown NES button", tok[2]); return; }

  if (nesKeymapDefault(hid) == code) return;        // same as the built-in rule
  for (int i = 0; i < kOverCount; i++)              // a later line replaces an earlier one
    if (kOver[i].hid == hid) { kOver[i].code = code; return; }
  if (kOverCount >= NESK_MAX_OVERRIDES) { keymapError(line, "too many changed keys, at", tok[0]); return; }
  kOver[kOverCount++] = {hid, code};
}

void nesKeymapLoad()
{
  kOverCount = 0;
  static const char *const kPaths[] = { "/nes/keymap.txt", "/neskeymap.txt" };
  for (const char *path : kPaths) {
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("NES keymap: out of heap opening the file"); return; }
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
    snprintf(buf, sizeof(buf), "NES keymap: %s, %d key(s) changed from the default", path, kOverCount);
    printLog(buf);
    return;
  }
}

// ---- the KEYBOARD page (optionsui.cpp) ------------------------------------------------------
const char *nesKeymapButtonName(uint8_t b) { return NES_NAMES[b & 7]; }

uint8_t nesKeymapButtonAt(int i)
{
  static const uint8_t kPick[8] = { NESK_UP, NESK_DOWN, NESK_LEFT, NESK_RIGHT, NESK_SELECT, NESK_START, NESK_B, NESK_A };
  return kPick[i & 7];
}

void nesKeymapDescribe(uint8_t code, char *out, size_t n)
{
  if (code == NESK_NONE)       snprintf(out, n, "-");
  else if (code == NESK_UNMAP) snprintf(out, n, "NONE");
  else                         snprintf(out, n, "%s", NES_NAMES[code & 7]);
}

uint8_t nesKeymapRule(uint8_t hid, bool *over)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  if (over) *over = c != NESK_NONE;
  return c != NESK_NONE ? c : nesKeymapDefault(hid);
}

bool nesKeymapSet(uint8_t hid, uint8_t code)
{
  int at = -1;
  for (int i = 0; i < kOverCount; i++) if (kOver[i].hid == hid) at = i;
  if (code == NESK_NONE || code == nesKeymapDefault(hid)) {          // back to the default
    if (at >= 0) kOver[at] = kOver[--kOverCount];
    return true;
  }
  if (at >= 0) { kOver[at].code = code; return true; }
  if (kOverCount >= NESK_MAX_OVERRIDES) return false;
  kOver[kOverCount++] = {hid, code};
  return true;
}

void nesKeymapResetAll()     { kOverCount = 0; }
int  nesKeymapChangedCount() { return kOverCount; }

bool nesKeymapSave()
{
  static const char *const kPath = "/nes/keymap.txt";
  // Timed lock, not busTake(): this runs in the render task (the menu).
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  bool ok = false;
  try {
    if (!FSTYPE.exists("/nes")) FSTYPE.mkdir("/nes");
    if (FSTYPE.exists(kPath)) FSTYPE.remove(kPath);     // FILE_WRITE appends on arduino-pico
    File f = FSTYPE.open(kPath, FILE_WRITE);
    if (f) {
      static const char kHead[] =
        "# emu8 NES keyboard map, saved from the NES menu (CONFIG > KEYBOARD), which rewrites it.\n"
        "# Only the keys changed from the built-in default are listed; the syntax and the whole\n"
        "# default map are in docs/nes-keymap.txt.\n";
      ok = f.write((const uint8_t *)kHead, sizeof(kHead) - 1) == sizeof(kHead) - 1;
      char line[48], name[16], rule[16];
      for (int i = 0; ok && i < kOverCount; i++) {
        c64KeymapHostName(kOver[i].hid, name, sizeof(name));
        nesKeymapDescribe(kOver[i].code, rule, sizeof(rule));
        int len = snprintf(line, sizeof(line), "%s = %s\n", name, rule);
        ok = f.write((const uint8_t *)line, len) == (size_t)len;
      }
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "NES keymap: %s %s, %d key(s) changed", kPath, ok ? "saved" : "NOT saved", kOverCount);
  printLog(buf);
  return ok;
}

#endif // BOARD_INPUT_USB
