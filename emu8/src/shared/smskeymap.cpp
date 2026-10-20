// smskeymap.cpp - which SMS control pad button each host key presses (USB keyboard, PicoCalc
// keyboard, the desktop build's PC keyboard: everything that reaches usbKeyboardReport()).
//
// A built-in default map, plus overrides read from a text file on the SD card when the SMS boots:
// /emu8/sms/keymap.txt (or /smskeymap.txt at the root). One rule per line, '#' starts a comment:
//
//     <host key> = <SMS button>
//
//     ENTER = 1                    # Enter is button 1 (START) too
//     CTRL = 2                     # so is Ctrl for button 2
//     SPACE = NONE                 # Space does nothing
//
// Host keys are named as in the C64 map (c64keymap.cpp, docs/c64-keymap.txt); the SMS buttons are
// UP DOWN LEFT RIGHT 1 2 (1 is the pad's START), or NONE. Any number of keys may press the same
// button. The pad has no SHIFT: a key is the same button with or without it. The full default map
// is in docs/sms-keymap.txt. PAUSE (F11) and reset (F12) are the console's, not the pad's: not here.
//
// The default: the arrows are the D-pad; the PicoCalc's third row gives the buttons: [ 1, ] 2.

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "smskeymap.h"
#include "c64keymap.h"

static const char *const SMS_NAMES[6] = { "UP", "DOWN", "LEFT", "RIGHT", "1", "2" };

struct SmsRule { uint8_t hid, code; };

// HID usage codes (the standard USB HID keyboard page), numeric so every board's header works.
static const SmsRule kDefault[] = {
  {0x52, SMSK_UP}, {0x51, SMSK_DOWN}, {0x50, SMSK_LEFT}, {0x4F, SMSK_RIGHT},   // arrows
  {0x2F, SMSK_1}, {0x30, SMSK_2},                                              // [  ]
};

// Overrides from the SD card. Rules equal to the default are not stored.
#define SMSK_MAX_OVERRIDES 64
static SmsRule kOver[SMSK_MAX_OVERRIDES];
static uint8_t kOverCount = 0;

static uint8_t findRule(const SmsRule *t, int n, uint8_t hid)
{
  for (int i = 0; i < n; i++) if (t[i].hid == hid) return t[i].code;
  return SMSK_NONE;
}

uint8_t smsKeymapDefault(uint8_t hid) { return findRule(kDefault, sizeof(kDefault) / sizeof(kDefault[0]), hid); }

uint8_t smsKeymapLookup(uint8_t hid)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  return c != SMSK_NONE ? c : smsKeymapDefault(hid);
}

// ---- file parsing ---------------------------------------------------------------------------
// SMS button name -> code, SMSK_UNMAP for NONE, SMSK_NONE when unknown. START / BUTTON1 / BUTTON2
// are taken for 1 / 2 too.
static uint8_t smsButton(const char *s)
{
  if (!strcmp(s, "NONE")) return SMSK_UNMAP;
  if (!strcmp(s, "START") || !strcmp(s, "BUTTON1")) return SMSK_1;
  if (!strcmp(s, "BUTTON2")) return SMSK_2;
  for (int i = 0; i < 6; i++) if (!strcmp(s, SMS_NAMES[i])) return (uint8_t)i;
  return SMSK_NONE;
}

static void keymapError(int line, const char *what, const char *tok)
{
  snprintf(buf, sizeof(buf), "SMS keymap line %d: %s '%s' - ignored", line, what, tok);
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
  const uint8_t code = smsButton(tok[2]);
  if (code == SMSK_NONE) { keymapError(line, "unknown SMS button", tok[2]); return; }

  if (smsKeymapDefault(hid) == code) return;        // same as the built-in rule
  for (int i = 0; i < kOverCount; i++)              // a later line replaces an earlier one
    if (kOver[i].hid == hid) { kOver[i].code = code; return; }
  if (kOverCount >= SMSK_MAX_OVERRIDES) { keymapError(line, "too many changed keys, at", tok[0]); return; }
  kOver[kOverCount++] = {hid, code};
}

void smsKeymapLoad()
{
  kOverCount = 0;
  static const char *const kPaths[] = { "/emu8/sms/keymap.txt", "/sms/keymap.txt", "/smskeymap.txt" };
  for (const char *path : kPaths) {
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("SMS keymap: out of heap opening the file"); return; }
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
    snprintf(buf, sizeof(buf), "SMS keymap: %s, %d key(s) changed from the default", path, kOverCount);
    printLog(buf);
    return;
  }
}

// ---- the KEYBOARD page (optionsui.cpp) ------------------------------------------------------
const char *smsKeymapButtonName(uint8_t b) { return b < 6 ? SMS_NAMES[b] : "?"; }

uint8_t smsKeymapButtonAt(int i)
{
  static const uint8_t kPick[6] = { SMSK_UP, SMSK_DOWN, SMSK_LEFT, SMSK_RIGHT, SMSK_1, SMSK_2 };
  return kPick[(unsigned)i < 6 ? i : 0];
}

void smsKeymapDescribe(uint8_t code, char *out, size_t n)
{
  if (code == SMSK_NONE)       snprintf(out, n, "-");
  else if (code == SMSK_UNMAP) snprintf(out, n, "NONE");
  else                         snprintf(out, n, "%s", smsKeymapButtonName(code));
}

uint8_t smsKeymapRule(uint8_t hid, bool *over)
{
  const uint8_t c = findRule(kOver, kOverCount, hid);
  if (over) *over = c != SMSK_NONE;
  return c != SMSK_NONE ? c : smsKeymapDefault(hid);
}

bool smsKeymapSet(uint8_t hid, uint8_t code)
{
  int at = -1;
  for (int i = 0; i < kOverCount; i++) if (kOver[i].hid == hid) at = i;
  if (code == SMSK_NONE || code == smsKeymapDefault(hid)) {          // back to the default
    if (at >= 0) kOver[at] = kOver[--kOverCount];
    return true;
  }
  if (at >= 0) { kOver[at].code = code; return true; }
  if (kOverCount >= SMSK_MAX_OVERRIDES) return false;
  kOver[kOverCount++] = {hid, code};
  return true;
}

void smsKeymapResetAll()     { kOverCount = 0; }
int  smsKeymapChangedCount() { return kOverCount; }

bool smsKeymapSave()
{
  static const char *const kPath = "/emu8/sms/keymap.txt";
  // Timed lock, not busTake(): this runs in the render task (the menu).
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  bool ok = false;
  try {
    if (!FSTYPE.exists("/emu8")) FSTYPE.mkdir("/emu8");
    if (!FSTYPE.exists("/emu8/sms")) FSTYPE.mkdir("/emu8/sms");
    if (FSTYPE.exists(kPath)) FSTYPE.remove(kPath);     // FILE_WRITE appends on arduino-pico
    File f = FSTYPE.open(kPath, FILE_WRITE);
    if (f) {
      static const char kHead[] =
        "# emu8 SMS keyboard map, saved from the SMS menu (CONFIG > KEYBOARD), which rewrites it.\n"
        "# Only the keys changed from the built-in default are listed; the syntax and the whole\n"
        "# default map are in docs/sms-keymap.txt.\n";
      ok = f.write((const uint8_t *)kHead, sizeof(kHead) - 1) == sizeof(kHead) - 1;
      char line[48], name[16], rule[16];
      for (int i = 0; ok && i < kOverCount; i++) {
        c64KeymapHostName(kOver[i].hid, name, sizeof(name));
        smsKeymapDescribe(kOver[i].code, rule, sizeof(rule));
        int len = snprintf(line, sizeof(line), "%s = %s\n", name, rule);
        ok = f.write((const uint8_t *)line, len) == (size_t)len;
      }
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "SMS keymap: %s %s, %d key(s) changed", kPath, ok ? "saved" : "NOT saved", kOverCount);
  printLog(buf);
  return ok;
}

#endif // BOARD_INPUT_USB
