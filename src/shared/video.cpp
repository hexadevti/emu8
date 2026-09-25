#include "../../emu.h"

#include "bootlogo.h"   // embedded boot-splash logo (RGB565)
#include <esp_system.h>          // esp_reset_reason()
#include <esp_attr.h>            // RTC_NOINIT_ATTR
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#include "esp_task_wdt.h"        // esp_task_wdt_reconfigure (core 3.x TWDT, replaces disableCore0WDT)
#endif
bool splashActive = true; // true until the boot splash times out or is dismissed
#if defined(BOARD_PICOCALC)
// Keyboard-driven splash navigation (this board has no touch panel). input_picocalc.cpp posts
// one of SPLASH_KEY_* here while splashActive is set, instead of dispatching that key to the
// core; splashService() consumes it on the next render pass. Single producer (the input pump,
// which runs from displayFlush() on this very task) and single consumer, so a plain volatile
// byte is enough -- no queue needed.
volatile int8_t splashKeyEvent = 0;
#endif

// Boot-splash policy. The SELECT SYSTEM banner only appears on a hardware reset (power-on / RST
// button) or when the user explicitly asks for it via the "Reboot" settings button / Ctrl-F5 --
// which call requestSplashOnNextBoot() to stash a magic in RTC memory (it survives the soft reset)
// just before restarting. Selecting a platform on the splash, and the mount+reboot paths, restart
// WITHOUT the magic, so they boot straight into the chosen system without re-showing the banner.

RTC_NOINIT_ATTR static uint32_t splashOnBootMagic;
#define SPLASH_ON_BOOT_MAGIC 0x5350A5AAu
void requestSplashOnNextBoot() { splashOnBootMagic = SPLASH_ON_BOOT_MAGIC; }

// Same mechanism for the SD Manager button: it is a boot mode, not a saved platform (emu.h), so the
// splash leaves the choice here instead of in EEPROM and reboots. Honoured only after a software
// reset -- RTC_NOINIT memory holds garbage at power-on -- and consumed on read, so the boot after
// this one (Ctrl-F6, a power cycle) comes back to the system the user was running.
RTC_NOINIT_ATTR static uint32_t sdManagerOnBootMagic;
#define SDMGR_ON_BOOT_MAGIC 0x53444D47u
static void requestSdManagerOnNextBoot() { sdManagerOnBootMagic = SDMGR_ON_BOOT_MAGIC; }
bool sdManagerBootRequested()
{
  bool yes = (esp_reset_reason() == ESP_RST_SW) && sdManagerOnBootMagic == SDMGR_ON_BOOT_MAGIC;
  sdManagerOnBootMagic = 0;
  return yes;
}

// colors[] / colors16[] are defined in globals.cpp (after tft, for static-init order).
int flashCount = 0;
int touchCount = 0;
int width = 280;
int height = 192;

// STATIC stack for the render task: a heap xTaskCreate fails on the fragmented Apple heap
// (no contiguous 8K block) -> black screen. Static can't fail. Fits the static budget now that
// the framebuffer shares Apple's RAM buffer (see sharedBigBuf). 8K: SD scan + TFT run here.
static StaticTask_t renderTaskTCB;
// Arduino-ESP32 sizes task stacks in BYTES; vanilla FreeRTOS (the PicoCalc's RP2350 core) sizes
// them in 4-byte WORDS. The number below is used BOTH as the array length and as the depth passed
// to xTaskCreateStatic*, so on the Pico the same 8192 would silently reserve 32KB out of 520KB.
// 2048 words = the same 8KB this task has always had.
#if defined(BOARD_PICOCALC)
#define RENDER_TASK_STACK 2048
#else
#define RENDER_TASK_STACK 8192
#endif
static StackType_t  renderTaskStack[RENDER_TASK_STACK];

#if !BOARD_DISPLAY_GFX && !defined(BOARD_DESKTOP) && !defined(BOARD_PICOCALC)
// On TFT_eSPI boards drawing goes straight to the 320x240 panel; the canvas flush and the
// UI/video mode switch are no-ops. (The Arduino_GFX boards define these in display_gfx.cpp;
// the desktop SDL backend defines them in src/desktop/display_sdl.cpp; the PicoCalc in
// src/picocalc/display_picocalc.cpp, where displayFlush() also polls the I2C keyboard.)
void displayFlush() {}
void displaySetUiMode(bool) {}
void displaySetVideoRect(int, int) {}
void displaySetVideoFill(int, int, bool) {}
#endif

// ---- boot progress screen ------------------------------------------------------------------
// What this replaced: a console that mirrored every printLog() line onto the panel. It made
// bring-up debuggable, but to anyone who had just switched the unit on it was a wall of log spam
// -- and it did not cover the two longest pauses in the boot AT ALL, because both happen before
// it could exist. picocalcWaitForMainboard() can sit there for seconds, and logSetup() waits up
// to five more for a USB terminal that is usually not coming. THAT was the black screen. So the
// panel now comes up ahead of both waits and shows one line naming the step the firmware is
// actually on. The full log still goes to USB CDC, which is where a log belongs.
#if defined(BOARD_PICOCALC)
static bool s_bootProg = false;
static const int kBootMsgY = 152;      // clear of the logo, which occupies rows 38..119

void bootProgressBegin()
{
  tft.begin();                         // idempotent -- videoSetup() calls it again below
  tft.fillScreen(TFT_BLACK);
  tft.fillPanelBlack();                // the bars too, or a warm reboot leaves the old frame in them
  tft.setSwapBytes(true);
  tft.pushImage((DISP_LOGICAL_W - BOOT_LOGO_W) / 2, 38, BOOT_LOGO_W, BOOT_LOGO_H, bootLogo);
  tft.setSwapBytes(false);
  s_bootProg = true;
  bootProgressStep("Starting up");
}

void bootProgressStep(const char *what)
{
  if (!s_bootProg || !what) return;
  Serial.println(what);                // deliberately not printLog(): no longer mirrored anywhere
  tft.fillRect(0, kBootMsgY - 11, DISP_LOGICAL_W, 22, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(tft.color565(0, 200, 120), TFT_BLACK);
  tft.drawString(what, DISP_LOGICAL_W / 2, kBootMsgY, 2);
}

void bootProgressEnd()
{
  if (!s_bootProg) return;
  s_bootProg = false;
  // The one thing the old console did that is worth keeping: say so when the card did not mount.
  // Without it a missing SD is silent until the emulator arrives with an empty disk list.
  if (!diskAttached && !hdAttached) {
    tft.fillRect(0, kBootMsgY - 11, DISP_LOGICAL_W, 22, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(tft.color565(255, 70, 70), TFT_BLACK);
    tft.drawString("SD card not found", DISP_LOGICAL_W / 2, kBootMsgY, 2);
    delay(1500);
  }
}
#else
// Other boards put the panel up inside videoSetup() and have no letterbox bars to draw in;
// the calls in emu8.ino stay unconditional so the boot sequence reads the same everywhere.
void bootProgressBegin() {}
void bootProgressStep(const char *) {}
void bootProgressEnd() {}
#endif

// ---- boot key hint ---------------------------------------------------------------------------
// Two lines in the BOTTOM letterbox bar (panel rows 280..319) for five seconds after the emulator
// comes up: the emulator-level Ctrl keys, then how the joystick and its buttons are reached. The
// bar is the right home for it because no core and no menu ever draws there, so it cannot cover
// the picture, and the emulator's clear-screen -- which writes exactly 320x240 through the addr
// window -- cannot wipe it early either.
//
// It stops short of the right-hand 26px so the Disk II drive light below keeps its corner. That
// matters more than it looks: the light is redrawn only when DriveMotorON_OFF changes, so erasing
// it here would leave it dark until the next time the motor happened to switch.
#if defined(BOARD_PICOCALC)
static uint8_t  s_hintState = 0;              // 0 = waiting for the splash, 1 = showing, 2 = done
static uint32_t s_hintUntil = 0;
static volatile bool s_hintKeyed = false;     // set from the keyboard task, read here

static const int32_t kHintTop = DISP_OFFSET_Y + DISP_LOGICAL_H;          // 280
static const int32_t kHintH   = PANEL_NATIVE_H - kHintTop;               // 40
static const int32_t kHintW   = PANEL_NATIVE_W - 26;                     // clear of the drive light

static void hintClearBar() { tft.fillPanelRect(0, kHintTop, kHintW, kHintH, TFT_BLACK); }

// Per platform, because none of it generalises: the reset key only exists on the Apples, and the
// fire button is a different key on every core (see the dispatch in src/shared/usbkeyboard.cpp --
// appleIsButtonKey, nesBit, atariKey, msxApplyJoystick are the authorities for these strings).
static void hintText(const char **keys, const char **joy)
{
  const bool apple = (currentPlatform == PLATFORM_APPLE2 || currentPlatform == PLATFORM_IIGS);
  *keys = apple ? "Ctrl-F1 options   Ctrl-F3 reset   Ctrl-F6 systems"
                : "Ctrl-F1 options   Ctrl-F6 systems";
  switch (currentPlatform) {
    case PLATFORM_APPLE2:
    case PLATFORM_IIGS:  *joy = "Arrows joystick   Space/F4 btn0   F5 btn1";   break;
    case PLATFORM_C64:   *joy = "Arrows joystick   Space fire";                break;
    case PLATFORM_NES:   *joy = "Arrows dpad   X=A  Z=B  Tab select  Enter start"; break;
    case PLATFORM_ATARI: *joy = "Arrows stick   Space fire   F4 select   F3 reset"; break;
    case PLATFORM_MSX:   *joy = "Arrows joystick   Space trigger";             break;
    default:             *joy = "Arrows joystick   Space fire";                break;
  }
}

void bootHintDismiss() { s_hintKeyed = true; }   // no drawing: this runs on the keyboard's task

void bootHintTick()
{
  if (s_hintState == 2) return;
  if (currentPlatform == PLATFORM_SDMANAGER) { s_hintState = 2; return; }   // its screen says it
  if (s_hintState == 0) {
    // Armed on the first frame after the splash closes, not in videoSetup(), because the splash
    // owns the whole panel and would be sitting on top of the hint for as long as it is up.
    s_hintKeyed = false;                      // the keypress that dismissed the splash is not ours
    const char *keys, *joy;
    hintText(&keys, &joy);
    hintClearBar();
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(tft.color565(200, 200, 90));
    tft.drawPanelString(keys, kHintW / 2, kHintTop + 12, 1);
    tft.setTextColor(tft.color565(150, 160, 175));
    tft.drawPanelString(joy,  kHintW / 2, kHintTop + 27, 1);
    s_hintUntil = millis() + 5000;
    s_hintState = 1;
    return;
  }
  if (!s_hintKeyed && (int32_t)(millis() - s_hintUntil) < 0) return;
  hintClearBar();
  s_hintState = 2;
}
#else
void bootHintTick() {}
void bootHintDismiss() {}
#endif

void videoSetup()
{
  bootProgressEnd();   // the render loop below owns the panel from here on
  printLog("Video Setup...");
  // Decide whether the boot splash shows this run (see splashOnBootMagic above): a soft reboot
  // (ESP.restart) skips it UNLESS the magic was set; any hardware reset (power-on / RST) shows it.
  bool softReboot = (esp_reset_reason() == ESP_RST_SW);
  splashActive = !softReboot || (splashOnBootMagic == SPLASH_ON_BOOT_MAGIC);
  splashOnBootMagic = 0;   // consume the request; the next plain reboot then skips the splash
  tft.begin();
  tft.setRotation(3);
  tft.invertDisplay(true);
  tft.initDMA();
  tft.fillRect(0, 0, 320, 240, TFT_BLACK);
#if defined(BOARD_DESKTOP)
  // Desktop: the render loop runs on the MAIN thread (SDL window/events/present must live there);
  // main() calls renderLoop(NULL) after spawning the CPU thread. Don't spawn it as a task here.
  printLog("video: renderLoop runs on main thread (desktop)");
#else
  TaskHandle_t h = xTaskCreateStaticPinnedToCore(renderLoop, "renderLoop", RENDER_TASK_STACK, NULL, 1,
                                                 renderTaskStack, &renderTaskTCB, 0); // core 0
  printLog(h ? "video: renderLoop started" : "video: renderLoop FAILED");
#endif

  // The render task (core 0) owns all the settings-window UI, and that includes BLOCKING SD
  // I/O: directory scans for the file browser and loading disk/PRG/CRT/D64 images. A single
  // Arduino openNextFile() fopens the entry and can take 1-5+ seconds per call on a slow SD
  // card / subdirectory (it busy-waits, so it can't yield mid-call). That starves the core-0
  // idle task and the 5s task watchdog reboots the board mid-SD-transaction (which then wedges
  // the card -> "Card Mount Failed" until a power cycle). Since core 0 legitimately blocks on
  // SD by design, drop the idle-task WDT for this core. Core 1 (the CPU core) keeps its WDT.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // IDF 5.x: disableCore0WDT() DELETES the idle task from the TWDT, but its idle hook keeps calling
  // esp_task_wdt_reset() -> a "task not found" flood every idle tick. Instead RECONFIGURE the TWDT:
  // stop monitoring the idle tasks (idle_core_mask=0 removes the hooks too) and make it non-panic
  // with a long timeout, so the render loop's multi-second SD blocking can never reboot the board.
  esp_task_wdt_config_t twdt = {};
  twdt.timeout_ms     = 60000;
  twdt.idle_core_mask = 0;
  twdt.trigger_panic  = false;
  esp_task_wdt_reconfigure(&twdt);
#else
  disableCore0WDT();
#endif
}

int red(int color) {
  return (color & 0xf800) >> 8;
}
int green(int color) {
  return (color & 0x7e0) >> 3;
}
int blue(int color) {
  return (color & 0x1f) << 3;
}

bool inversed = false;
float screen_width = 280;
float screen_height = 192;
uint16_t last_y = 0;
uint16_t last_x = 0;

// Boot splash + system selector. Shows the logo and one button per system; the running one is
// highlighted. Tapping a system selects it (switching saves to EEPROM and reboots so setup() can
// init the new core); tapping elsewhere, a joystick button, or the timeout boots the current one.
// Runs on core 0 from renderLoop (which owns the TFT).
//
// The button list is NOT the Platform enum any more. Apple II+ and Apple IIe are two entries on
// one core: they differ only in which memory map memoryAlloc() builds and whether iie.bin is
// loaded, and since that is now decided once at startup for the selected machine -- rather than
// both maps being allocated so a MACHINE toggle could flip between them -- the model has to be
// chosen here, like any other system. Every other entry is one platform with iie = -1 ("n/a").
//
// Eleven buttons do not fit one row of the 320px panel at a readable width, so they sit in two:
// SPLASH_COLS across, the second row centred under the first. The logo and the SELECT SYSTEM line
// moved up to make the room.
#define SPLASH_MS      12000   // generous: time to read the menu and tap a system
#define SPLASH_LOGO_Y  22
#define SPLASH_SUB_Y   122     // "SELECT SYSTEM" / "Loading..." line (centre)
#define SPLASH_BTN_Y   136     // top of the first row
#define SPLASH_BTN_H   40
#define SPLASH_ROW_GAP 4
#define SPLASH_COLS    6
struct SplashSystem { const char *label; uint8_t platform; int8_t iie; const char *why; };
static const SplashSystem splashSystems[] = {
  { "II+",   PLATFORM_APPLE2,   0, "SOON"   },
  { "IIe",   PLATFORM_APPLE2,   1, "NO RAM" },   // greyed once memoryAlloc has found it does not fit
  { "C64",   PLATFORM_C64,     -1, "SOON"   },
  { "NES",   PLATFORM_NES,     -1, "SOON"   },
  { "ATARI", PLATFORM_ATARI,   -1, "SOON"   },
  { "IIGS",  PLATFORM_IIGS,    -1, "SOON"   },
  { "MSX",   PLATFORM_MSX,     -1, "SOON"   },
  { "SMS",   PLATFORM_SMS,     -1, "SOON"   },
  { "PCXT",  PLATFORM_PCXT,    -1, "SOON"   },
  { "386",   PLATFORM_TINY386, -1, "SOON"   },
  { "SD MGR", PLATFORM_SDMANAGER, -1, "N/A"  },   // not an emulator: the SD card file manager
};
#define SPLASH_N     ((int)(sizeof(splashSystems) / sizeof(splashSystems[0])))
#define SPLASH_PITCH (320 / SPLASH_COLS)   // 53px pitch,
#define SPLASH_BTN_W (SPLASH_PITCH - 2)    // 51px wide, 2px gap

// Top-left corner of button i. A row shorter than SPLASH_COLS (the last one) is centred.
static void splashBtnPos(int i, int *x, int *y)
{
  int row = i / SPLASH_COLS, col = i % SPLASH_COLS;
  int inRow = SPLASH_N - row * SPLASH_COLS;
  if (inRow > SPLASH_COLS) inRow = SPLASH_COLS;
  *x = (320 - inRow * SPLASH_PITCH) / 2 + col * SPLASH_PITCH + 1;
  *y = SPLASH_BTN_Y + row * (SPLASH_BTN_H + SPLASH_ROW_GAP);
}

static int splashHitTest(int16_t x, int16_t y)
{
  for (int i = 0; i < SPLASH_N; i++) {
    int bx, by;
    splashBtnPos(i, &bx, &by);
    if (x >= bx && x < bx + SPLASH_BTN_W && y >= by && y < by + SPLASH_BTN_H) return i;
  }
  return -1;
}

// Which platforms this BOARD can actually run. IIGS / PC-XT / tiny386 each want 1-4MB of
// ps_malloc'd guest RAM, so they only exist where BOARD_HAS_BIGRAM_CORES is set (see the
// matching #if gates in emu8.ino, which also keep their cores out of the link); tiny386
// additionally only builds on the P4 / desktop. MSX / SMS go the same way wherever
// BOARD_HAS_MSX_CORE / BOARD_HAS_SMS_CORE is clear. Disabled buttons draw greyed with "SOON" and ignore taps.
static bool splashEnabled(int i)
{
  // The Apple IIe is the one entry whose availability is not a property of the build: its map plus
  // 30K of system ROM is ~119K of heap, which some boards have and the RP2040 does not. memoryAlloc()
  // is what finds out, by trying, so the first IIe boot on a board that cannot hold one comes up as
  // a II+ and sets this; from then on the button is greyed "NO RAM" instead of lying.
  if (splashSystems[i].iie > 0 && apple2IIeUnavailable) return false;
  switch (splashSystems[i].platform) {
    case PLATFORM_IIGS:
    case PLATFORM_PCXT:     return BOARD_HAS_BIGRAM_CORES;
    case PLATFORM_MSX:      return BOARD_HAS_MSX_CORE;
    case PLATFORM_SMS:      return BOARD_HAS_SMS_CORE;
    case PLATFORM_TINY386:
#if defined(BOARD_JC1060P470) || defined(BOARD_DESKTOP)
      return BOARD_HAS_BIGRAM_CORES;   // i386 PC: P4 / desktop only
#else
      return false;                    // S3 (not built here) / CYD (no PSRAM)
#endif
    case PLATFORM_SDMANAGER:
#if defined(BOARD_DESKTOP)
      return false;                    // the "card" is a host folder already (sdserial.cpp)
#else
      return true;
#endif
    default:                return true;
  }
}

// Which button the running system is. The Apple entries match on AppleIIe as well, so a II+
// session highlights II+ and a IIe session highlights IIe.
static int splashCurrentIndex()
{
  for (int i = 0; i < SPLASH_N; i++)
    if (splashSystems[i].platform == currentPlatform &&
        (splashSystems[i].iie < 0 || (splashSystems[i].iie != 0) == (bool)AppleIIe))
      return i;
  return 0;
}

// Which button is highlighted. -1 means "follow the running system", which is what touch boards
// always use; the PicoCalc's keyboard nav below moves it independently before committing.
static int splashCursor = -1;
static inline int splashHighlight() { return splashCursor < 0 ? splashCurrentIndex() : splashCursor; }

static void splashDrawBtn(int i, bool enabled)
{
  const char *label = splashSystems[i].label;
  bool active = (i == splashHighlight());
  int x, y, w = SPLASH_BTN_W, h = SPLASH_BTN_H;
  splashBtnPos(i, &x, &y);
  uint16_t face = !enabled ? tft.color565(28, 30, 38)
                : active   ? tft.color565(0, 120, 215)
                           : tft.color565(44, 48, 60);
  tft.fillRoundRect(x, y, w, h, 6, face);
  tft.drawRoundRect(x, y, w, h, 6, active ? TFT_WHITE : tft.color565(70, 78, 92));
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(enabled ? TFT_WHITE : tft.color565(110, 118, 130), face);
  int lblFont = ((int)strlen(label) * 12 <= w - 2) ? 2 : 1;   // font 2 is 12px/char, font 1 is 6px
  tft.drawString(label, x + w / 2, enabled ? y + h / 2 : y + h / 2 - 6, lblFont);
  if (!enabled) {
    tft.setTextColor(tft.color565(110, 118, 130), face);
    tft.drawString(splashSystems[i].why, x + w / 2, y + h - 12, 1);
  }
}

static void splashFinish()           // boot the current platform
{
  splashActive = false;
  clearScr = true;                   // wipe the whole panel before the emulator video starts
}

static void splashSelect(int idx)
{
  // Deselect the previously highlighted button and mark the tapped one, swap the subtitle for a
  // loading message, hold it for a second, then close the splash (or reboot if the system
  // changed, since setup() must re-init the new core / rebuild the Apple memory map).
  int     prevIdx  = splashCurrentIndex();
  uint8_t prevPlat = currentPlatform;
  bool    prevIIe  = AppleIIe;
  currentPlatform = splashSystems[idx].platform;   // drives the highlight in splashDrawBtn below
  if (splashSystems[idx].iie >= 0) AppleIIe = (splashSystems[idx].iie != 0);
  splashCursor = -1;                   // ...so stop overriding it with the keyboard cursor
  if (prevIdx != idx) splashDrawBtn(prevIdx, splashEnabled(prevIdx));
  splashDrawBtn(idx, splashEnabled(idx));
  tft.fillRect(0, SPLASH_SUB_Y - 10, 320, 20, TFT_BLACK);   // wipe the "SELECT SYSTEM" subtitle
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(tft.color565(0, 200, 120), TFT_BLACK);
  tft.drawString("Loading...", 160, SPLASH_SUB_Y, 2);
  displayFlush();
  delay(1000);

  if (currentPlatform == PLATFORM_SDMANAGER) {
    // Always a reboot, and never a saved platform: saveConfig() keeps the emulator's byte in
    // EEPROM (eprom.cpp) and the one-boot flag carries the choice across the restart instead.
    saveConfig();
    requestSdManagerOnNextBoot();
    ESP.restart();
  }

  if (prevPlat == currentPlatform && prevIIe == (bool)AppleIIe) { splashFinish(); return; }
  // Switching systems needs a reboot to re-init. ESP.restart() (an on-chip reset from firmware)
  // reboots this board cleanly - unlike the host-side RTS reset which wedges it. currentPlatform
  // and AppleIIe are already the new ones; persist them so setup() builds the saved system.
  saveConfig();
  ESP.restart();
}

static void splashService()
{
  static bool drawn = false;
  static unsigned long startMs = 0;
  if (!drawn) {
    startMs = millis();
    tft.fillScreen(TFT_BLACK);
    tft.setSwapBytes(true);
    tft.pushImage((320 - BOOT_LOGO_W) / 2, SPLASH_LOGO_Y, BOOT_LOGO_W, BOOT_LOGO_H, bootLogo);
    tft.setSwapBytes(false);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(tft.color565(150, 160, 175), TFT_BLACK);
    tft.drawString("SELECT SYSTEM", 160, SPLASH_SUB_Y, 2);
    for (int i = 0; i < SPLASH_N; i++) splashDrawBtn(i, splashEnabled(i));
    drawn = true;
  }

  int16_t tx, ty;
  if (touchRead(&tx, &ty)) {
    int b = splashHitTest(tx, ty);
    if (b < 0)                 splashFinish();      // tapped outside -> boot current
    else if (splashEnabled(b)) splashSelect(b);     // a "SOON" button just ignores the tap
    return;
  }

#if defined(BOARD_PICOCALC)
  // No touch panel here, so the splash is driven from the keyboard instead: left/right move
  // the highlight over the enabled platforms, up/down jump between the two rows, Enter commits
  // (the same path a tap takes). Any nav key also restarts the timeout, so browsing the list
  // cannot boot out from under you.
  int8_t ev = splashKeyEvent;
  splashKeyEvent = 0;
  if (ev) {
    int cur = splashHighlight();
    if (ev == SPLASH_KEY_SELECT) { if (splashEnabled(cur)) splashSelect(cur); return; }
    int n = cur;
    if (ev == SPLASH_KEY_UP || ev == SPLASH_KEY_DOWN) {
      int t = cur + (ev == SPLASH_KEY_DOWN ? SPLASH_COLS : -SPLASH_COLS);
      if (t >= SPLASH_N) t = SPLASH_N - 1;               // the bottom row is the shorter one
      if (t >= 0 && splashEnabled(t)) n = t;             // a SOON button there: stay put
    } else {
      do { n = (n + ev + SPLASH_N) % SPLASH_N; } while (!splashEnabled(n) && n != cur);   // skip SOON
    }
    if (n != cur) {
      splashCursor = n;
      splashDrawBtn(cur, splashEnabled(cur));
      splashDrawBtn(n,   splashEnabled(n));
    }
    startMs = millis();
    return;
  }
#endif
  if (millis() - startMs >= SPLASH_MS || Pb0 || Pb1 || Pb2 || Pb3) splashFinish();
}

// SD Manager mode (PLATFORM_SDMANAGER): no emulator runs, the USB serial port belongs to the
// file-manager server (sdserial.cpp), and the panel just says how to use it and what state it is
// in. Redrawn only when that state changes.
static void sdManagerRender()
{
  static int shown = -1;
  int state = (sdSerialActive ? 1 : 0) | (sdCardMounted ? 2 : 0);
  if (clearScr) { shown = -1; clearScr = false; }
  if (state != shown) {
    shown = state;
    uint16_t dim = tft.color565(150, 160, 175);
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("SD MANAGER", 160, 34, 2);
    tft.setTextColor(dim, TFT_BLACK);
    tft.drawString("Connect this board over USB and open", 160, 72, 2);
    tft.drawString("the emu8 SD Manager web app", 160, 90, 2);
    tft.drawString("(or emu8sd.py) on the computer.", 160, 108, 2);
    if (!sdCardMounted) {
      tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK);
      tft.drawString("SD card not mounted", 160, 146, 2);
    } else if (sdSerialActive) {
      tft.setTextColor(tft.color565(0, 200, 120), TFT_BLACK);
      tft.drawString("Host connected", 160, 146, 2);
    } else {
      tft.setTextColor(tft.color565(230, 180, 40), TFT_BLACK);
      tft.drawString("Waiting for a host...", 160, 146, 2);
    }
    tft.setTextColor(dim, TFT_BLACK);
#if defined(BOARD_PICOCALC)
    tft.drawString("Ctrl-F6: back to the system menu", 160, 200, 2);
#else
    tft.drawString("Tap the screen: back to the system menu", 160, 200, 2);
#endif
  }
  // Leaving is a reboot into the splash, like Ctrl-F6 is on the PicoCalc (input_picocalc.cpp):
  // the emulator the user comes back to is set up from scratch, with the whole heap.
  int16_t tx, ty;
  if (touchRead(&tx, &ty)) { requestSplashOnNextBoot(); ESP.restart(); }
}

#if defined(BOARD_PICOCALC)
// Bring-up instrumentation. A black panel with a live emulator (audio playing, 6502 running)
// is indistinguishable from a render task that was created but never scheduled, so say once a
// second which core this task is actually on, how many passes it completed, and which branch
// of the loop below it is taking. Costs one sprintf per second.
static void renderHeartbeat()
{
  static uint32_t frames = 0, lastMs = 0;
  frames++;
  uint32_t now = millis();
  if (now - lastMs < 1000) return;
  lastMs = now;
  char m[96];   // local, not the shared `buf`: this runs on the other core from setup()/loop()
  sprintf(m, "video: %lu fps core=%u splash=%d opts=%d plat=%d clr=%d",
          (unsigned long)frames, (unsigned)get_core_num(), (int)splashActive,
          (int)OptionsWindow, (int)currentPlatform, (int)clearScr);
  printLog(m);
  frames = 0;
}
#endif

void renderLoop(void *pvParameters)
{
#if defined(BOARD_PICOCALC)
  {
    char m[64];
    sprintf(m, "video: renderLoop task entered on core %u", (unsigned)get_core_num());
    printLog(m);
  }
#endif

  while (running)
  {
#if defined(BOARD_PICOCALC)
    renderHeartbeat();   // FIRST in the body: a heartbeat that stops tells us where it hung
#endif
    // Push the previous iteration's frame to the panel. On Arduino_GFX boards the cores draw
    // into a PSRAM canvas and this streams it over QSPI; on TFT_eSPI boards it is a no-op
    // (drawing went straight to the panel). Flushing at the top covers every render path below,
    // each of which ends in its own `continue`.
    displayFlush();

#if BOARD_DISPLAY_GFX
    // When a full-screen menu just closed, request a full-panel wipe so no UI remnants linger in
    // the black border around the centered emulator video. (TFT_eSPI boards have no border.)
    static bool prevOptions = false;
    if (prevOptions && !OptionsWindow) clearScr = true;
    prevOptions = OptionsWindow;
    // Reset the NES direct-to-panel bypass each iteration. The NES video branch re-arms it after it
    // pushes its frame straight to the panel, so the displayFlush() above no-ops on the frame AFTER a
    // direct NES draw (nothing in the canvas to flush) but runs normally for UI / menus / C64 / Atari.
    tft.setBypassCanvas(false);
#endif
#if defined(BOARD_PICOCALC)
    // The settings menu draws on the whole 320x320 panel (optionsui.cpp). Once it has closed, go
    // back to the letterboxed 320x240 and blacken the bars it drew over; the logical area is
    // repainted by the platform code below (showHideOptionsWindow() already set clearScr).
    if (!OptionsWindow && tft.setFullPanel(false)) tft.fillPanelBlack();
#endif

    Vertical_blankingOn_Off = false;
    unsigned long startTime = millis();

    // Boot splash takes over the screen until it times out / is dismissed.
    if (splashActive)
    {
      displaySetUiMode(true);
      splashService();
      vTaskDelay(pdMS_TO_TICKS(15));
      continue;
    }

    bootHintTick();    // first frame past the splash puts the hint up; five seconds later, down

    // Everything below the splash draws in UI mode by default (full-screen menus, warnings, the
    // on-screen keyboard); the per-platform emulator-video sections switch to video mode (centered
    // 320x240) just before they draw, and back to UI for any keyboard overlay.
    displaySetUiMode(true);

    if (currentPlatform == PLATFORM_SDMANAGER)
    {
      sdManagerRender();
      Vertical_blankingOn_Off = true;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // NES startup ROM-skip warning: hold a full-screen note (skipped/over-budget ROMs) for a few
    // seconds after boot, before normal rendering / touch handling kicks in.
    if (currentPlatform == PLATFORM_NES && nesRenderLoadWarning())
    {
      Vertical_blankingOn_Off = true;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // Atari 2600 startup ROM-skip warning (same idea as the NES overlay above).
    if (currentPlatform == PLATFORM_ATARI && atariRenderLoadWarning())
    {
      Vertical_blankingOn_Off = true;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // MSX startup overlay: hard error if no BIOS, or a brief "C-BIOS = no Disk BASIC" note.
    if (currentPlatform == PLATFORM_MSX && msxRenderLoadWarning())
    {
      Vertical_blankingOn_Off = true;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // SMS startup overlay: held while no .sms/.bin ROM is loaded. Still poll touch so a tap opens
    // SETTINGS (to pick a ROM); smsRenderLoadWarning() yields to the options window once it opens.
    if (currentPlatform == PLATFORM_SMS && smsRenderLoadWarning())
    {
      oskPoll();                       // a screen tap opens SETTINGS even while the notice is up
      Vertical_blankingOn_Off = true;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // Apple II startup overlay: held when the system ROMs are missing from /roms/apple2 on the SD card.
    if (currentPlatform == PLATFORM_APPLE2 && apple2RenderLoadWarning())
    {
      Vertical_blankingOn_Off = true;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // On-screen touch keyboard: poll touch every frame so its state is current
    // before we pick the raster geometry below.
    oskPoll();
#if BOARD_PANEL_DSI
    osgRender();   // redraw the on-screen virtual gamepad overlay (no-op unless NES/Atari/SMS + dirty)
#endif

    // Modern touch-driven settings window takes over the whole screen (CPU paused).
    if (OptionsWindow)
    {
      optionsUiPoll();
      optionsUiRender();
      Vertical_blankingOn_Off = true;
      vTaskDelay(pdMS_TO_TICKS(15));
      continue;
    }

    // C64 core renders its own framebuffer (push to TFT); skip the Apple raster.
    if (currentPlatform == PLATFORM_C64)
    {
#if BOARD_DISPLAY_GFX
      if (clearScr) { tft.fillScreen(TFT_BLACK); clearScr = false; }   // wipe border after a menu
#endif
      displaySetUiMode(false);
      displaySetVideoRect(20, 200);      // VIC-II screen is 200 lines (logical y 20..220); fill-screen scales that
      displaySetVideoFill(0, 320, true); // fill-screen stretches the full-width 320x200 picture to 100% (no aspect/bars)
      c64RenderFrame();                  // text screen (top 14 rows when the OSK is open)
      if (oskActive()) { displaySetUiMode(true); oskRender(); }  // keyboard owns the bottom (UI)
      Vertical_blankingOn_Off = true;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // NES core renders its own 256x240 framebuffer (filled by the PPU on the CPU core);
    // convert + push it here, pillarboxed in the 320-wide panel.
    if (currentPlatform == PLATFORM_NES)
    {
#if BOARD_DISPLAY_GFX
      // Wipe the WHOLE panel (not just the canvas) after a menu: the NES fast path bypasses the canvas
      // flush, so a canvas-only fillScreen would never reach the panel and menu remnants would linger.
      if (clearScr) { tft.fillScreen(TFT_BLACK); tft.fillPanelBlack(); clearScr = false; }
      // Display-skip: present the panel only every Nth core-0 pass. Skipping the convert+push frees
      // core-0 bus time that was contending with the core-1 interpreter, so the GAME runs faster at
      // the cost of a choppier picture. 1 = every frame (smoothest); 2-3 = faster game, lower visual fps.
      static uint32_t nesDispCtr = 0;
      uint8_t skipN = (nesDisplaySkip < 1) ? 1 : nesDisplaySkip;   // global, set in NES options; guard /0
      bool nesDraw = ((nesDispCtr++ % skipN) == 0);
#else
      bool nesDraw = true;
#endif
      displaySetUiMode(false);
      displaySetVideoRect(0, 240);       // NES fills the full 240 height (only pillarboxed horizontally)
      if (nesDraw) nesRenderFrame();
#if BOARD_DISPLAY_GFX
      else tft.setBypassCanvas(true);    // skipped frame: keep the flush a no-op, leave the last frame up
#endif
      Vertical_blankingOn_Off = true;
      // Unlike the other cores, nesRenderFrame() already blocks until the PPU hands over a fresh
      // picture, so it paces this loop by itself. A fixed 10ms nap on top of that was pure latency
      // -- a fifth of the whole display frame time. Yield one tick so the idle task still runs.
      vTaskDelay(1);
      continue;
    }

    // Atari 2600 core renders its own 160x192+ framebuffer (filled by the TIA on the CPU core);
    // convert + push it here, doubled to 320 wide with top/bottom borders.
    if (currentPlatform == PLATFORM_ATARI)
    {
#if BOARD_DISPLAY_GFX
      if (clearScr) { tft.fillScreen(TFT_BLACK); clearScr = false; }   // wipe border after a menu
#endif
      displaySetUiMode(false);
      displaySetVideoRect(24, 192);      // TIA picture is 192 lines centered in 240 (24px borders)
      displaySetVideoFill(0, 320, true); // fill-screen stretches the full-width 320x192 picture to 100% (no aspect/bars)
      atariRenderFrame();
      Vertical_blankingOn_Off = true;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // MSX1 core: the Z80 (core 1) runs the BIOS/BASIC; the VDP fills a 256x192 framebuffer that we
    // convert + push here, centered with 32px side borders + 24px top/bottom (like NES/Atari).
    if (currentPlatform == PLATFORM_MSX)
    {
#if BOARD_DISPLAY_GFX
      if (clearScr) { tft.fillScreen(TFT_BLACK); clearScr = false; }   // wipe border after a menu
#endif
      displaySetUiMode(false);
      displaySetVideoRect(24, 192);      // 192 active lines centered in 240
      displaySetVideoFill(32, 256, true);// 256-wide picture starting at x=32
      msxRenderFrame();
      if (oskActive()) { displaySetUiMode(true); oskRender(); }   // on-screen keyboard overlays the bottom
      Vertical_blankingOn_Off = true;
#if defined(BOARD_PICOCALC)
      vTaskDelay(1);                     // the ~16ms SPI push already paces us; a 10ms nap on top drops frames
#else
      vTaskDelay(pdMS_TO_TICKS(10));
#endif
      continue;
    }

    // SMS core: the Z80 (core 1) runs the cartridge; the VDP fills a 256x192 Mode 4 framebuffer that we
    // convert (via the live CRAM palette) + push here, centered with 32px side borders (like MSX).
    if (currentPlatform == PLATFORM_SMS)
    {
#if BOARD_DISPLAY_GFX
      if (clearScr) { tft.fillScreen(TFT_BLACK); clearScr = false; }   // wipe border after a menu
#endif
      displaySetUiMode(false);
      displaySetVideoRect(24, 192);      // 192 active lines centered in 240
      displaySetVideoFill(32, 256, true);// 256-wide picture starting at x=32
      smsRenderFrame();
      Vertical_blankingOn_Off = true;
#if defined(BOARD_PICOCALC)
      vTaskDelay(1);                     // the ~16ms SPI push already paces us; a 10ms nap on top drops frames
#else
      vTaskDelay(pdMS_TO_TICKS(10));
#endif
      continue;
    }

#if BOARD_HAS_BIGRAM_CORES
    // Apple IIGS: the 65C816 (core 1) runs the firmware; draw its 40-col text page here.
    if (currentPlatform == PLATFORM_IIGS)
    {
#if BOARD_DISPLAY_GFX
      if (clearScr) { tft.fillScreen(TFT_BLACK); clearScr = false; }
#endif
      iigsRenderText();                  // UI-mode text (sets fillScreen + drawString); flush at loop top
      if (oskActive()) { displaySetUiMode(true); oskRender(); }   // on-screen keyboard overlays the bottom
      Vertical_blankingOn_Off = true;
      vTaskDelay(pdMS_TO_TICKS(33));      // ~30 fps text refresh
      continue;
    }

    // PC-XT: the 8086 (core 1) runs the BIOS/DOS; render the CGA buffer here. pcxtRenderFrame() returns
    // false when the picture is UNCHANGED -> we then skip the QSPI flush (setBypassCanvas), so core 0
    // stops draining the shared MSPI bus and the 8086 runs much faster while the screen is static.
    if (currentPlatform == PLATFORM_PCXT)
    {
#if BOARD_DISPLAY_GFX
      if (clearScr) { tft.fillScreen(TFT_BLACK); tft.fillPanelBlack(); clearScr = false; pcxtForceRedraw(); }
#endif
      bool drew = pcxtRenderFrame();
      if (oskActive()) { displaySetUiMode(true); oskRender(); drew = true; }
#if BOARD_DISPLAY_GFX
      if (!drew) tft.setBypassCanvas(true);   // nothing changed -> no flush this round (free the bus)
#endif
      Vertical_blankingOn_Off = true;
#if defined(BOARD_DESKTOP)
      vTaskDelay(pdMS_TO_TICKS(4));   // desktop: present is vsync-paced; the device's 16-40ms throttle
                                      // (shared MSPI bus) just adds input lag here — keep it snappy
#else
      vTaskDelay(pdMS_TO_TICKS(drew ? 16 : 40));
#endif
      continue;
    }

    // tiny386 (Intel i386 + VGA): the i386 (core 1) runs SeaBIOS/DOS/Windows; nearest-scale the VGA
    // framebuffer onto the panel here. tiny386RenderFrame() returns false when the picture is
    // UNCHANGED -> skip the QSPI flush so core 0 stops draining the shared bus (like PC-XT).
    if (currentPlatform == PLATFORM_TINY386)
    {
#if BOARD_DISPLAY_GFX
      if (clearScr) { tft.fillScreen(TFT_BLACK); tft.fillPanelBlack(); clearScr = false; tiny386ForceRedraw(); }
#endif
      bool vgaDrew = tiny386RenderFrame();
      bool osk = oskActive();
      bool oskChanged = false;
      if (osk) { displaySetUiMode(true); oskChanged = oskDirty(); oskRender(); }  // oskDirty before oskRender clears it
#if BOARD_PANEL_DSI
      if (osk && oskChanged && !vgaDrew) {
        tft.flushOskBand();          // only the keyboard changed -> push just its band (cheap), and
        tft.setBypassCanvas(true);   // skip the loop-top full 1024x600 composite (that made it laggy)
      } else
#endif
      {
#if BOARD_DISPLAY_GFX
        if (!(vgaDrew || oskChanged)) tft.setBypassCanvas(true);   // nothing changed -> skip the flush
#endif
      }
      Vertical_blankingOn_Off = true;
      // Keyboard open: poll touch fast (12ms) so it stays responsive -- an idle frame is cheap (flush is
      // gated above). Otherwise cap the render rate (~22 fps): a full 1024x600 frame costs a lot of PSRAM
      // bandwidth and at 60 fps saturates the bus the i386 (core 1) shares, so throttling frees the CPU.
#if defined(BOARD_DESKTOP)
      vTaskDelay(pdMS_TO_TICKS(4));   // desktop: vsync-paced present; skip the device's 45-80ms bus-relief
                                      // throttle so input + the VGA display stay responsive
#else
      vTaskDelay(pdMS_TO_TICKS(osk ? 12 : ((vgaDrew || oskChanged) ? 45 : 80)));
#endif
      continue;
    }
#endif // BOARD_HAS_BIGRAM_CORES

    // 320x240 TFT: center the 280x192 raster (overriding the S3/VGA margins below).
    // When the touch keyboard is open, squeeze the raster into the top rows so the
    // emulated screen stays live above the keyboard; the vertical scaler (coef192)
    // and the matching setAddrWindow height handle the squeeze automatically.
    displaySetUiMode(false);   // Apple raster is emulator video: centered 320x240 on the panel
    margin_x = 20;
    margin_y = oskRasterTop();
    screen_width = 280;
    screen_height = oskRasterHeight();
    displaySetVideoRect(margin_y, (int)screen_height);   // Apple raster (192 lines at margin_y); fill-screen scales just that
    last_y = margin_y;
    last_x = margin_x;

    float coef192 = screen_height / 192;
    float coef560 = screen_width / 560;
    float coef280 = screen_width / 280;
    float coef140 = screen_width / 140;

    int rasterH = (int)screen_height; // matches the line count produced by coef192

    // clearScr is tested FIRST: the wipe below writes exactly 320*240 pixels, so the window has
    // to be the full 320x240 or the surplus wraps back to the window origin and shifts the frame
    // down by the overrun. The 40-col branch used to win this race on an AppleIIe at boot (its
    // window is 320x192 = 61,440 px, 15,360 short of the clear) and cost a 48-row offset.
    if (OptionsWindow || clearScr)
    tft.setAddrWindow(0, 0, 320, 240);
    else if (AppleIIe && !Cols40_80 && !DHiResOn_Off)
    tft.setAddrWindow(0, margin_y, 320, rasterH); // Set the area to draw
    else if (AppleIIe && DHiResOn_Off && !videoColor)
    tft.setAddrWindow(margin_x, margin_y, 280, rasterH); // DHiRes mono (centered 280)
    else
    tft.setAddrWindow(margin_x, margin_y, 280, rasterH);
    // fill-screen: stretch the raster to 100% of the panel (no 4:3 aspect, no side bars). Mirror the
    // horizontal extent of the addr window above so we sample exactly the columns the raster filled
    // (full 320 for the 40-col branch; the centered 280 at margin_x for the graphics/80-col branches).
    if (!OptionsWindow && AppleIIe && !Cols40_80 && !DHiResOn_Off) displaySetVideoFill(0, 320, true);
    else                                                           displaySetVideoFill(margin_x, (int)screen_width, true);
    tft.startWrite();
    
    

    int x = margin_x;
    int y = margin_y;
    int x_upscaled = margin_x;
    int y_upscaled = margin_y;
    // Snapshot the page selection under a brief lock so a CPU page-flip (C054/C055)
    // can't block for a whole frame; the rest of the frame renders from these locals.
    page_lock.lock();
    ushort textPage = Page1_Page2 ? 0x400 : 0x800;
    ushort graphicsPage = Page1_Page2 ? 0x2000 : 0x4000;
    page_lock.unlock();
    unsigned long endTime1 = millis();
    // if (demo) {
    // y=0;
    //   for (int v = 0; v < 30; v++)
    //   {
    //     for (int i = 0; i < 8; i++) // char lines
    //     {
    //       x=0;
    //       for (int h = 0; h < 45; h++)
    //       {
    //         uint8_t chr = menuScreen[v * 45 + h];
    //         for (int c = 0; c < 7; c++) // char cols
    //         {
    //           bool bpixel = AppleIIeFontPixels[(chr*7*8) + (i * 7) + c];
    //           uint8_t color = menuColor[v * 45 + h];
    //           uint8_t fgColor = 0xff;
    //           uint8_t bgColor = color;
    //           #ifdef TFT
    //           tft.writeColor((bpixel ? fgColor : bgColor), 1);
    //           #else
    //           vga.dotFast(x, y, bpixel ? fgColor : bgColor);
    //           x++;
    //           vga.dotFast(x, y, bpixel ? fgColor : bgColor);
    //           #endif
    //           x++;
    //         }
    //       }
    //       y++;
    //     }
    //   }
    // }
    if (clearScr) {
#if BOARD_DISPLAY_GFX
      // Clear the WHOLE panel (incl. the border around the centered video) so closing the options
      // window / leaving the splash leaves no remnants. (Canvas fill, no SPI transaction.)
      tft.fillScreen(colors[0]);
#else
      // CYD: clear the 320x240 panel within the active addr-window write transaction.
      tft.writeColor(colors[0], (uint32_t)320 * 240);
#endif
      clearScr = false;
    }
    else if (OptionsWindow || DebugWindow) 
    {
      y=0;
      for (int v = 0; v < 30; v++)
      {
        for (int i = 0; i < 8; i++) // char lines
        {
          x=0;
          for (int h = 0; h < 45; h++)
          {
            uint8_t chr = menuScreen[v * 45 + h];
            for (int c = 0; c < 7; c++) // char cols
            {
              bool bpixel = AppleIIeFontPixels[(chr*7*8) + (i * 7) + c];
              uint8_t color = menuColor[v * 45 + h];
              uint8_t fgColor = (color & 0xf0) >> 4;
              uint8_t bgColor = (color & 0x0f);
                tft.writeColor((bpixel ? colors16[fgColor] : colors16[bgColor]), 1);
              x++;
            }
          }
          y++;
        }
      }
    }
    else
    {
      for (int b = 0; b < 3; b++)
      {
        for (int l = 0; l < 8; l++)
        {
          if ((Graphics_Text && DisplayFull_Split) || (Graphics_Text && !DisplayFull_Split && (b < 2 || (b == 2 && l < 4))))
          {
            if (LoRes_HiRes)
            {
              for (int j = 0; j < 8; j++)
              {
                // Init Upscale
                int repeat_y = 0;
                uint16_t upscaleCoef_y = floor(coef192 * (float)(y+1));
                uint16_t repeatTimes_y = upscaleCoef_y - last_y;
                last_y = upscaleCoef_y;
                while (repeat_y < repeatTimes_y)
                {
                  x_upscaled = margin_x;
                  x = margin_x;
                  last_x = margin_x;
                  uint16_t lastPixel = 0;
                  bool lastLine = false;
                  bool lastCol = false;
                  // End Upscale
                  for (int c = 0; c < 0x28; c++)
                  {
                    for (int k = 0; k < 7; k++)
                    {
                      char value = ram[(textPage + (b * 0x28) + (l * 0x80) + c)];
                      int firstColor = (value & 0b11110000) >> 4;
                      int secondColor = value & 0b00001111;
                        if (j < 4)
                          tft.writeColor(colors16[secondColor], 1);
                        else
                          tft.writeColor(colors16[firstColor], 1);
                      
                      x++;
                    }
                  }
                  repeat_y++;
                  y_upscaled++;
                }
                y++;
              }
            }
            else if (DHiResOn_Off)
            {
              static bool line[0x50 * 7]; // 560 DHGR bits; static avoids per-scanline malloc churn

              for (int block = 0; block < 8; block++)
              {
                // Init Upscale
                int repeat_y = 0;
                uint16_t upscaleCoef_y = floor(coef192 * (float)(y+1));
                //Serial.printf("upscaleCoef_y=%d screen_width=%f (float)(y+1)=%f last_y=%d\n",upscaleCoef_y, screen_width, (float)(y+1), last_y);
                uint16_t repeatTimes_y = upscaleCoef_y - last_y;
                //Serial.printf("y=%d repeatTimes_y=%d\n", y, repeatTimes_y);
                last_y = upscaleCoef_y;
                while (repeat_y < repeatTimes_y)
                {
                  int lineId = 0;
                  x_upscaled = margin_x;
                  x = margin_x;
                  last_x = margin_x;
                  uint16_t lastPixel = 0;
                  bool lastLine = false;
                  bool lastCol = false;
                  char bottomChr;
                  // End Upscale
                  for (ushort c = 0; c < 0x50; c++)
                  {
                    char chr;
                    if (c % 2 == 0)
                    {
                      chr = auxram[(ushort)((0x2000 + (b * 0x28) + (l * 0x80) + c / 2) + block * 0x400)];
                    }
                    else
                    {
                      chr = ram[(ushort)((0x2000 + (b * 0x28) + (l * 0x80) + (c - 1) / 2) + block * 0x400)];
                    }

                    bool blockline[8];
                    for (int i = 0; i < 8; i++)
                      blockline[7 - i] = (chr & (1 << i)) != 0;

                    bool fillLine = videoColor;
                    fillLine = true; // TFT renders both color & mono from line[] after the row
                    if (fillLine)
                    {
                      for (int i = 7; i > 0; i--)
                      {
                        *(line + lineId) = blockline[i];
                        lineId++;
                      }
                    }
                    else
                    {
                      for (int i = 7; i > 0; i--)
                      {
                        // Init Upscale
                        int repeat_x = 0;
                        uint16_t upscaleCoef_x = floor(coef560 * (float)(x+1));
                        //Serial.printf("upscaleCoef_x=%d screen_width=%f (float)(x+1)=%f last_x=%d\n",upscaleCoef_x, screen_width, (float)(x+1), last_x);
                        bool downScale = upscaleCoef_x < last_x;
                        uint8_t repeatTimes_x = upscaleCoef_x - last_x;
                        //Serial.printf("x=%d repeatTimes_x=%d\n", x, repeatTimes_x);
                        last_x = upscaleCoef_x;
                        while (repeat_x < repeatTimes_x)
                        {
                          repeat_x++;
                        }
                        x++;
                      }
                    }
                  }
                  if (videoColor) {
                    // 140 DHGR color pixels (4 bits each) x2 = 280 px to match the window
                    for (int i = 0; i < 0x50 * 7; i += 4) {
                      int color = (line[i] ? 8 : 0) + (line[i + 1] ? 4 : 0) + (line[i + 2] ? 2 : 0) + (line[i + 3] ? 1 : 0);
                      tft.writeColor(colors16[color], 2);
                    }
                  } else {
                    // 560 mono DHGR bits downsampled 2:1 = 280 px to match the window
                    for (int i = 0; i < 0x50 * 7; i += 2)
                      tft.writeColor(line[i] ? TFT_WHITE : TFT_BLACK, 1);
                  }
                  repeat_y++;
                  y_upscaled++;
                }
                y++;
              }

            }
            else // hires
            {
              for (int block = 0; block < 8; block++)
              {
                // Track the displayed page live (C054/C055) instead of latching it once
                // per frame: with a slow free-running renderer the latched page can be the
                // one the CPU is redrawing during page-flip animation, which flickers.
                // Reading it per scanline keeps us on the currently-shown (completed) page.
                graphicsPage = Page1_Page2 ? 0x2000 : 0x4000;
                // Init Upscale
                int repeat_y = 0;
                uint16_t repeatTimes_y = 1;
                uint16_t upscaleCoef_y = floor(coef192 * (float)(y+1));
                repeatTimes_y = upscaleCoef_y - last_y;
                last_y = upscaleCoef_y;
                while (repeat_y < repeatTimes_y)
                {
                  x_upscaled = margin_x;
                  x = margin_x;
                  last_x = margin_x;
                  uint16_t lastPixel = 0;
                  bool lastLine = false;
                  bool lastCol = false;
                  // End Upscale
                  for (ushort c = 0; c < 0x28; c++)
                  {
                    if (c == 0x27)
                      lastCol = true;
                    char chr;
                    char prevChr;
                    char pixels[7];
                    char chrBottom;
                    char prevChrBottom;
                    char pixelsBottom[7];
                    if (smoothUpscale) {
                      if (videoColor && repeat_y > 0) {
                        int blockb = block;
                        int lb = l;
                        int bb = b;
                        blockb++;
                        if (blockb == 8) { blockb=0; lb++; }
                        if (lb == 8) { lb=0; bb++; }
                        if (bb == 3) { bb=0; lastLine = true; }

                        chrBottom = ram[(ushort)(((graphicsPage) + (bb * 0x28) + (lb * 0x80) + c) + blockb * 0x400)];
                        if (c % 2 == 0) // Odd
                        {
                          pixelsBottom[0] = (chrBottom & 0x80) >> 5 | (chrBottom & 1) << 1 | (prevChrBottom & 0x40) >> 6;
                          pixelsBottom[1] = (chrBottom & 0x80) >> 5 | (chrBottom & 1) << 1 | (chrBottom & 0x2) >> 1;
                          pixelsBottom[2] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x4) >> 1 | (chrBottom & 0x2) >> 1;
                          pixelsBottom[3] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x4) >> 1 | (chrBottom & 0x8) >> 3;
                          pixelsBottom[4] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x10) >> 3 | (chrBottom & 0x8) >> 3;
                          pixelsBottom[5] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x10) >> 3 | (chrBottom & 0x20) >> 5;
                          pixelsBottom[6] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x40) >> 5 | (chrBottom & 0x20) >> 5;
                        }
                        else // Even
                        {
                          pixelsBottom[0] = (chrBottom & 0x80) >> 5 | (prevChrBottom & 0x40) >> 5 | (chrBottom & 0x1); 
                          pixelsBottom[1] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x2) | (chrBottom & 0x1);
                          pixelsBottom[2] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x2) | (chrBottom & 0x4) >> 2;
                          pixelsBottom[3] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x8) >> 2 | (chrBottom & 0x4) >> 2;
                          pixelsBottom[4] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x8) >> 2 | (chrBottom & 0x10) >> 4;
                          pixelsBottom[5] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x20) >> 4 | (chrBottom & 0x10) >> 4;
                          pixelsBottom[6] = (chrBottom & 0x80) >> 5 | (chrBottom & 0x20) >> 4 | (chrBottom & 0x40) >> 6;
                          
                        }
                      }
                    }
                    
                    chr = ram[(ushort)(((graphicsPage) + (b * 0x28) + (l * 0x80) + c) + block * 0x400)];

                    if (videoColor)
                    {
                      char pixels[7];
                      if (c % 2 == 0) // Odd
                      {
                        pixels[0] = (chr & 0x80) >> 5 | (chr & 1) << 1 | (prevChr & 0x40) >> 6;
                        pixels[1] = (chr & 0x80) >> 5 | (chr & 1) << 1 | (chr & 0x2) >> 1;
                        pixels[2] = (chr & 0x80) >> 5 | (chr & 0x4) >> 1 | (chr & 0x2) >> 1;
                        pixels[3] = (chr & 0x80) >> 5 | (chr & 0x4) >> 1 | (chr & 0x8) >> 3;
                        pixels[4] = (chr & 0x80) >> 5 | (chr & 0x10) >> 3 | (chr & 0x8) >> 3;
                        pixels[5] = (chr & 0x80) >> 5 | (chr & 0x10) >> 3 | (chr & 0x20) >> 5;
                        pixels[6] = (chr & 0x80) >> 5 | (chr & 0x40) >> 5 | (chr & 0x20) >> 5;
                      }
                      else // Even
                      {
                        pixels[0] = (chr & 0x80) >> 5 | (prevChr & 0x40) >> 5 | (chr & 0x1); 
                        pixels[1] = (chr & 0x80) >> 5 | (chr & 0x2) | (chr & 0x1);
                        pixels[2] = (chr & 0x80) >> 5 | (chr & 0x2) | (chr & 0x4) >> 2;
                        pixels[3] = (chr & 0x80) >> 5 | (chr & 0x8) >> 2 | (chr & 0x4) >> 2;
                        pixels[4] = (chr & 0x80) >> 5 | (chr & 0x8) >> 2 | (chr & 0x10) >> 4;
                        pixels[5] = (chr & 0x80) >> 5 | (chr & 0x20) >> 4 | (chr & 0x10) >> 4;
                        pixels[6] = (chr & 0x80) >> 5 | (chr & 0x20) >> 4 | (chr & 0x40) >> 6;
                      }

                      for (int id = 0; id < 7; id++)
                      { 
                        tft.writeColor(colors[pixels[id]], 1);
                        x++;
                      }
                      prevChr = chr;
                      if (smoothUpscale) {
                        if (repeat_y > 0) {
                          prevChrBottom = chrBottom;
                        }
                      }
                    }
                    else
                    {
                      bool blockline[8];
                      for (int i = 0; i < 8; i++)
                        blockline[7 - i] = (chr & (1 << i)) != 0;
                      for (int i = 7; i > 0; i--)
                      {
                        uint16_t color = TFT_BLACK;
                        if (blockline[i])
                          color = TFT_WHITE;
                        else
                          color = TFT_BLACK;
                        tft.writeColor(color, 1);
                        x++;
                      }
                    }
                  }
                  repeat_y++;
                  y_upscaled++;
                }
                y++;
              }
            }
          }
          else if (Cols40_80) // Text modes
          {
            for (int i = 0; i < 8; i++) // char lines
            {
              // Init Upscale
              int repeat_y = 0;
              uint16_t repeatTimes_y = 1;
              uint16_t upscaleCoef_y = floor(coef192 * (float)(y+1));
              repeatTimes_y = upscaleCoef_y - last_y;
              last_y = upscaleCoef_y;
              while (repeat_y < repeatTimes_y)
              {
                x_upscaled = margin_x;
                x = margin_x;
                last_x = margin_x;
                uint16_t lastPixel = 0;
                bool lastLine = false;
                bool lastCol = false;
                // End Upscale

                for (int c = 0; c < 0x28; c++)
                {
                  for (int k = 0; k < 7; k++)
                  {
                    // Init Upscale
                    bool bbPixel = 0;
                    if (smoothUpscale) {
                      if (c == 0x27 && k == 6)
                        lastCol = true;
                      if (repeat_y > 0) {
                        int ib = i;
                        int lb = l;
                        int bb = b;
                        ib++;
                        if (ib == 8) { ib=0; lb++; }
                        if (lb == 8) { lb=0; bb++; }
                        if (bb == 3) { bb=0; lastLine = true; }

                        char bottomChr = ram[(ushort)(textPage + (bb * 0x28) + (lb * 0x80) + c)];
                        
                        ushort bottomAddr = (bottomChr * 7 * 8) + (ib * 7) + k;
                        bbPixel = AppleIIe ? AppleIIeFontPixels[bottomAddr] : AppleFontPixels[bottomAddr];
                      }
                    }
                    // End Upscale
                    char chr = ram[(ushort)(textPage + (b * 0x28) + (l * 0x80) + c)];
                    ushort addr = (chr * 7 * 8) + (i * 7) + k;
                    bool bpixel = AppleIIe ? AppleIIeFontPixels[addr] : AppleFontPixels[addr];
                    bool inverted = false;
                    if (!AppleIIe)
                      inverted = chr >= 0x40 && chr < 0x80 && inversed;
                      tft.writeColor(bpixel ? (inverted ? TFT_BLACK : TFT_WHITE) : (inverted ? TFT_WHITE : TFT_BLACK), 1);
                    x++;
                  }
                }
                repeat_y++;
                y_upscaled++;
              }
              y++;
            }
          }
          else if (AppleIIe && !Cols40_80)
          {
            for (int i = 0; i < 8; i++)
            {
              // Init Upscale
              int repeat_y = 0;
              uint16_t upscaleCoef_y = floor(coef192 * (float)(y+1));
              uint16_t repeatTimes_y = upscaleCoef_y - last_y;
              last_y = upscaleCoef_y;
              while (repeat_y < repeatTimes_y)
              {
                x_upscaled = margin_x;
                x = margin_x;
                last_x = margin_x;
                uint16_t lastPixel = 0;
                bool lastLine = false;
                bool lastCol = false;
                char bottomChr;
                // End Upscale
                for (int j = 0; j < 0x50; j++)
                {
                  // Init Upscale
                  bool bbPixel = 0;
                  int ib = i;
                  if (smoothUpscale) {
                    if (j == 0x4f)
                      lastCol = true;
                    int lb = l;
                    int bb = b;
                    if (repeat_y > 0) {
                      ib++;
                      if (ib == 8) { ib=0; lb++; }
                      if (lb == 8) { lb=0; bb++; }
                      if (bb == 3) { bb=0; lastLine = true; }

                      if (j % 2 == 0)
                      {
                        bottomChr = auxram[0, (ushort)(0x400 + (bb * 0x28) + (lb * 0x80) + j / 2)];
                      }
                      else
                      {
                        bottomChr = ram[(ushort)(0x400 + (bb * 0x28) + (lb * 0x80) + (j - 1) / 2)];
                      }
                      
                    }
                  }
                  // End Upscale

                  char chr;
                  if (j % 2 == 0)
                  {
                    chr = auxram[0, (ushort)(0x400 + (b * 0x28) + (l * 0x80) + j / 2)];
                  }
                  else
                  {
                    chr = ram[(ushort)(0x400 + (b * 0x28) + (l * 0x80) + (j - 1) / 2)];
                  }
                  bool last7bits = false;
                  for (int k = 0; k < 7; k++)
                  {
                    ushort addr = (chr * 7 * 8) + (i * 7) + k;
                    bool bpixel = AppleIIeFontPixels[addr];
                    uint16_t color = 0;
                    if (k % 2 == 0)
                    {
                      if (bpixel && last7bits)
                        color = tft.color565(255, 255, 255);
                      else if (bpixel != last7bits)
                        color = tft.color565(127, 127, 127);
                      else
                        color = tft.color565(0, 0, 0);
                      tft.writeColor(color, 1);
                      x++;
                    }
                    last7bits = bpixel;
                  }
                }
                repeat_y++;
                y_upscaled++;
              }
              y++;
            }
          }
        }
      }
    }
    
    unsigned long endTime2 = millis();
    tft.endWrite();
#if defined(BOARD_PICOCALC)
    // Disk II drive light. It sits in the bottom letterbox bar, BELOW the 320x240 emulated
    // screen, which is the one piece of panel the Apple raster does not own -- so it can never
    // land on top of the picture, and the clear-screen path above (which writes exactly
    // 320x240) never erases it. DriveMotorON_OFF is set by the $C0E8/$C0E9 soft switches in
    // src/apple2/disk.cpp, so it lights when the real drive's lamp would.
    //
    // Redrawn only when it changes. The frame rate here is ~18fps and this is a 12x12 square
    // that is usually not changing, so an unconditional write would be an SPI transaction and a
    // pxFlush() per frame for nothing.
    {
      static int8_t ledOn = -1;                       // -1 = never drawn
      int8_t want = DriveMotorON_OFF ? 1 : 0;
      if (want != ledOn) {
        ledOn = want;
        tft.fillPanelRect(PANEL_NATIVE_W - 20, PANEL_NATIVE_H - 20, 12, 12,
                          want ? tft.color565(255, 40, 40) : TFT_BLACK);
      }
    }
#endif
    // Draw the touch keyboard over the bottom rows (only when it changed). The
    // squeezed raster above never touches this region, so one draw per change is
    // enough and there is no flicker.
    if (oskActive()) { displaySetUiMode(true); oskRender(); }
    Vertical_blankingOn_Off = true;
    unsigned long endTime3 = millis();
    vTaskDelay(pdMS_TO_TICKS(5));
    unsigned long endTime4 = millis();
    tft.invertDisplay(true);
    flashCount++;
    if (flashCount > 7)
    {
      inversed = !inversed;
      flashCount = 0;
    }
    unsigned long endTime5 = millis();
    // Calculate and print the duration
    unsigned long duration1 = endTime1 - startTime;
    unsigned long duration2 = endTime2 - endTime1;
    unsigned long duration3 = endTime3 - endTime2;
    unsigned long duration4 = endTime4 - endTime3;
    unsigned long duration5 = endTime5 - endTime4;
    
    unsigned long duration = endTime5 - startTime;

  // Serial.printf("Execution time: %d %d %d %d %d total: %d\n", duration1, duration2, duration3, duration4, duration5, duration);

  }
}
