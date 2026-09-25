// input_picocalc.cpp - ClockworkPi PicoCalc keyboard input.
//
// The PicoCalc has no USB host and no touchscreen. Its QWERTY keyboard is a separate STM32F103
// that exposes a small register file over i2c1 (SDA=6, SCL=7, 10 kHz, addr 0x1F). We poll its
// event FIFO, translate each event into a USB-HID usage code plus a modifier byte, and hand it
// to the UNCHANGED usbKeyboardReport() dispatcher in src/shared/usbkeyboard.cpp -- which already
// implements every core's keyboard behaviour (Apple II keymem, C64/MSX matrices, NES/Atari/SMS
// pads, the settings menu and CPU reset). Modelled on src/desktop/input_sdl.cpp.
//
// What this file does NOT define:
//   * keyboard_read/keyboardStrobe/keyboardSetup/keyboard_bit -- unlike desktop (where CMake
//     excludes keyboardPs2.cpp), that file IS compiled here and already no-ops itself because
//     KEYBOARD_DATA_PIN/KEYBOARD_IRQ_PIN are -1 in board.h.
// What it DOES define, because their owning files are compiled out on this board:
//   * usbGamepadSetup()  -- usbgamepad.cpp needs EspUsbHost (no USB host here).
//   * the osk*/touchRead surface -- touchkeyboard.cpp is compiled out (no touch panel), which
//     also gives back its ~30 KB of layout tables.
//
// --- STM32 keyboard protocol (clockworkpi/PicoCalc, Code/picocalc_kbd_tester/i2ckbd) ---
// Write one byte 0x09 (REG_ID_FIF) to select the FIFO register, then read two bytes back:
//   buf[0] = key state (0 idle, 1 pressed, 2 held, 3 released)
//   buf[1] = PicoCalc key code
// 0x0000 means the FIFO is empty. The slave needs a turnaround gap between the write and the
// read -- ClockworkPis own drivers all sleep 5..16 ms there, which is far too long to block a
// render frame for. So the poll is SPLIT-PHASE: the register write is issued at the end of one
// pump and the result read at the start of the next, one frame (>= 16 ms at any playable frame
// rate) later. That costs zero blocking time and yields one FIFO event per frame; the STM32s
// 31-entry FIFO absorbs bursts, and its own auto-repeat drives held keys.
//
// --- Shift, caps lock and ctrl ---
// The STM32 firmware ships with CFG_USE_MODS | CFG_REPORT_MODS, so it BOTH transforms the
// character by shift/caps-lock/alt AND reports the modifier keys as their own events. Ctrl is
// the exception: it is reported but never alters the character. Therefore:
//   * the shift bit is derived from the CHARACTER (a already means "no shift", A means "shift"),
//     which is the only thing that gets caps lock right, ORed with the physical shift state so
//     the C64/MSX continuous-modifier paths see a held shift;
//   * ctrl/alt come from the modifier events.
// A key's BASE HID code is shift-invariant (a and A are both 0x04, 1 and ! are both 0x1E), so
// releasing by base code stays correct even if shift is let go before the key.
//
// --- Function keys ---
// The machine-level actions all sit behind Ctrl so that a bare function key stays available to
// the emulated machine and cannot be hit by accident mid-game:
//   Ctrl-F1                  -> HID_KEY_F10  open / close the settings menu
//   Ctrl-F3                  -> HID_KEY_F11  CPU reset (Apple II / IIGS); PAUSE/NMI on SMS
//   Ctrl-F6 / Ctrl-Shift-F3  -> reboot into the system selection menu (the boot splash)
//   Ctrl-Shift-F1            -> reboot the Pico itself (see the note at the handler)
// Bare keys pass through:
//   F1, F2 -> HID_KEY_F1, HID_KEY_F2
//   F3     -> HID_KEY_F12    hard reset (SMS / PC-XT / tiny386) -- unchanged
//   F4, F5 -> HID_KEY_F4, HID_KEY_F5   Apple II paddle buttons 0 and 1; MSX matrix keys
// F6..F10 are translated too in case a unit's firmware emits them; the stock PicoCalc keyboard
// only sends F1..F5, which is why Ctrl-Shift-F3 exists as an alias for Ctrl-F6.
// usbkeyboard.cpp is untouched; the remap happens here at translation time.

#include "../../emu.h"

#if defined(BOARD_PICOCALC)

#include <Wire.h>
#include <string.h>
#include "EspUsbHost.h"   // the shim in pico_shim/: HID_KEY_* usage codes + KEYBOARD_MODIFIER_*

// ---------------------------------------------------------------------------------------------
// Poll tuning. Strict split-phase (one event per pump) blocks for exactly 0 us, so it is the
// default. If hardware bring-up shows dropped keys under fast typing, raise PCKBD_DRAIN_EXTRA:
// each extra event costs one PCKBD_TURNAROUND_US busy-wait, and only while events are actually
// flowing (the drain stops at the first empty read).
// ---------------------------------------------------------------------------------------------
#ifndef PCKBD_DRAIN_EXTRA
#define PCKBD_DRAIN_EXTRA   0
#endif
#ifndef PCKBD_TURNAROUND_US
#define PCKBD_TURNAROUND_US 2000
#endif

#define KBD_REG_FIF 0x09          // FIFO register: write to select, then read 2 bytes

// PicoCalc key codes (Code/picocalc_kbd_tester/keyboard_define.h). Codes below 0x80 are the
// already shift/caps-resolved ASCII character.
enum {
  PCK_BACKSPACE = 0x08, PCK_TAB = 0x09, PCK_ENTER = 0x0A,
  PCK_F1 = 0x81, PCK_F2 = 0x82, PCK_F3 = 0x83, PCK_F4 = 0x84, PCK_F5 = 0x85,
  PCK_F6 = 0x86, PCK_F7 = 0x87, PCK_F8 = 0x88, PCK_F9 = 0x89, PCK_F10 = 0x8A,
  PCK_MOD_ALT = 0xA1, PCK_MOD_SHL = 0xA2, PCK_MOD_SHR = 0xA3,
  PCK_MOD_SYM = 0xA4, PCK_MOD_CTRL = 0xA5,
  PCK_ESC = 0xB1,
  PCK_LEFT = 0xB4, PCK_UP = 0xB5, PCK_DOWN = 0xB6, PCK_RIGHT = 0xB7,
  PCK_CAPS_LOCK = 0xC1,
  PCK_BREAK = 0xD0, PCK_INSERT = 0xD1, PCK_HOME = 0xD2, PCK_DEL = 0xD4,
  PCK_END = 0xD5, PCK_PAGE_UP = 0xD6, PCK_PAGE_DOWN = 0xD7,
};

enum { PCK_STATE_IDLE = 0, PCK_STATE_PRESSED = 1, PCK_STATE_HOLD = 2, PCK_STATE_RELEASED = 3 };

// ---------------------------------------------------------------------------------------------
// key code -> HID usage code (+ whether the character implies shift)
// ---------------------------------------------------------------------------------------------
struct HidKey { uint8_t hid; bool shift; };

// Printable US-ASCII -> its base HID usage code. Shift is reported separately so that the base
// code stays identical for a key and its shifted twin (needed for release matching).
static HidKey asciiToHid(uint8_t c)
{
  if (c >= 'a' && c <= 'z') return { (uint8_t)(HID_KEY_A + (c - 'a')), false };
  if (c >= 'A' && c <= 'Z') return { (uint8_t)(HID_KEY_A + (c - 'A')), true  };
  if (c >= '1' && c <= '9') return { (uint8_t)(HID_KEY_1 + (c - '1')), false };
  if (c == '0')             return { HID_KEY_0, false };
  switch (c) {
    case ' ':  return { HID_KEY_SPACE,         false };
    case '!':  return { HID_KEY_1,             true  };
    case '@':  return { HID_KEY_2,             true  };
    case '#':  return { HID_KEY_3,             true  };
    case '$':  return { HID_KEY_4,             true  };
    case '%':  return { HID_KEY_5,             true  };
    case '^':  return { HID_KEY_6,             true  };
    case '&':  return { HID_KEY_7,             true  };
    case '*':  return { HID_KEY_8,             true  };
    case '(':  return { HID_KEY_9,             true  };
    case ')':  return { HID_KEY_0,             true  };
    case '-':  return { HID_KEY_MINUS,         false };
    case '_':  return { HID_KEY_MINUS,         true  };
    case '=':  return { HID_KEY_EQUAL,         false };
    case '+':  return { HID_KEY_EQUAL,         true  };
    case '[':  return { HID_KEY_BRACKET_LEFT,  false };
    case '{':  return { HID_KEY_BRACKET_LEFT,  true  };
    case ']':  return { HID_KEY_BRACKET_RIGHT, false };
    case '}':  return { HID_KEY_BRACKET_RIGHT, true  };
    case '\\': return { HID_KEY_BACKSLASH,     false };
    case '|':  return { HID_KEY_BACKSLASH,     true  };
    case ';':  return { HID_KEY_SEMICOLON,     false };
    case ':':  return { HID_KEY_SEMICOLON,     true  };
    case '\'': return { HID_KEY_APOSTROPHE,    false };
    case '"':  return { HID_KEY_APOSTROPHE,    true  };
    case '`':  return { HID_KEY_GRAVE,         false };
    case '~':  return { HID_KEY_GRAVE,         true  };
    case ',':  return { HID_KEY_COMMA,         false };
    case '<':  return { HID_KEY_COMMA,         true  };
    case '.':  return { HID_KEY_PERIOD,        false };
    case '>':  return { HID_KEY_PERIOD,        true  };
    case '/':  return { HID_KEY_SLASH,         false };
    case '?':  return { HID_KEY_SLASH,         true  };
  }
  return { HID_KEY_NONE, false };
}

// Full translation. Returns HID_KEY_NONE for codes we deliberately swallow (caps lock is handled
// inside the STM32 firmware; the modifier codes are handled by the caller before this is reached).
static HidKey pcToHid(uint8_t code)
{
  switch (code) {
    case PCK_BACKSPACE: return { HID_KEY_BACKSPACE,   false };
    case PCK_TAB:       return { HID_KEY_TAB,         false };
    case PCK_ENTER:
    case '\r':          return { HID_KEY_ENTER,       false };
    case PCK_ESC:       return { HID_KEY_ESCAPE,      false };
    case PCK_LEFT:      return { HID_KEY_ARROW_LEFT,  false };
    case PCK_UP:        return { HID_KEY_ARROW_UP,    false };
    case PCK_DOWN:      return { HID_KEY_ARROW_DOWN,  false };
    case PCK_RIGHT:     return { HID_KEY_ARROW_RIGHT, false };
    // function keys (see the header comment). The machine-level actions are Ctrl combinations
    // handled in handleEvent() before this is reached, so these are the bare-key meanings.
    case PCK_F1:        return { HID_KEY_F1,          false };
    case PCK_F2:        return { HID_KEY_F2,          false };
    case PCK_F3:        return { HID_KEY_F12,         false };   // SMS / PC-XT / tiny386 hard reset
    case PCK_F4:        return { HID_KEY_F4,          false };   // Apple II button 0 (open-apple)
    case PCK_F5:        return { HID_KEY_F5,          false };   // Apple II button 1 (solid-apple)
    case PCK_F6:        return { HID_KEY_F6,          false };
    case PCK_F7:        return { HID_KEY_F7,          false };
    case PCK_F8:        return { HID_KEY_F8,          false };
    case PCK_F9:        return { HID_KEY_F9,          false };
    case PCK_BREAK:     return { HID_KEY_PAUSE,       false };
    case PCK_INSERT:    return { HID_KEY_INSERT,      false };
    case PCK_HOME:      return { HID_KEY_HOME,        false };
    case PCK_DEL:       return { HID_KEY_DELETE,      false };
    case PCK_END:       return { HID_KEY_END,         false };
    case PCK_PAGE_UP:   return { HID_KEY_PAGE_UP,     false };
    case PCK_PAGE_DOWN: return { HID_KEY_PAGE_DOWN,   false };
    case PCK_CAPS_LOCK: return { HID_KEY_NONE,        false };   // resolved by the STM32 already
    default: break;
  }
  if (code < 0x80) return asciiToHid(code);
  return { HID_KEY_NONE, false };
}

// ---------------------------------------------------------------------------------------------
// live key state -> HID boot report
// ---------------------------------------------------------------------------------------------
static uint8_t g_keys[6]       = {0, 0, 0, 0, 0, 0};   // currently-down non-modifier HID codes
static bool    g_keyShift[6]   = {false, false, false, false, false, false};
static uint8_t g_lastReport[6] = {0, 0, 0, 0, 0, 0};
static bool    g_shiftHeld = false, g_ctrlHeld = false, g_altHeld = false, g_symHeld = false;

static void pressAdd(uint8_t hid, bool shift)
{
  if (!hid || hid >= 0xE0) return;                 // modifiers live in the modifier byte
  for (int i = 0; i < 6; i++)
    if (g_keys[i] == hid) { g_keyShift[i] = shift; return; }
  for (int i = 0; i < 6; i++)
    if (!g_keys[i]) { g_keys[i] = hid; g_keyShift[i] = shift; return; }
}

static void pressRemove(uint8_t hid)
{
  for (int i = 0; i < 6; i++)
    if (g_keys[i] == hid) { g_keys[i] = 0; g_keyShift[i] = false; return; }
}

static uint8_t hidModifiers()
{
  bool shift = g_shiftHeld;
  for (int i = 0; i < 6; i++) if (g_keys[i] && g_keyShift[i]) shift = true;
  uint8_t mod = 0;
  if (shift)      mod |= KEYBOARD_MODIFIER_LEFTSHIFT;
  if (g_ctrlHeld) mod |= KEYBOARD_MODIFIER_LEFTCTRL;
  if (g_altHeld)  mod |= KEYBOARD_MODIFIER_LEFTALT;    // Apple II open-apple  (paddle button 0)
  if (g_symHeld)  mod |= KEYBOARD_MODIFIER_RIGHTALT;   // Apple II solid-apple (paddle button 1)
  return mod;
}

static void sendKeyboardReport()
{
  // Compact the sparse slot array into the dense keys[6] the boot report expects.
  uint8_t keys[6] = {0, 0, 0, 0, 0, 0};
  int n = 0;
  for (int i = 0; i < 6 && n < 6; i++) if (g_keys[i]) keys[n++] = g_keys[i];
  usbKeyboardReport(hidModifiers(), keys, g_lastReport);
  memcpy(g_lastReport, keys, 6);
}

// Synthesise a complete press+release of one HID code. Used by the Ctrl hotkeys so that the
// action they trigger goes through the normal usbKeyboardReport() dispatch -- which already
// knows that F11 means cpuReset on an Apple II but PAUSE/NMI on an SMS -- instead of this file
// having to duplicate that per-platform knowledge.
static void tapHid(uint8_t hid)
{
  pressAdd(hid, false);
  sendKeyboardReport();
  pressRemove(hid);
  sendKeyboardReport();
}

// Returns true if the code was a modifier (and therefore fully handled here).
static bool applyModifier(uint8_t code, bool down)
{
  switch (code) {
    case PCK_MOD_SHL:
    case PCK_MOD_SHR:  g_shiftHeld = down; return true;
    case PCK_MOD_CTRL: g_ctrlHeld  = down; return true;
    case PCK_MOD_ALT:  g_altHeld   = down; return true;
    case PCK_MOD_SYM:  g_symHeld   = down; return true;
    default: return false;
  }
}

static void handleEvent(uint8_t state, uint8_t code)
{
  bool down;
  switch (state) {
    case PCK_STATE_PRESSED:
    case PCK_STATE_HOLD:     down = true;  break;
    case PCK_STATE_RELEASED: down = false; break;
    default: return;                                  // idle / unknown
  }

  // Any key at all takes the boot hint down early -- including a bare Ctrl, since holding it is
  // how you start the very shortcut the hint is advertising.
  if (down) bootHintDismiss();

  if (applyModifier(code, down)) { sendKeyboardReport(); return; }

  // --- Ctrl hotkeys ---------------------------------------------------------------------
  // Only on the PRESSED edge, never on HOLD: the STM32 auto-repeats held keys, and a repeating
  // menu toggle would flicker the window open and shut.
  if (state == PCK_STATE_PRESSED && g_ctrlHeld) {
    // Ctrl-Shift-F1 -> reboot the Pico itself. This board needs its own way back to a clean
    // boot: the Pico is powered from its own USB, so the PicoCalc's power switch never resets
    // it, and a firmware that came up against a dead mainboard has no other route out.
    if (code == PCK_F1 && g_shiftHeld) {
      printLog("Ctrl-Shift-F1: rebooting");
      delay(50);
      ESP.restart();
    }
    if (code == PCK_F1) { tapHid(HID_KEY_F10); return; }              // settings menu

    // Ctrl-F6 -> back to the system selection menu. Same three steps as the REBOOT button in
    // the settings window (ouiReboot in src/shared/optionsui.cpp): persist first so the trip
    // through the splash does not silently discard a setting, then ask for the splash, then
    // restart. Without requestSplashOnNextBoot() the firmware would come straight back up in
    // the platform it is already running, which is the one thing this key exists to escape.
    // The delay lets the log line reach the serial port before the core goes down.
    if (code == PCK_F6 || (code == PCK_F3 && g_shiftHeld)) {
      printLog("Ctrl-F6: rebooting to the system menu");
      saveConfig();
      requestSplashOnNextBoot();
      delay(50);
      ESP.restart();
    }

    // CPU reset moved here when Ctrl-F8 (now Ctrl-F6) became the system menu. This MUST stay below
    // the branch above, which claims Ctrl-Shift-F3 for the system menu: the two share a key and
    // are told apart only by shift, so testing the unshifted form first would swallow both.
    // (Safe as written either way -- the branch above restarts the board rather than returning.)
    if (code == PCK_F3) { tapHid(HID_KEY_F11); return; }              // CPU reset
  }

  // Boot splash: the shared splashService() picks a platform from touch coordinates, which this
  // board cannot produce. Swallow left/right/Enter while it is up and post them as nav events
  // instead -- swallowing matters because the keys would otherwise reach the core underneath
  // (an Enter would land in Applesoft the moment the splash closed).
  if (splashActive) {
    if (!down) return;                              // releases are meaningless to the splash
    switch (code) {
      case PCK_LEFT:  splashKeyEvent = SPLASH_KEY_LEFT;   return;
      case PCK_RIGHT: splashKeyEvent = SPLASH_KEY_RIGHT;  return;
      case PCK_UP:    splashKeyEvent = SPLASH_KEY_UP;     return;
      case PCK_DOWN:  splashKeyEvent = SPLASH_KEY_DOWN;   return;
      case PCK_ENTER: splashKeyEvent = SPLASH_KEY_SELECT; return;
      default: break;                               // anything else falls through to the core
    }
  }

  HidKey k = pcToHid(code);
  if (!k.hid) return;

  if (down) {
    // A repeat (HOLD, or a PRESSED for an already-down key) must re-trigger the edge-driven
    // cores: drop the key, report, then press it again so usbKeyboardReport sees a fresh down.
    bool already = false;
    for (int i = 0; i < 6; i++) if (g_keys[i] == k.hid) { already = true; break; }
    if (already) { pressRemove(k.hid); sendKeyboardReport(); }
    pressAdd(k.hid, k.shift);
  } else {
    pressRemove(k.hid);                               // base HID code is shift-invariant
  }
  sendKeyboardReport();
}

// ---------------------------------------------------------------------------------------------
// i2c1 transport (split-phase, see the header comment)
// ---------------------------------------------------------------------------------------------
static bool g_kbdReady   = false;   // Wire1 brought up
static bool g_selPending = false;   // a FIFO-register write is outstanding; its result is readable

static void selectFifoReg()
{
  Wire1.beginTransmission(KBD_I2C_ADDR);
  Wire1.write((uint8_t)KBD_REG_FIF);
  g_selPending = (Wire1.endTransmission() == 0);
}

// Reads the pending FIFO word. Returns false when nothing was read or the FIFO is empty.
static bool readFifo(uint8_t *state, uint8_t *code)
{
  if (!g_selPending) return false;
  g_selPending = false;
  if (Wire1.requestFrom((uint8_t)KBD_I2C_ADDR, (size_t)2) != 2) return false;
  uint8_t s = (uint8_t)Wire1.read();
  uint8_t c = (uint8_t)Wire1.read();
  if (s == 0 && c == 0) return false;                 // FIFO empty
  *state = s; *code = c;
  return true;
}

// Nothing answered at 0x1F -- say what IS on the bus. An empty bus and a mis-wired bus look
// identical from one failed transaction but need opposite fixes: empty means the STM32 has no
// power (on the PicoCalc the keyboard MCU, the backlight and the SD slot all sit on the
// mainboard rail that the power switch gates, so a Pico running on USB alone finds nothing),
// while any other address means we are on the wrong pins or the wrong i2c block.
static void scanI2c1()
{
  int found = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire1.beginTransmission(a);
    if (Wire1.endTransmission() == 0) {
      sprintf(buf, "input: i2c1 device found at 0x%02X", (unsigned)a);
      printLog(buf);
      found++;
    }
  }
  if (!found)
    printLog("input: i2c1 bus EMPTY - nothing answering on GPIO6/7 at all");
}

// Bring up i2c1 on the PicoCalc's pins. Split out of picocalcInputSetup() because the
// mainboard-presence wait below has to run long before the keyboard is set up.
static void wireBegin()
{
  if (g_kbdReady) return;
  Wire1.setSDA(KBD_I2C_SDA_PIN);
  Wire1.setSCL(KBD_I2C_SCL_PIN);
  Wire1.begin();
  Wire1.setClock(KBD_I2C_HZ);
  g_kbdReady = true;
}

// Is the mainboard alive? The STM32 answering at 0x1F is the one cheap proof we have: it, the
// panel, the backlight and the SD slot all sit on the same mainboard rail, so if it answers,
// they are all powered.
static bool mainboardPresent()
{
  wireBegin();
  Wire1.beginTransmission(KBD_I2C_ADDR);
  return Wire1.endTransmission() == 0;
}

// The Pico is powered from its OWN USB port, so flipping the PicoCalc's power switch does not
// reset it. Boot the firmware with the unit switched off and every mainboard peripheral is dead
// while setup() runs: the SD does not mount, the keyboard does not answer, the panel init goes
// nowhere -- and switching the unit on afterwards is far too late, because all of that already
// ran and failed. So before touching any of it, wait here for the rail to come up.
// Returns true if the mainboard is up by the time we give up waiting.
bool g_bootedHeadless = false;   // setup() gave up waiting; picocalcPumpInput() watches for it

bool picocalcWaitForMainboard()
{
  if (mainboardPresent()) return true;

  printLog("Waiting for PicoCalc mainboard - turn the unit on");
  uint32_t t0 = millis(), lastLog = 0;
  while (millis() - t0 < PICOCALC_MAINBOARD_WAIT_MS) {
    if (mainboardPresent()) {
      delay(300);                       // let the rail settle before the SD init hits it
      sprintf(buf, "PicoCalc mainboard up after %lums", (unsigned long)(millis() - t0));
      printLog(buf);
      return true;
    }
    if (millis() - lastLog >= 2000) {   // heartbeat, so the serial log does not look hung
      lastLog = millis();
      sprintf(buf, "  ...still waiting (%lus)", (unsigned long)((millis() - t0) / 1000));
      printLog(buf);
    }
    delay(100);
  }
  printLog("PicoCalc mainboard never came up - no SD, no keyboard, no screen");
  printLog("(switch the unit on and it will reboot itself)");
  g_bootedHeadless = true;
  return false;
}

void picocalcInputSetup()
{
  bool first = !g_kbdReady;
  wireBegin();
  if (!first) return;
  selectFifoReg();                                    // prime the split-phase poll
  if (g_selPending) {
    printLog("input: PicoCalc keyboard ready (i2c1 0x1F)");
  } else {
    printLog("input: PicoCalc keyboard NOT responding on i2c1 0x1F");
    scanI2c1();
  }
}

// Backlight. On the PicoCalc BOTH backlights are PWM outputs of the STM32 keyboard MCU, reachable
// only through its register file. The STM32 keeps its own power domain and REMEMBERS the levels
// across a Pico reset and across a reflash, so a unit left at zero stays black no matter how
// correctly the panel is driven -- which is exactly what a "nothing draws at all" bring-up looks
// like.
//
// The two registers are easy to confuse and we had the wrong one:
//   REG_ID_BKL (0x05) -- the KEYBOARD backlight
//   REG_ID_BK2 (0x0A) -- the LCD backlight        <-- the one that matters here
// Writing a register means sending (id | 0x80) followed by the 0..255 level; sending the bare id
// selects it for reading instead, which is how the read-back below works.
//
// Both are driven to full: the LCD because it is the point, the keyboard because a lit keyboard is
// a free confirmation that i2c register WRITES are reaching the STM32 at all -- if the keys light
// up and the panel stays dark, the bus is fine and the fault is in the panel.
#define KBD_REG_BKL 0x05          // keyboard backlight
#define KBD_REG_BK2 0x0A          // LCD backlight
#define KBD_REG_WRITE 0x80        // OR into the register id to make it a write

static bool bklWrite(uint8_t reg, uint8_t level)
{
  Wire1.beginTransmission(KBD_I2C_ADDR);
  Wire1.write((uint8_t)(reg | KBD_REG_WRITE));
  Wire1.write(level);
  return Wire1.endTransmission() == 0;
}

// Select `reg` and read one byte back. Returns -1 if the STM32 did not answer.
static int bklRead(uint8_t reg)
{
  Wire1.beginTransmission(KBD_I2C_ADDR);
  Wire1.write(reg);
  // false == REPEATED START. Ending with a STOP lets the STM32 drop the selection, and it then
  // answers the following read with the last byte it received -- which is why a read of 0x0A came
  // back as literally 10 and a read of 0x05 as 5. Those were echoes, not brightness levels.
  if (Wire1.endTransmission(false) != 0) return -1;
  if (Wire1.requestFrom((uint8_t)KBD_I2C_ADDR, (uint8_t)1) != 1) return -1;
  return Wire1.read();
}

void picocalcBacklightOn()
{
  picocalcInputSetup();

  int wasLcd = bklRead(KBD_REG_BK2);
  int wasKbd = bklRead(KBD_REG_BKL);

  bool okLcd = bklWrite(KBD_REG_BK2, 0xFF);
  bool okKbd = bklWrite(KBD_REG_BKL, 0xFF);

  int nowLcd = bklRead(KBD_REG_BK2);
  int nowKbd = bklRead(KBD_REG_BKL);

  char m[140];
  sprintf(m, "display: backlight LCD(0x0A) %d->%d %s / KBD(0x05) %d->%d %s",
          wasLcd, nowLcd, okLcd ? "ok" : "WRITE FAILED",
          wasKbd, nowKbd, okKbd ? "ok" : "WRITE FAILED");
  printLog(m);

  // The reads above left the STM32's register pointer on a backlight register, so the FIFO read
  // primed by picocalcInputSetup() would come back with a brightness level instead of a key event.
  // Re-arm it here rather than letting the first pump decode a bogus {state, code} pair.
  selectFifoReg();
}

// Block until a key is pressed, or timeoutMs elapses. Used to hold the boot console on screen
// long enough to read it -- the renderer takes the panel over within milliseconds otherwise.
// Serial counts as a key so the pause is escapable when the keyboard itself is the thing that
// is broken; a timeout is kept so an unattended unit still boots.
bool picocalcWaitAnyKey(uint32_t timeoutMs)
{
  wireBegin();
  while (Serial.available()) Serial.read();          // drop anything already buffered
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (Serial.available()) { while (Serial.available()) Serial.read(); return true; }
    selectFifoReg();
    delayMicroseconds(PCKBD_TURNAROUND_US);
    uint8_t state, code;
    if (readFifo(&state, &code)) return true;
    delay(20);
  }
  return false;
}

// Called once per frame from DisplayGFX::flush().
void picocalcPumpInput()
{
  if (!g_kbdReady) picocalcInputSetup();

  // We booted against a dead mainboard (the wait in setup() timed out), so the SD never
  // mounted, no ROMs loaded and the panel was initialised with no power behind it. If the unit
  // is switched on now, none of that fixes itself -- the only clean recovery is to start over
  // with everything powered, which is what the user cannot do by hand because the Pico is fed
  // from its own USB and the PicoCalc's switch never resets it. So do it for them.
  if (g_bootedHeadless) {
    if (mainboardPresent()) {
      printLog("PicoCalc mainboard just came up - rebooting to initialise it properly");
      delay(50);
      ESP.restart();
    }
    return;                             // nothing to poll until then
  }

  uint8_t state, code;
  if (!readFifo(&state, &code)) {selectFifoReg();return; }
  handleEvent(state, code);

#if PCKBD_DRAIN_EXTRA > 0
  for (int i = 0; i < PCKBD_DRAIN_EXTRA; i++) {
    selectFifoReg();
    delayMicroseconds(PCKBD_TURNAROUND_US);
    if (!readFifo(&state, &code)) break;
    handleEvent(state, code);
  }
#endif

  selectFifoReg();                                    // arm the read for the next frame
}

// ---------------------------------------------------------------------------------------------
// stubs for the files that are compiled out on this board
// ---------------------------------------------------------------------------------------------

// usbgamepad.cpp (EspUsbHost) is excluded; joystickSetup() still calls this. No USB host here --
// the arrow keys drive the consoles through usbKeyboardReport()'s per-platform joystick paths.
void usbGamepadSetup()
{
  picocalcInputSetup();
  printLog("input: no USB gamepad on PicoCalc (arrow keys drive the consoles)");
}

// touchkeyboard.cpp is excluded (no touch panel). Keep the raster geometry it would have reported
// with the keyboard closed, so renderLoop draws the normal centred 320x192 picture.
void oskBuildLayout() {}
void oskSetup() {}
void oskRender() {}
bool oskActive() { return false; }
bool oskDirty()  { return false; }
int  oskRasterTop()    { return 24; }
int  oskRasterHeight() { return 192; }
bool touchRead(int16_t *sx, int16_t *sy) { (void)sx; (void)sy; return false; }
void oskPoll() {}
void oskIgnoreCurrentTouch() {}

#endif // BOARD_PICOCALC
