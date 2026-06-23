#include "../../emu.h"

// optionsui.ino - Modern, touch-driven settings window.
//
// Replaces the old Apple II text-grid options screen with a styled, clickable UI
// drawn with TFT_eSPI primitives. Like the on-screen keyboard, all touch reads and
// drawing happen on core 0 from renderLoop() (video.ino) while OptionsWindow is set
// (the CPU is paused). Touch is read through the shared touchRead() in touchkeyboard.ino.
//
// It drives the same emulator state and helpers the PS/2 menu used (HdDisk, AppleIIe,
// Fast1MhzSpeed, sound, joystick, videoColor, upscale, smoothUpscale, volume, the
// disk/HD file lists, setDiskFile/setHdFile, saveEEPROM, ESP.restart), so PS/2 and
// touch stay interchangeable. PS/2 changes call optionsUiMarkDirty() to refresh it.

// ---- Layout (320 x 240) ----
#define OUI_TITLE_H   26
#define OUI_TG_TOP    28          // toggle grid: 4 columns x 2 rows
#define OUI_TG_W      80
#define OUI_TG_H      34
#define OUI_VOL_TOP   98
#define OUI_VOL_H     20
#define OUI_FB_TOP    120         // file browser header
#define OUI_FB_HDR_H  14
#define OUI_FB_LIST   134         // file rows
#define OUI_FB_ROWH   14
#define OUI_FB_ROWS   5
#define OUI_ACT_TOP   208         // action buttons
#define OUI_ACT_H     30

// ---- Palette (macros: evaluated at runtime so tft is already constructed) ----
#define OUI_BG      tft.color565(18, 20, 26)
#define OUI_TITLE   tft.color565(0, 150, 200)
#define OUI_CARD    tft.color565(44, 48, 60)
#define OUI_CARD2   tft.color565(30, 33, 42)
#define OUI_SEL     tft.color565(0, 120, 215)
#define OUI_ON      tft.color565(40, 175, 95)
#define OUI_OFF     tft.color565(150, 160, 175)
#define OUI_TXT     TFT_WHITE
#define OUI_LBL     tft.color565(150, 160, 175)
#define OUI_MOUNT   tft.color565(40, 150, 80)
#define OUI_REBOOT  tft.color565(210, 120, 30)
#define OUI_RED     tft.color565(205, 70, 60)
#define OUI_BORDER  tft.color565(70, 78, 92)

static bool optionsUiDirty       = false;
static bool optionsUiFirstDraw   = false;
static bool optionsUiPrevDown    = false;
static bool optionsUiWaitRelease = false;
static bool ouiHelpOpen          = false;   // HELP overlay (controls cheat-sheet) is showing
static void ouiOpenHelp();                  // (defined below; forward-declared for the nav handlers)
static void ouiCloseHelp();

// Joystick focus: left/right moves between controls; the focused one gets a white
// border. Order: 0..5 toggle cards, then volume, file list, MOUNT, SAVE & REBOOT.
// Toggle grid slots 6 and 7 (bottom-right) are intentionally left empty for two
// future buttons; navigation skips them.
#define OUI_TG_COUNT      6
#if BOARD_DISPLAY_GFX
// The JC4827W543 adds a SCREEN (fill / original) toggle in grid slot 6, so the later focus
// targets shift up by one. On the CYD the panel is already 320x240 (nothing to fill) -> no slot.
#define OUI_FOC_SCREEN    6   // grid slot 6: fill-screen video toggle
#define OUI_FOC_VOL       7
#define OUI_FOC_FILES     8
#define OUI_FOC_MOUNT     9   // Apple: MOUNT          / C64: LOAD & RUN
#define OUI_FOC_MNTREBOOT 10  // Apple: MOUNT + REBOOT / C64: (unused)
#define OUI_FOC_REBOOT    11  // both:  REBOOT
#define OUI_FOC_COUNT     12
#else
#define OUI_FOC_VOL       6
#define OUI_FOC_FILES     7
#define OUI_FOC_MOUNT     8   // Apple: MOUNT          / C64: LOAD & RUN
#define OUI_FOC_MNTREBOOT 9   // Apple: MOUNT + REBOOT / C64: (unused)
#define OUI_FOC_REBOOT    10  // both:  REBOOT
#define OUI_FOC_COUNT     11
#endif
static int optionsUiFocus = 0;

// The settings window is shared by every platform. These accessors pick the active
// file list / selection so the file browser, scrolling and actions are platform-aware
// without duplicating the whole UI.
static bool ouiIsC64() { return currentPlatform == PLATFORM_C64; }
static bool ouiIsNES() { return currentPlatform == PLATFORM_NES; }
static bool ouiIsAtari() { return currentPlatform == PLATFORM_ATARI; }
static bool ouiIsIIgs() { return currentPlatform == PLATFORM_IIGS; }   // shares the Apple grid, minus MACHINE

static std::vector<std::string> &ouiFiles()
{
  if (ouiIsC64()) return c64Files;
  if (ouiIsNES()) return nesFiles;
  if (ouiIsAtari()) return atariFiles;
  return HdDisk ? hdFiles : diskFiles;
}

static std::string ouiSel()
{
  if (ouiIsC64()) return std::string(selectedC64FileName.c_str());
  if (ouiIsNES()) return std::string(selectedNesFileName.c_str());
  if (ouiIsAtari()) return std::string(selectedAtariFileName.c_str());
  return std::string((HdDisk ? selectedHdFileName : selectedDiskFileName).c_str());
}

// A browser entry is a directory if it's the ".." up-entry or ends with "/".
static bool ouiIsDir(const std::string &e) { return e == ".." || (!e.empty() && e.back() == '/'); }

// Display label for a browser entry: ".." for up, "[dir]" for a subdirectory, else the
// file's basename (path stripped).
static std::string ouiDisplayName(const std::string &e)
{
  if (e == "..") return "..";
  bool dir = !e.empty() && e.back() == '/';
  std::string p = e;
  if (dir) p.pop_back();
  size_t sl = p.find_last_of('/');
  std::string base = (sl == std::string::npos) ? p : p.substr(sl + 1);
  return dir ? ("[" + base + "]") : base;
}

// Navigate the C64 browser into a directory entry (or up via "..") and refresh the list.
static void ouiBrowse(const std::string &entry)
{
  if (entry == "..") c64BrowseUp();
  else               c64BrowseEnter(entry.c_str());
  shownFile = 0xff; firstShowFile = 0;
  optionsUiSyncSelection();
  optionsUiDirty = true;
}

// ---------------------------------------------------------------------------
// Selection / scrolling helpers
// ---------------------------------------------------------------------------
void optionsUiSyncSelection()
{
  std::vector<std::string> &files = ouiFiles();
  std::string sel = ouiSel();
  int idx = -1;
  for (int i = 0; i < (int)files.size(); i++)
    if (files[i] == sel) { idx = i; break; }
  if (idx < 0) idx = 0;
  shownFile = (uint8_t)idx;
  if (files.empty()) { firstShowFile = 0; return; }
  if (shownFile < firstShowFile) firstShowFile = shownFile;
  else if (shownFile >= firstShowFile + OUI_FB_ROWS) firstShowFile = shownFile - OUI_FB_ROWS + 1;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void ouiSmallBtn(int x, int y, int w, int h, const char *s, uint16_t face)
{
  tft.fillRoundRect(x, y, w, h, 4, face);
  tft.drawRoundRect(x, y, w, h, 4, OUI_BORDER);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_TXT, face);
  tft.drawString(s, x + w / 2, y + h / 2, 2);
}

// 2px white outline marking the control the joystick is focused on.
static void ouiFocusRing(int x, int y, int w, int h, int r)
{
  tft.drawRoundRect(x,     y,     w,     h,     r, TFT_WHITE);
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, r, TFT_WHITE);
}

static void ouiDrawToggle(int idx, const char *label, const char *value, uint16_t valColor)
{
  int col = idx % 4, row = idx / 4;
  int x = col * OUI_TG_W, y = OUI_TG_TOP + row * OUI_TG_H;
  tft.fillRoundRect(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5, OUI_CARD);
  tft.drawRoundRect(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5, OUI_BORDER);
  // Keep the ring inside the card's filled area (x+2..) so a repaint erases it when
  // focus moves; drawing it 1px outside would leave white pixels in the inter-card gap.
  if (optionsUiFocus == idx) ouiFocusRing(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(OUI_LBL, OUI_CARD);
  tft.drawString(label, x + 8, y + 5, 1);
  tft.setTextDatum(BL_DATUM);
  tft.setTextColor(valColor, OUI_CARD);
  tft.drawString(value, x + 8, y + OUI_TG_H - 5, 2);
}

// Blank a toggle slot (so stale Apple labels don't linger on the C64 screen).
static void ouiClearToggle(int idx)
{
  int col = idx % 4, row = idx / 4;
  int x = col * OUI_TG_W, y = OUI_TG_TOP + row * OUI_TG_H;
  tft.fillRect(x, y, OUI_TG_W, OUI_TG_H, OUI_BG);
}

// JC4827W543 only: a SCREEN toggle in grid slot 6 (drawn after the platform toggles clear it).
// FILL = video scaled to fill the panel (keep 4:3); ORIG = centered 320x240 with a border.
static void ouiDrawScreenToggle()
{
#if BOARD_DISPLAY_GFX
  ouiDrawToggle(6, "SCREEN", screenFill ? "FILL" : "ORIG", OUI_TXT);
#endif
}

static void ouiDrawToggles()
{
  if (ouiIsC64()) {
    ouiDrawToggle(0, "SOUND",    sound ? "ON" : "MUTE",          OUI_TXT);
    ouiDrawToggle(1, "JOYSTICK", joystick ? "ON" : "OFF",        OUI_TXT);
    ouiDrawToggle(2, "VIDEO",    videoColor ? "COLOR" : "MONO",  OUI_TXT);
    ouiDrawToggle(3, "AUTOLOAD", c64Autoload ? "ON" : "OFF",     OUI_TXT);
    ouiDrawToggle(4, "JOY PORT", joyPort == 1 ? "1" : "2",       OUI_TXT);
    for (int i = 5; i < 8; i++) ouiClearToggle(i);
    ouiDrawScreenToggle();
    return;
  }
  if (ouiIsNES() || ouiIsAtari()) {          // NES / Atari grid: SOUND / JOYSTICK / VIDEO
    ouiDrawToggle(0, "SOUND",    sound ? "ON" : "MUTE",          OUI_TXT);
    ouiDrawToggle(1, "JOYSTICK", joystick ? "ON" : "OFF",        OUI_TXT);
    ouiDrawToggle(2, "VIDEO",    videoColor ? "COLOR" : "MONO",  OUI_TXT);
#if BOARD_DISPLAY_GFX
    if (ouiIsNES()) {                         // NES (S3 only): display frame-skip (game speed vs smoothness)
      const char *sv = (nesDisplaySkip <= 1) ? "OFF" : (nesDisplaySkip == 2) ? "2" : "3";
      ouiDrawToggle(3, "SKIP", sv, OUI_TXT);
      for (int i = 4; i < 8; i++) ouiClearToggle(i);
    } else
#endif
    { for (int i = 3; i < 8; i++) ouiClearToggle(i); }
    ouiDrawScreenToggle();
    return;
  }
  if (ouiIsIIgs()) {                          // IIGS: DEVICE + SPEED + SOUND/JOYSTICK/VIDEO (no II+/IIe)
    ouiDrawToggle(0, "DEVICE",   HdDisk ? "HD" : "DISK",          OUI_TXT);
    ouiDrawToggle(1, "SPEED",    Fast1MhzSpeed ? "FAST" : "1MHz", OUI_TXT);
    ouiDrawToggle(2, "SOUND",    sound ? "ON" : "MUTE",           OUI_TXT);
    ouiDrawToggle(3, "JOYSTICK", joystick ? "ON" : "OFF",         OUI_TXT);
    ouiDrawToggle(4, "VIDEO",    videoColor ? "COLOR" : "MONO",   OUI_TXT);
    for (int i = 5; i < 8; i++) ouiClearToggle(i);
    ouiDrawScreenToggle();
    return;
  }
  ouiDrawToggle(0, "DEVICE",   HdDisk ? "HD" : "DISK",          OUI_TXT);
  ouiDrawToggle(1, "MACHINE",  AppleIIe ? "IIe" : "II+",        OUI_TXT);
  ouiDrawToggle(2, "SPEED",    Fast1MhzSpeed ? "FAST" : "1MHz", OUI_TXT);
  ouiDrawToggle(3, "SOUND",    sound ? "ON" : "MUTE",           OUI_TXT);
  ouiDrawToggle(4, "JOYSTICK", joystick ? "ON" : "OFF",         OUI_TXT);
  ouiDrawToggle(5, "VIDEO",    videoColor ? "COLOR" : "MONO",   OUI_TXT);
  ouiDrawScreenToggle();   // slot 6 (S3); slot 7 still free for a future button
}

static void ouiDrawVolume()
{
  tft.fillRect(0, OUI_VOL_TOP, 320, OUI_VOL_H, OUI_BG);
  int level = volume / 0x10;
  char vlabel[24];
  sprintf(vlabel, "VOLUME  %d/15", level);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(OUI_LBL, OUI_BG);
  tft.drawString(vlabel, 7, OUI_VOL_TOP + OUI_VOL_H / 2, 1);

  int by = OUI_VOL_TOP + 1, bh = OUI_VOL_H - 2;
  ouiSmallBtn(120, by, 28, bh, "-", OUI_CARD);
  ouiSmallBtn(288, by, 28, bh, "+", OUI_CARD);

  int tx = 152, tw = 130, th = 8, ty = by + (bh - th) / 2;
  tft.fillRoundRect(tx, ty, tw, th, 3, OUI_CARD2);
  int fw = (int)((long)tw * level / 15);
  if (fw > 0) tft.fillRoundRect(tx, ty, fw, th, 3, OUI_SEL);

  if (optionsUiFocus == OUI_FOC_VOL) ouiFocusRing(116, OUI_VOL_TOP, 202, OUI_VOL_H, 4);
}

static void ouiDrawFiles()
{
  std::vector<std::string> &files = ouiFiles();
  std::string sel = ouiSel();

  // header
  tft.fillRect(0, OUI_FB_TOP, 320, OUI_FB_HDR_H, OUI_BG);
  char hdr[40];
  sprintf(hdr, "%s  (%d)", ouiIsC64() ? "PRG/D64/CRT" : ouiIsNES() ? "NES ROMS"
                         : ouiIsAtari() ? "A26/BIN ROMS"
                         : (HdDisk ? "HD IMAGES" : "DISK IMAGES"),
          (int)files.size());
  tft.setTextDatum(BL_DATUM);
  tft.setTextColor(OUI_LBL, OUI_BG);
  tft.drawString(hdr, 7, OUI_FB_TOP + OUI_FB_HDR_H - 2, 1);

  int listH = OUI_FB_ROWS * OUI_FB_ROWH;
  if (files.empty()) {
    tft.fillRect(0, OUI_FB_LIST, 300, listH, OUI_CARD2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(OUI_LBL, OUI_CARD2);
    tft.drawString("No images on SD card", 150, OUI_FB_LIST + listH / 2, 1);
  } else {
    for (int r = 0; r < OUI_FB_ROWS; r++) {
      int idx = firstShowFile + r;
      int ry = OUI_FB_LIST + r * OUI_FB_ROWH;
      if (idx >= (int)files.size()) { tft.fillRect(0, ry, 300, OUI_FB_ROWH, OUI_CARD2); continue; }

      bool selected = (idx == shownFile);
      bool mounted  = (files[idx] == sel);
      uint16_t rowbg = selected ? OUI_SEL : OUI_CARD2;
      tft.fillRect(0, ry, 300, OUI_FB_ROWH, rowbg);
      if (mounted) tft.fillRect(0, ry, 3, OUI_FB_ROWH, OUI_ON);

      std::string nm = ouiIsC64() ? ouiDisplayName(files[idx]) : files[idx];
      if (!ouiIsC64() && !nm.empty() && nm[0] == '/') nm = nm.substr(1);
      if (nm.size() > 46) nm = nm.substr(0, 43) + "...";
      tft.setTextDatum(ML_DATUM);
      uint16_t txtcol = ouiIsC64() && ouiIsDir(files[idx]) ? tft.color565(120, 200, 255)
                      : (selected ? OUI_TXT : tft.color565(200, 205, 215));
      tft.setTextColor(txtcol, rowbg);
      tft.drawString(nm.c_str(), 9, ry + OUI_FB_ROWH / 2, 1);
    }
  }

  // scroll buttons (right column)
  int sX = 302, sW = 18, half = listH / 2;
  ouiSmallBtn(sX, OUI_FB_LIST, sW, half - 1, "^", OUI_CARD);
  ouiSmallBtn(sX, OUI_FB_LIST + half + 1, sW, half - 1, "v", OUI_CARD);

  if (optionsUiFocus == OUI_FOC_FILES) ouiFocusRing(0, OUI_FB_LIST, 300, listH, 2);
}

// Called from the (slow) directory scan in the render task: shows a "Loading… N" bar in the
// file-list area and yields (vTaskDelay) so the scan doesn't block the task / trip the watchdog.
void uiDirScanProgress(int count)
{
  int y = OUI_FB_LIST, listH = OUI_FB_ROWS * OUI_FB_ROWH;
  tft.fillRect(0, y, 320, listH, OUI_CARD2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_TXT, OUI_CARD2);
  char s[24];
  sprintf(s, "Loading...  %d", count);
  tft.drawString(s, 160, y + listH / 2 - 9, 2);
  int bx = 24, bw = 272, bh = 8, by = y + listH / 2 + 6;
  tft.drawRoundRect(bx, by, bw, bh, 3, OUI_BORDER);
  int fw = count >= 250 ? bw - 2 : (bw - 2) * count / 250;     // bar fills toward the 250 cap
  if (fw > 0) tft.fillRoundRect(bx + 1, by + 1, fw, bh - 2, 2, OUI_SEL);
  vTaskDelay(1);   // yield: feed the watchdog + let the idle task run during a long scan
}

// Draw one labelled action button (rounded, optional focus ring).
static void ouiActBtn(int x, int w, const char *label, uint16_t face, uint16_t txt, int focusId)
{
  int y = OUI_ACT_TOP, h = OUI_ACT_H;
  tft.fillRoundRect(x, y, w, h, 6, face);
  tft.drawRoundRect(x, y, w, h, 6, OUI_BORDER);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(txt, face);
  tft.drawString(label, x + w / 2, y + h / 2, 2);
  if (optionsUiFocus == focusId) ouiFocusRing(x, y, w, h, 6);
}

static void ouiDrawActions()
{
  bool canMount = !ouiFiles().empty();
  uint16_t mc = canMount ? OUI_MOUNT : OUI_CARD2;
  uint16_t mt = canMount ? OUI_TXT : OUI_LBL;

  if (ouiIsC64() || ouiIsNES() || ouiIsAtari()) {   // C64/NES/Atari: LOAD & RUN + REBOOT
    ouiActBtn(6,   120, "LOAD & RUN", mc,         mt,      OUI_FOC_MOUNT);
    ouiActBtn(132, 182, "REBOOT",     OUI_REBOOT, OUI_TXT, OUI_FOC_REBOOT);
    return;
  }
  // Apple II: MOUNT / M+REBOOT / REBOOT
  ouiActBtn(4,   102, "MOUNT",    mc,         mt,      OUI_FOC_MOUNT);
  ouiActBtn(109, 102, "M+REBOOT", mc,         mt,      OUI_FOC_MNTREBOOT);
  ouiActBtn(214, 102, "REBOOT",   OUI_REBOOT, OUI_TXT, OUI_FOC_REBOOT);
}

static void ouiDrawTitle()
{
  tft.fillRect(0, 0, 320, OUI_TITLE_H, OUI_TITLE);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(OUI_TXT, OUI_TITLE);
  tft.drawString(ouiIsC64() ? "COMMODORE 64  SETTINGS"
               : ouiIsNES() ? "NINTENDO  NES  SETTINGS"
               : ouiIsAtari() ? "ATARI 2600  SETTINGS" : "APPLE II  SETTINGS",
                 10, OUI_TITLE_H / 2, 2);
  int cw = OUI_TITLE_H, cx = 320 - cw;
  tft.fillRect(cx, 0, cw, OUI_TITLE_H, OUI_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_TXT, OUI_RED);
  tft.drawString("X", cx + cw / 2, OUI_TITLE_H / 2, 2);
}

// HELP button: occupies the free toggle-grid slot 7 (bottom-right). Tapping it opens the
// controls cheat-sheet overlay (ouiDrawHelp). Drawn for every platform.
static void ouiDrawHelpButton()
{
  int idx = 7, col = idx % 4, row = idx / 4;
  int x = col * OUI_TG_W, y = OUI_TG_TOP + row * OUI_TG_H;
  uint16_t face = tft.color565(58, 92, 130);
  tft.fillRoundRect(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5, face);
  tft.drawRoundRect(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5, OUI_BORDER);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_TXT, face);
  tft.drawString("HELP", x + OUI_TG_W / 2, y + OUI_TG_H / 2, 2);
}

// --- HELP overlay (per-platform controls cheat-sheet) ---
static void ouiHelpHdr(int &y, const char *s)
{
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(tft.color565(90, 200, 255), OUI_BG);
  tft.drawString(s, 8, y, 2);
  y += 19;
}
static void ouiHelpRow(int &y, const char *k, const char *v)
{
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(OUI_ON, OUI_BG);
  tft.drawString(k, 14, y, 1);
  tft.setTextColor(tft.color565(205, 210, 220), OUI_BG);
  tft.drawString(v, 120, y, 1);
  y += 12;
}

static void ouiDrawHelp()
{
  tft.fillScreen(OUI_BG);
  // title bar with an X (any tap closes, but the X is the obvious affordance)
  tft.fillRect(0, 0, 320, OUI_TITLE_H, OUI_TITLE);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(OUI_TXT, OUI_TITLE);
  tft.drawString("HELP  /  CONTROLS", 10, OUI_TITLE_H / 2, 2);
  int cw = OUI_TITLE_H, cx = 320 - cw;
  tft.fillRect(cx, 0, cw, OUI_TITLE_H, OUI_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_TXT, OUI_RED);
  tft.drawString("X", cx + cw / 2, OUI_TITLE_H / 2, 2);

  int y = OUI_TITLE_H + 6;
  ouiHelpHdr(y, "GLOBAL");
  ouiHelpRow(y, "F10",           "Open / close menu");
  ouiHelpRow(y, "Vol +/-",       "Volume (media keys)");
  ouiHelpRow(y, "Pad SEL+START", "Open / close menu");

  if (ouiIsNES()) {
    ouiHelpHdr(y, "NES  -  KEYBOARD");
    ouiHelpRow(y, "Arrows",      "D-pad");
    ouiHelpRow(y, "X / Z",       "A / B");
    ouiHelpRow(y, "Enter / Tab", "Start / Select");
    ouiHelpHdr(y, "NES  -  GAMEPAD");
    ouiHelpRow(y, "D-pad",       "D-pad");
    ouiHelpRow(y, "A / B",       "A / B");
    ouiHelpRow(y, "Start / Sel", "Start / Select");
  } else if (ouiIsAtari()) {
    ouiHelpHdr(y, "ATARI  -  KEYBOARD");
    ouiHelpRow(y, "Arrows",    "Joystick");
    ouiHelpRow(y, "Space / X", "Fire");
    ouiHelpRow(y, "Enter",     "Reset switch");
    ouiHelpRow(y, "Tab",       "Select switch");
    ouiHelpHdr(y, "ATARI  -  GAMEPAD");
    ouiHelpRow(y, "D-pad", "Joystick");
    ouiHelpRow(y, "A / B", "Fire / Select");
    ouiHelpRow(y, "Start", "Reset");
  } else if (ouiIsC64()) {
    ouiHelpHdr(y, "C64  -  KEYBOARD");
    ouiHelpRow(y, "Keys",   "C64 layout");
    ouiHelpRow(y, "Arrows", "Cursor");
    ouiHelpHdr(y, "C64  -  GAMEPAD");
    ouiHelpRow(y, "D-pad", "Stick");
    ouiHelpRow(y, "A",     "Fire");
  } else {   // Apple II / IIGS
    ouiHelpHdr(y, "APPLE  -  KEYBOARD");
    ouiHelpRow(y, "Keys",   "Type into Apple");
    ouiHelpRow(y, "Arrows", "Cursor");
    ouiHelpRow(y, "F11",    "Reset");
    ouiHelpHdr(y, "APPLE  -  GAMEPAD");
    ouiHelpRow(y, "D-pad", "Paddle / stick");
    ouiHelpRow(y, "A / B", "Button 0 / 1");
  }

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(OUI_LBL, OUI_BG);
  tft.drawString("Tap anywhere to close", 160, 236, 1);
}

void optionsUiRender()
{
  if (!optionsUiDirty) return;
  if (optionsUiFirstDraw) { tft.fillScreen(OUI_BG); optionsUiFirstDraw = false; }
  if (ouiHelpOpen) { ouiDrawHelp(); optionsUiDirty = false; return; }
  ouiDrawTitle();
  ouiDrawToggles();
  ouiDrawHelpButton();
  ouiDrawVolume();
  ouiDrawFiles();
  ouiDrawActions();
  optionsUiDirty = false;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
#if BOARD_DISPLAY_GFX
static void ouiToggleScreenFill() { screenFill = !screenFill; optionsUiDirty = true; }
#endif

static void ouiToggle(int idx)
{
  if (ouiIsC64()) {                       // C64 grid: SOUND/JOYSTICK/VIDEO/AUTOLOAD/JOY PORT
    switch (idx) {
      case 0: sound = !sound;             break;
      case 1: joystick = !joystick;       break;
      case 2: videoColor = !videoColor;   break;
      case 3: c64Autoload = !c64Autoload; break;
      case 4: joyPort = (joyPort == 2) ? 1 : 2; break;
      default: return;
    }
    optionsUiDirty = true;
    return;
  }
  if (ouiIsNES() || ouiIsAtari()) {        // NES / Atari grid: SOUND / JOYSTICK / VIDEO (+ SKIP on NES)
    switch (idx) {
      case 0: sound = !sound;           break;
      case 1: joystick = !joystick;     break;
      case 2: videoColor = !videoColor; break;
#if BOARD_DISPLAY_GFX
      case 3: if (!ouiIsNES()) return;        // NES (S3): cycle display frame-skip 1(off)->2->3
              nesDisplaySkip = (nesDisplaySkip >= 3) ? 1 : nesDisplaySkip + 1;
              break;
#endif
      default: return;
    }
    optionsUiDirty = true;
    return;
  }
  if (ouiIsIIgs()) {                    // IIGS grid: 0=DEVICE 1=SPEED 2=SOUND 3=JOYSTICK 4=VIDEO
    switch (idx) {
      case 0:
        HdDisk = !HdDisk;
        if (HdDisk) { if (hdFiles.empty())   loadHdFilesSync();   }
        else        { if (diskFiles.empty()) loadDiskFilesSync(); }
        shownFile = 0xff; firstShowFile = 0; optionsUiSyncSelection();
        break;
      case 1: Fast1MhzSpeed = !Fast1MhzSpeed; break;   // SPEED: FAST (~2.8MHz) vs 1MHz regulator
      case 2: sound = !sound; break;
      case 3: joystick = !joystick; break;
      case 4: videoColor = !videoColor; break;
      default: return;
    }
    optionsUiDirty = true;
    return;
  }
  switch (idx) {
    case 0:
      HdDisk = !HdDisk;
      // Only the boot device's image list is loaded at startup; scan the other on
      // demand (synchronously) so HD/DISK mode always shows its files.
      if (HdDisk) { if (hdFiles.empty())   loadHdFilesSync();   }
      else        { if (diskFiles.empty()) loadDiskFilesSync(); }
      shownFile = 0xff; firstShowFile = 0; optionsUiSyncSelection();
      break;
    case 1: AppleIIe = !AppleIIe; activeFlags = AppleIIe ? flagsIIe : flagsIIplus; break;
    case 2: Fast1MhzSpeed = !Fast1MhzSpeed; break;
    case 3: sound = !sound; break;
    case 4: joystick = !joystick; break;
    case 5: videoColor = !videoColor; break;
    default: return;
  }
  optionsUiDirty = true;
}

static void ouiScroll(int dir)
{
  std::vector<std::string> &files = ouiFiles();
  int maxStart = (int)files.size() - OUI_FB_ROWS;
  if (maxStart < 0) maxStart = 0;
  int fs = (int)firstShowFile + dir;
  if (fs < 0) fs = 0;
  if (fs > maxStart) fs = maxStart;
  firstShowFile = (uint8_t)fs;
  optionsUiDirty = true;
}

static void ouiMount()
{
  std::vector<std::string> &files = ouiFiles();
  if (files.empty()) return;
  if (ouiIsC64()) {                       // C64: load the highlighted image (.prg/.d64/.crt) + run
    if (shownFile >= files.size()) return;
    if (ouiIsDir(files[shownFile])) { ouiBrowse(files[shownFile]); return; }  // dir -> navigate
    selectedC64FileName = files[shownFile].c_str();
    c64LoadSelected(selectedC64FileName.c_str());
    showHideOptionsWindow();              // close -> CPU resumes -> BASIC autoruns
    return;
  }
  if (ouiIsNES()) {                       // NES: load the highlighted .nes + reset into it
    if (shownFile >= files.size()) return;
    if (nesLoadSelected(files[shownFile].c_str()))
      showHideOptionsWindow();            // close only on success (failure keeps the old ROM)
    return;
  }
  if (ouiIsAtari()) {                      // Atari: load the highlighted .a26/.bin + reset into it
    if (shownFile >= files.size()) return;
    if (atariLoadSelected(files[shownFile].c_str()))
      showHideOptionsWindow();            // close only on success (failure keeps the old ROM)
    return;
  }
  if (ouiIsIIgs()) {                       // IIGS: persist the highlighted image + reboot -> auto-mounted on boot
    if (shownFile >= files.size()) return;
    if (ouiIsDir(files[shownFile])) { ouiBrowse(files[shownFile]); return; }
    if (HdDisk) setHdFile(); else setDiskFile();   // selectedHd/DiskFileName = highlighted image
    saveConfig();         // persist so the boot auto-load (emu6502.ino) mounts it
    ESP.restart();        // reboot -> iigsSetup loads it -> firmware boots (slot 7 HD / slot 6 disk)
    return;
  }
  if (HdDisk) setHdFile(); else setDiskFile();
  diskChanged = true;
  showHideOptionsWindow();   // mount selected image and close
}

// Apple "MOUNT + REBOOT": apply the highlighted image as the boot device, save, then restart.
static void ouiMountReboot()
{
  if (ouiIsC64()) return;                 // C64 has no such button
  if (!ouiFiles().empty()) { if (HdDisk) setHdFile(); else setDiskFile(); }
  saveConfig();
  ESP.restart();
}

// "REBOOT" button (both platforms): persist settings, then restart -> the boot splash, where
// you can switch platforms. (Explicit reboots ask for the splash; platform-select / mount+reboot
// do not, so they boot straight into the chosen system.)
static void ouiReboot()
{
  saveConfig();
  requestSplashOnNextBoot();
  ESP.restart();
}

// ---- Joystick navigation (called from joystick.ino, core 0) ----
// Left/right move the focus; up/down act on the focused control; fire activates it.
void optionsUiNav(int dir)            // dir: -1 = left, +1 = right
{
  if (ouiHelpOpen) { ouiCloseHelp(); return; }   // any input dismisses the help overlay
  optionsUiFocus = (optionsUiFocus + dir + OUI_FOC_COUNT) % OUI_FOC_COUNT;
  optionsUiDirty = true;
}

void optionsUiAdjust(int dir)         // dir: -1 = up, +1 = down
{
  if (ouiHelpOpen) { ouiCloseHelp(); return; }
  int f = optionsUiFocus;
  if (f >= 0 && f < OUI_TG_COUNT) { ouiToggle(f); return; }
#if BOARD_DISPLAY_GFX
  if (f == OUI_FOC_SCREEN) { ouiToggleScreenFill(); return; }
#endif
  if (f == OUI_FOC_VOL) {
    if (dir < 0) { if (volume < 0xf0) volume += 0x10; }
    else         { if (volume > 0) { volume -= 0x10; if (volume > 0xf0) volume = 0; } }
    optionsUiDirty = true;
    return;
  }
  if (f == OUI_FOC_FILES) {
    std::vector<std::string> &files = ouiFiles();
    if (files.empty()) return;
    int idx = (int)shownFile + (dir < 0 ? -1 : 1);
    if (idx < 0) idx = 0;
    if (idx > (int)files.size() - 1) idx = (int)files.size() - 1;
    shownFile = (uint8_t)idx;
    if (shownFile < firstShowFile) firstShowFile = shownFile;
    else if (shownFile >= firstShowFile + OUI_FB_ROWS) firstShowFile = shownFile - OUI_FB_ROWS + 1;
    optionsUiDirty = true;
  }
  // MOUNT / REBOOT have nothing to adjust
}

void optionsUiActivate()              // joystick fire button on the focused control
{
  if (ouiHelpOpen) { ouiCloseHelp(); return; }
  int f = optionsUiFocus;
  if (f >= 0 && f < OUI_TG_COUNT) ouiToggle(f);
#if BOARD_DISPLAY_GFX
  else if (f == OUI_FOC_SCREEN)    ouiToggleScreenFill();
#endif
  else if (f == OUI_FOC_FILES)     ouiMount();
  else if (f == OUI_FOC_MOUNT)     ouiMount();
  else if (f == OUI_FOC_MNTREBOOT) ouiMountReboot();
  else if (f == OUI_FOC_REBOOT)    ouiReboot();
  // FOC_VOL: nothing (adjust with up/down)
}

// HELP overlay open/close. Opening/closing forces a full repaint (the pages don't overlap).
static void ouiOpenHelp()  { ouiHelpOpen = true;  optionsUiFirstDraw = true; optionsUiDirty = true; }
static void ouiCloseHelp() { ouiHelpOpen = false; optionsUiFirstDraw = true; optionsUiDirty = true; }

static void ouiHandleTap(int16_t x, int16_t y)
{
  // HELP overlay is modal: any tap returns to the settings page.
  if (ouiHelpOpen) { ouiCloseHelp(); return; }

  // close button
  if (y < OUI_TITLE_H && x >= 320 - OUI_TITLE_H) { showHideOptionsWindow(); return; }

  // toggle grid
  if (y >= OUI_TG_TOP && y < OUI_TG_TOP + 2 * OUI_TG_H) {
    int col = x / OUI_TG_W, row = (y - OUI_TG_TOP) / OUI_TG_H;
    int idx = row * 4 + col;
    if (idx < OUI_TG_COUNT) ouiToggle(idx);
#if BOARD_DISPLAY_GFX
    else if (idx == 6) ouiToggleScreenFill();   // grid slot 6 = SCREEN (fill / original)
#endif
    else if (idx == 7) ouiOpenHelp();           // grid slot 7 = HELP (controls cheat-sheet)
    return;
  }

  // volume +/-
  if (y >= OUI_VOL_TOP && y < OUI_VOL_TOP + OUI_VOL_H) {
    if (x >= 120 && x < 148) { if (volume > 0) { volume -= 0x10; if (volume > 0xf0) volume = 0; } optionsUiDirty = true; return; }
    if (x >= 288 && x < 316) { if (volume < 0xf0) volume += 0x10; optionsUiDirty = true; return; }
  }

  // file list / scroll
  int listH = OUI_FB_ROWS * OUI_FB_ROWH;
  if (y >= OUI_FB_LIST && y < OUI_FB_LIST + listH) {
    if (x >= 302) { ouiScroll(y < OUI_FB_LIST + listH / 2 ? -1 : 1); return; }
    if (x < 300) {
      std::vector<std::string> &files = ouiFiles();
      int idx = firstShowFile + (y - OUI_FB_LIST) / OUI_FB_ROWH;
      if (idx < (int)files.size()) {
        if (ouiIsC64() && ouiIsDir(files[idx])) ouiBrowse(files[idx]);   // enter dir / go up
        else { shownFile = (uint8_t)idx; optionsUiDirty = true; }
      }
      return;
    }
  }

  // action buttons
  if (y >= OUI_ACT_TOP && y < OUI_ACT_TOP + OUI_ACT_H) {
    if (ouiIsC64() || ouiIsNES() || ouiIsAtari()) {   // LOAD & RUN (6..126) | REBOOT (132..314)
      if (x >= 6 && x < 126)        ouiMount();
      else if (x >= 132 && x < 314) ouiReboot();
    } else {                                // MOUNT (4..106) | M+REBOOT (109..211) | REBOOT (214..316)
      if (x >= 4 && x < 106)        ouiMount();
      else if (x >= 109 && x < 211) ouiMountReboot();
      else if (x >= 214 && x < 316) ouiReboot();
    }
  }
}

// ---------------------------------------------------------------------------
// Per-frame service (renderLoop, core 0) + entry points for the rest of the app
// ---------------------------------------------------------------------------
void optionsUiPoll()
{
  int16_t x = 0, y = 0;
  bool down = touchRead(&x, &y);
  if (optionsUiWaitRelease) {            // ignore the touch that opened the window
    if (!down) optionsUiWaitRelease = false;
    optionsUiPrevDown = down;
    return;
  }
  if (down && !optionsUiPrevDown) ouiHandleTap(x, y);
  optionsUiPrevDown = down;
}

void optionsUiOpen()
{
  if (ouiIsC64() && c64Files.empty()) loadC64FilesSync();   // populate the .prg browser
  if (ouiIsNES() && nesFiles.empty()) nesScanFiles();       // populate the .nes browser
  if (ouiIsAtari() && atariFiles.empty()) atariScanFiles(); // populate the .a26/.bin browser
  optionsUiSyncSelection();
  optionsUiFocus       = 0;
  ouiHelpOpen          = false;  // always open on the settings page, not the help overlay
  optionsUiFirstDraw   = true;
  optionsUiDirty       = true;
  optionsUiWaitRelease = true;   // don't treat the opening tap as a click
  optionsUiPrevDown    = true;
}

// Called by the PS/2 menu handlers (optionsScreenRender/listFiles) so keyboard
// changes refresh the touch UI and keep the selected file visible.
void optionsUiMarkDirty()
{
  if (shownFile != 0xff) {
    if (shownFile < firstShowFile) firstShowFile = shownFile;
    else if (shownFile >= firstShowFile + OUI_FB_ROWS) firstShowFile = shownFile - OUI_FB_ROWS + 1;
  }
  optionsUiDirty = true;
}
