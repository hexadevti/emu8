// input_web.cpp — browser input. Replaces src/desktop/input_sdl.cpp.
//
// The page (main browser thread) calls the exported web_* functions; they only append to a small
// lock-free event queue. desktopPumpInput() (render thread, every frame via DisplayGFX::flush())
// drains it and feeds the unchanged emulator entry points:
//   * keys   -> USB-HID usage codes -> usbKeyboardReport() (per-platform key maps reused as-is)
//   * pad    -> joyX/joyY/Pb0-3 -> applyPlatformInput()   (like the desktop's SDL game controller)
//   * touch  -> tft.setMouseState() in framebuffer coords -> touchRead() (menus / splash / OSK)
#if defined(BOARD_DESKTOP) && defined(EMU_WEB)

#include "emu.h"
#include "src/shared/apple2joycal.h"
#include <emscripten.h>
#include <emscripten/threading.h>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

extern DisplayGFX tft;

unsigned char keyboard_read() { return (unsigned char)keymem; }
void keyboardStrobe() { keymem &= 0x7F; }
void keyboardSetup() {}
void keyboard_bit() {}
void usbGamepadSetup() { printLog("input: web keyboard / touch pad ready"); }

// ---- event queue (single producer: browser main thread; single consumer: render thread) ----
enum { EV_KEY = 1, EV_PAD = 2, EV_TOUCH = 3, EV_MODS = 4, EV_MENU = 5, EV_STICK = 6 };
struct Ev { int type, a, b, c; };
static const uint32_t QN = 1024;
static Ev g_q[QN];
static std::atomic<uint32_t> g_qW{0}, g_qR{0};

static void qPush(int type, int a, int b, int c) {
  uint32_t w = g_qW.load(std::memory_order_relaxed);
  if (w - g_qR.load(std::memory_order_acquire) >= QN) return;   // full: drop
  g_q[w & (QN - 1)] = Ev{type, a, b, c};
  g_qW.store(w + 1, std::memory_order_release);
}

extern "C" {
// hid = USB-HID usage code (0x04 'A' ... 0xE7 right GUI); modifiers 0xE0..0xE7 are allowed here.
EMSCRIPTEN_KEEPALIVE void web_key(int hid, int down) { qPush(EV_KEY, hid, down, 0); }
// bit0 up, bit1 down, bit2 left, bit3 right, bit4 A, bit5 B, bit6 select, bit7 start
EMSCRIPTEN_KEEPALIVE void web_pad(int mask) { qPush(EV_PAD, mask, 0, 0); }
// Apple analog stick (on-screen stick / a gamepad's axes): -32768..32767 each, on = 0 lets it spring
// back to the centre
EMSCRIPTEN_KEEPALIVE void web_stick(int x, int y, int on) { qPush(EV_STICK, x, y, on); }
// framebuffer coordinates (x<0 = release)
EMSCRIPTEN_KEEPALIVE void web_touch(int x, int y, int down) { qPush(EV_TOUCH, x, y, down); }
EMSCRIPTEN_KEEPALIVE int  web_platform() { return (int)currentPlatform; }
EMSCRIPTEN_KEEPALIVE int  web_menu_open() { return OptionsWindow ? 1 : 0; }
// the system picker (splash) is up: no machine running yet

EMSCRIPTEN_KEEPALIVE int  web_splash() { return splashActive ? 1 : 0; }
// The page is hidden (tab switched, app minimised, phone locked): the whole machine sleeps. The cores
// park in their pause spin (the shared `paused`, as the desktop debugger uses it; they re-anchor
// their pacing on the way out), the render loop and the speaker thread nap in webHiddenWait().
static std::atomic<int> g_hidden{0};
static bool g_hidPaused = false;
EMSCRIPTEN_KEEPALIVE void web_hidden(int h) {
  if ((h != 0) == (g_hidden.load() != 0)) return;
  if (h) { g_hidPaused = paused; paused = true; g_hidden.store(1); }
  else   { g_hidden.store(0); paused = g_hidPaused; }
}
// Apple II model (the page draws the II+ or the IIe keyboard)
EMSCRIPTEN_KEEPALIVE int  web_apple_iie() { return AppleIIe ? 1 : 0; }
// The on-screen keyboard is open: its keys are keys (the Apple's arrows type), the pad is the stick
static std::atomic<int> g_oskOpen{0};
EMSCRIPTEN_KEEPALIVE void web_osk_open(int on) { g_oskOpen.store(on ? 1 : 0); }
// The phone's settings sheet (app.js) drives optionsui.cpp through these: commands are queued to the
// render thread (see optionsUiWebCmd); the menu's state comes back as JSON, rebuilt there a few
// times a second while the menu is open, double-buffered so the page reads a complete copy.
EMSCRIPTEN_KEEPALIVE void web_menu_cmd(int cmd, int arg) { qPush(EV_MENU, cmd, arg, 0); }
static std::string g_menuBuf[2];
static std::atomic<int> g_menuPub{0}, g_menuSeq{0};
EMSCRIPTEN_KEEPALIVE int web_menu_seq() { return g_menuSeq.load(); }
EMSCRIPTEN_KEEPALIVE const char *web_menu_state() { return g_menuBuf[g_menuPub.load()].c_str(); }
}
bool webHidden() { return g_hidden.load() != 0; }
void webHiddenWait() { while (g_hidden.load() && running) emscripten_thread_sleep(100); }
const char *optionsUiWebState();
void optionsUiWebCmd(int cmd, int arg);
static void menuPublish() {
  const int nx = g_menuPub.load() ^ 1;
  g_menuBuf[nx] = optionsUiWebState();
  g_menuPub.store(nx);
  g_menuSeq.fetch_add(1);
}

// ---- keyboard state ----
static std::vector<uint8_t> g_pressed;
static uint8_t g_mods = 0;
static uint8_t g_lastReport[6] = {0, 0, 0, 0, 0, 0};

static void pressAdd(uint8_t hid) {
  for (auto c : g_pressed) if (c == hid) return;
  g_pressed.push_back(hid);
}
static void pressRemove(uint8_t hid) {
  for (size_t i = 0; i < g_pressed.size(); i++) if (g_pressed[i] == hid) { g_pressed.erase(g_pressed.begin() + i); return; }
}
static int g_pad = 0;                           // the on-screen pad's buttons (web_pad mask)
static int g_stickX = 0, g_stickY = 0;
static bool g_stickOn = false;                  // Apple analog stick held
static void applePadOverlay();
static void sendKeyboardReport() {
  uint8_t keys[6] = {0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < g_pressed.size() && i < 6; i++) keys[i] = g_pressed[i];
  // Apple: with the on-screen keyboard open its arrows must type (JOYSTICK = JOY would turn them into
  // paddle moves only); the on-screen pad is the stick. The setting is kept for the physical keyboard.
  const bool apple = currentPlatform == PLATFORM_APPLE2;
  const uint8_t arrows = appleArrows;
  if (apple && g_oskOpen.load() && appleArrows == APPLE_ARROWS_JOY) appleArrows = APPLE_ARROWS_BOTH;
  usbKeyboardReport(g_mods, keys, g_lastReport);   // Apple: the pad / stick ride on top (webApplePadOverlay)
  appleArrows = arrows;
  memcpy(g_lastReport, keys, 6);
}

// ---- virtual pad ----
// Apple: stick / digital pad -> paddles + open/solid-apple, laid over what the held keys say. The
// keyboard report calls this before it writes anything, so the paddles go straight to the final value.
void webApplePadOverlay(float &p0, float &p1, bool &b0, bool &b1) {
  const int m = g_pad;
  const bool up = m & 1, down = m & 2, left = m & 4, right = m & 8;
  static bool calInit = false;
  if (!calInit) { apple2JoycalDefaults(-32768, 0, 0, 32767); calInit = true; }
  if (g_stickOn) { p0 = apple2JoycalPaddle(0, g_stickX); p1 = apple2JoycalPaddle(1, g_stickY); }
  if (left || right) p0 = apple2JoycalPaddle(0, left ? -32768 : 32767);
  if (up || down)    p1 = apple2JoycalPaddle(1, up ? -32768 : 32767);
  if (m & 0x10) b0 = true;
  if (m & 0x20) b1 = true;
}
static void applePadOverlay() {
  float p0 = timerpdl0, p1 = timerpdl1; bool b0 = Pb0, b1 = Pb1;
  webApplePadOverlay(p0, p1, b0, b1);
  timerpdl0 = p0; timerpdl1 = p1; Pb0 = b0; Pb1 = b1;
}

static void applyPad(int m) {
  const bool up = m & 1, down = m & 2, left = m & 4, right = m & 8;
  g_pad = m;
  joystick = true;                              // the on-screen pad IS a joystick: let it reach the core
  joyY = left ? 0 : right ? 2 : 1;
  joyX = up ? 0 : down ? 2 : 1;
  Pb0 = (m & 0x10) != 0;
  Pb1 = (m & 0x20) != 0;
  Pb2 = (m & 0x40) != 0;
  Pb3 = (m & 0x80) != 0;
  if (currentPlatform == PLATFORM_APPLE2) sendKeyboardReport();   // keys' paddles + the pad on top
  applyPlatformInput();
}

void desktopPumpInput() {
  bool kb = false;
  uint32_t r = g_qR.load(std::memory_order_relaxed);
  const uint32_t w = g_qW.load(std::memory_order_acquire);
  bool touchDown = false, menu = false;
  for (; r != w; r++) {
    const Ev e = g_q[r & (QN - 1)];
    // A tap's press and release can land in the same frame: keep the release for the next frame so
    // touchRead() sees the screen pressed at least once.
    if (e.type == EV_TOUCH && !e.c && touchDown) break;
    if (e.type == EV_TOUCH && e.c) touchDown = true;
    switch (e.type) {
      case EV_KEY: {
        const uint8_t hid = (uint8_t)e.a;
        if (hid >= 0xE0 && hid <= 0xE7) {
          const uint8_t bit = (uint8_t)(1u << (hid - 0xE0));
          g_mods = e.b ? (g_mods | bit) : (g_mods & ~bit);
        } else if (e.b) pressAdd(hid); else pressRemove(hid);
        kb = true;
        // A full key press can arrive within one frame (tap): report the down state now, the up
        // state on the next event, so the diff in usbKeyboardReport() sees both edges.
        sendKeyboardReport();
        break;
      }
      case EV_PAD:   applyPad(e.a); break;
      case EV_STICK:
        g_stickX = e.a; g_stickY = e.b; g_stickOn = e.c != 0;
        // Held: straight onto the paddles (a full keyboard report would first write the centre, and a
        // PDL read running on the CPU thread can catch that and stop short). Released: the report
        // puts back whatever the held keys say.
        if (currentPlatform == PLATFORM_APPLE2) { if (g_stickOn) applePadOverlay(); else sendKeyboardReport(); }
        break;
      case EV_TOUCH: tft.setMouseState(e.a, e.b, e.c != 0); break;
      case EV_MENU:  optionsUiWebCmd(e.a, e.b); menu = true; break;
    }
  }
  g_qR.store(r, std::memory_order_release);
  // cheap; also carries modifier-only state (PC-XT). Not on the Apple: its report only changes with a
  // key / pad / stick event (each sends its own), and re-sending it every frame would flick the paddles
  // to the centre under a held stick for a moment, enough to cut a game's PDL read short.
  if (!kb && currentPlatform != PLATFORM_APPLE2) sendKeyboardReport();
  // the settings sheet's view of the menu: after each command, else ~8x a second while it is open
  static int frames = 0;
  static bool wasOpen = false;
  if (menu || OptionsWindow != wasOpen || (OptionsWindow && ++frames >= 6)) { frames = 0; menuPublish(); }
  wasOpen = OptionsWindow;
}

#endif // BOARD_DESKTOP && EMU_WEB
