// input_sdl.cpp — desktop SDL2 input. Replaces usbgamepad.cpp + keyboardPs2.cpp.
//
//   * Keyboard: SDL physical scancodes ARE USB-HID usage codes (SDL_SCANCODE_A == 0x04 == HID_KEY_A),
//     so we forward them straight into the unchanged usbKeyboardReport(modifier, keys[6], last[6])
//     dispatcher in src/shared/usbkeyboard.cpp (per-platform key handling reused as-is).
//   * Mouse -> the display backend's touch state (tft.setMouseState) -> touchRead()/oskPoll().
//   * Optional game controller -> joyX/joyY/Pb0-3 -> applyPlatformInput().
//   * SDL_QUIT -> clean exit.
//
// desktopPumpInput() is called every frame from DisplayGFX::flush() (main thread).
#if defined(BOARD_DESKTOP)

#include "../../emu.h"
#include "ui_imgui.h"
#include <SDL.h>
#include <cstring>
#include <vector>

extern DisplayGFX tft;

static SDL_GameController *g_pad = nullptr;

// keyboardPs2.cpp is excluded on desktop; provide its 4 entry points with the SAME semantics (the
// Apple II/IIGS read the keyboard latch keymem; the OSK/USB paths set it). No PS/2 hardware here.
unsigned char keyboard_read() { return (unsigned char)keymem; }
void keyboardStrobe() { keymem &= 0x7F; }     // clear the "key ready" bit on $C010 strobe
void keyboardSetup() {}
void keyboard_bit() {}

// usbgamepad.cpp's entry point is called by joystickSetup(); provide it here (that file is excluded).
void usbGamepadSetup() {
  if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
  for (int i = 0; i < SDL_NumJoysticks(); i++)
    if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); if (g_pad) break; }
  printLog(g_pad ? "input: SDL game controller open" : "input: keyboard input ready (no pad)");
}

// --- keyboard: maintain the live pressed-key set + modifier byte, forward the HID boot report ---
static uint8_t hidModifiers() {
  SDL_Keymod m = SDL_GetModState();
  uint8_t mod = 0;
  if (m & KMOD_LCTRL)  mod |= 0x01;
  if (m & KMOD_LSHIFT) mod |= 0x02;
  if (m & KMOD_LALT)   mod |= 0x04;
  if (m & KMOD_LGUI)   mod |= 0x08;
  if (m & KMOD_RCTRL)  mod |= 0x10;
  if (m & KMOD_RSHIFT) mod |= 0x20;
  if (m & KMOD_RALT)   mod |= 0x40;
  if (m & KMOD_RGUI)   mod |= 0x80;
  return mod;
}

static std::vector<uint8_t> g_pressed;          // active non-modifier HID usage codes (press order)
static uint8_t g_lastReport[6] = {0, 0, 0, 0, 0, 0};

static void pressAdd(uint8_t hid) {
  if (hid >= 0xE0) return;                       // modifier keys live in the modifier byte, not keys[]
  for (auto c : g_pressed) if (c == hid) return;
  g_pressed.push_back(hid);
}
static void pressRemove(uint8_t hid) {
  for (size_t i = 0; i < g_pressed.size(); i++) if (g_pressed[i] == hid) { g_pressed.erase(g_pressed.begin() + i); return; }
}

static void sendKeyboardReport() {
  uint8_t keys[6] = {0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < g_pressed.size() && i < 6; i++) keys[i] = g_pressed[i];
  usbKeyboardReport(hidModifiers(), keys, g_lastReport);
  memcpy(g_lastReport, keys, 6);
}

// --- optional game controller -> joystick globals ---
static void pollController() {
  if (!g_pad) return;
  auto b = [&](SDL_GameControllerButton btn) { return SDL_GameControllerGetButton(g_pad, btn) != 0; };
  int ax = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
  int ay = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
  bool up    = b(SDL_CONTROLLER_BUTTON_DPAD_UP)    || ay < -12000;
  bool down  = b(SDL_CONTROLLER_BUTTON_DPAD_DOWN)  || ay >  12000;
  bool left  = b(SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || ax < -12000;
  bool right = b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax >  12000;
  joyY = left ? 0 : right ? 2 : 1;               // horizontal: 0 left, 1 center, 2 right
  joyX = up   ? 0 : down  ? 2 : 1;               // vertical:   0 up,   1 center, 2 down
  Pb0 = b(SDL_CONTROLLER_BUTTON_A) || b(SDL_CONTROLLER_BUTTON_B);
  Pb1 = b(SDL_CONTROLLER_BUTTON_X) || b(SDL_CONTROLLER_BUTTON_Y);
  Pb2 = b(SDL_CONTROLLER_BUTTON_BACK);
  Pb3 = b(SDL_CONTROLLER_BUTTON_START);
  applyPlatformInput();
}

// Headless scripting: inject SPACE / joystick on a frame schedule so the demo can be driven offline.
//   EMU_TAP=<f>       press SPACE for [f, f+15)              (enter the game)
//   EMU_HOLD_FROM=<f> from frame f, hold a joystick direction (EMU_HOLD_DIR = L|R|U|D, default R)
//   EMU_TRIG=1        pulse the trigger (Pb0) ~3 frames on / 3 off while holding (punch)
static void desktopScriptInput() {
  static long f = -1; f++;
  static int  tap  = []{ const char*s=getenv("EMU_TAP");       return s?atoi(s):-1; }();
  static int  hfrom= []{ const char*s=getenv("EMU_HOLD_FROM"); return s?atoi(s):-1; }();
  static char dir  = []{ const char*s=getenv("EMU_HOLD_DIR");  return s?s[0]:'R'; }();
  static int  trig = []{ const char*s=getenv("EMU_TRIG");      return s?atoi(s):0; }();
  if (tap < 0 && hfrom < 0) return;                      // no script
  if (tap >= 0) { if (f >= tap && f < tap + 15) pressAdd(0x2C); else pressRemove(0x2C); }  // SPACE = HID 0x2C
  // arrow-key HID codes: Right 0x4F, Left 0x50, Down 0x51, Up 0x52
  const uint8_t AR=0x4F, AL=0x50, AD=0x51, AU=0x52;
  if (hfrom >= 0 && f >= hfrom) {
    bool R=(dir=='R'),L=(dir=='L'),U=(dir=='U'),D=(dir=='D');
    if(R)pressAdd(AR);else pressRemove(AR);  if(L)pressAdd(AL);else pressRemove(AL);
    if(U)pressAdd(AU);else pressRemove(AU);  if(D)pressAdd(AD);else pressRemove(AD);
    joyY = L?0:R?2:1; joyX = U?0:D?2:1;                   // also drive the joystick
    Pb0  = trig ? (((f / 3) & 1) == 0) : false;
    applyPlatformInput();
  }
}

// Offline keyboard self-test: EMU_DBG_TYPE="HELLO" pushes REAL SDL key events (one char every few
// frames, after boot) so the full chain is exercised — ImGui ProcessEvent + the WantTextInput gate +
// usbKeyboardReport — exactly like a user typing. Proves the host keyboard reaches the emulator.
static SDL_Scancode scancodeForChar(char c) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  if (c >= 'A' && c <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (c - 'A'));
  if (c >= '1' && c <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (c - '1'));
  if (c == '0') return SDL_SCANCODE_0;
  if (c == ' ') return SDL_SCANCODE_SPACE;
  if (c == '\n') return SDL_SCANCODE_RETURN;
  return SDL_SCANCODE_UNKNOWN;
}
static void desktopTypeInject() {
  static const char *s = getenv("EMU_DBG_TYPE");
  if (!s || !*s) return;
  static long fr = -1; fr++;
  static long at = []{ const char *e = getenv("EMU_DBG_TYPE_AT"); return e ? atol(e) : 120L; }();
  static size_t idx = 0; static bool down = false;
  if (fr < at || (fr % 6) != 0) return;         // wait for boot, then ~1 transition / 6 frames
  if (!s[idx]) return;
  SDL_Scancode sc = scancodeForChar(s[idx]);
  if (sc == SDL_SCANCODE_UNKNOWN) { idx++; return; }
  SDL_Event e; memset(&e, 0, sizeof(e));
  e.key.keysym.scancode = sc;
  e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
  if (!down) { e.type = SDL_KEYDOWN; e.key.state = SDL_PRESSED;  down = true; }
  else       { e.type = SDL_KEYUP;   e.key.state = SDL_RELEASED; down = false; idx++; }
  SDL_PushEvent(&e);
}

void desktopPumpInput() {
  desktopScriptInput();
  desktopTypeInject();
  SDL_Event e;
  bool kbChanged = false;
  while (SDL_PollEvent(&e)) {
    desktopUiProcessEvent(&e);                       // let the ImGui shell see every event first
    // When ImGui owns the keyboard/mouse (typing in a debug field, dragging a panel), don't also
    // feed the emulator. Key RELEASES always pass through so a key can't get stuck "pressed".
    bool uiKb = desktopUiWantCaptureKeyboard();
    bool uiMouse = desktopUiWantCaptureMouse();
    switch (e.type) {
      case SDL_QUIT:
        desktopUiSaveState();      // persist window size, panel layout, view prefs, settings + last disk
        running = false;
        SDL_Quit();
        std::exit(0);
        break;
      case SDL_KEYDOWN:
        if (!e.key.repeat && !uiKb) { pressAdd((uint8_t)e.key.keysym.scancode); kbChanged = true; }
        break;
      case SDL_KEYUP:
        pressRemove((uint8_t)e.key.keysym.scancode);
        kbChanged = true;
        break;
      case SDL_MOUSEMOTION:
        if (!uiMouse) tft.setMouseState(e.motion.x, e.motion.y, (e.motion.state & SDL_BUTTON_LMASK) != 0);
        break;
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
        if (!uiMouse) tft.setMouseState(e.button.x, e.button.y, e.button.state == SDL_PRESSED);
        else          tft.setMouseState(-1, -1, false);   // release any emulator touch when UI grabs the mouse
        break;
      case SDL_CONTROLLERDEVICEADDED:
        if (!g_pad && SDL_IsGameController(e.cdevice.which)) g_pad = SDL_GameControllerOpen(e.cdevice.which);
        break;
    }
  }
  // Always re-send the keyboard report (cheap; usbKeyboardReport diffs internally and also needs to
  // see modifier-only changes for the PC-XT path).
  (void)kbChanged;
  sendKeyboardReport();
  pollController();
}

#endif // BOARD_DESKTOP
