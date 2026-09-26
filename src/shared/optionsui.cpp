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

#if defined(BOARD_PICOCALC)
// ---- Layout (320 x 320) ----
// The PicoCalc panel is square: the emulators letterbox into its middle 320x240, but the menu
// switches the display to full-panel mode (tft.setFullPanel) and spends the extra 80 rows on a
// taller grid and, mostly, on more and taller file rows drawn in the larger list font (font 3).
// Order differs from the touch boards: the file list comes first (it is what the menu is opened
// for), then its MOUNT / REBOOT buttons, then the option grid and VOLUME. The keyboard row map
// (ouiRows, further down) follows the same order so Up/Down walk the page top to bottom.
#define OUI_SCR_H     320
#define OUI_TITLE_H   28
#define OUI_FB_TOP    30          // file browser header
#define OUI_FB_HDR_H  16
#define OUI_FB_LIST   46          // file rows
#define OUI_FB_ROWH   17
#define OUI_FB_ROWS   8
#define OUI_FB_FONT   3           // display_picocalc.cpp: FreeSans at 4/5, between fonts 1 and 2
#define OUI_ACT_TOP   186         // action buttons (or the key hints while the list is focused)
#define OUI_ACT_H     30
#define OUI_TG_TOP    220         // toggle grid: 4 columns x 2 rows
#define OUI_TG_W      80
#define OUI_TG_H      38
#define OUI_VOL_TOP   297
#define OUI_VOL_H     22
#define OUI_HELP_ROWH 15
#else
// ---- Layout (320 x 240) ----
#define OUI_SCR_H     240
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
#define OUI_FB_FONT   1
#define OUI_ACT_TOP   208         // action buttons
#define OUI_ACT_H     30
#define OUI_HELP_ROWH 12
#endif

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
static bool ouiEditing           = false;   // keyboard: inside VOL / the file list, arrows change the value
static void ouiOpenHelp();                  // (defined below; forward-declared for the nav handlers)
static void ouiCloseHelp();

// Joystick focus: left/right moves between controls; the focused one gets a white
// border. Order: 0..5 toggle cards, then volume, file list, MOUNT, SAVE & REBOOT.
// Toggle grid slots 6 and 7 (bottom-right) are intentionally left empty for two
// future buttons; navigation skips them.
#define OUI_TG_COUNT      6
#if BOARD_HAS_SCREENFILL
// The S3 panel + the desktop window add a SCREEN (fill / original) toggle in grid slot 6, so the
// later focus targets shift up by one. On the CYD the panel is already 320x240 -> no slot.
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
// HELP sits at OUI_FOC_COUNT, i.e. deliberately OUTSIDE the ring optionsUiNav() walks
// (it wraps modulo OUI_FOC_COUNT). The analog stick and the touch handler therefore behave
// exactly as before -- on those boards HELP is a tap away. The keyboard row map below does
// include it, because the PicoCalc has no touchscreen and this was the one control on the
// page that no key could reach.
#define OUI_FOC_HELP      OUI_FOC_COUNT
#if defined(BOARD_PICOCALC)
static int optionsUiFocus = OUI_FOC_FILES;   // the top of this board's page (see the layout)
#else
static int optionsUiFocus = 0;
#endif

// The settings window is shared by every platform. These accessors pick the active
// file list / selection so the file browser, scrolling and actions are platform-aware
// without duplicating the whole UI.
static bool ouiIsC64() { return currentPlatform == PLATFORM_C64; }
static bool ouiIsNES() { return currentPlatform == PLATFORM_NES; }
static bool ouiIsAtari() { return currentPlatform == PLATFORM_ATARI; }
static bool ouiIsMsx() { return currentPlatform == PLATFORM_MSX; }     // ROM/disk browser like NES/Atari
static bool ouiIsSms() { return currentPlatform == PLATFORM_SMS; }     // ROM browser like NES/Atari/MSX
static bool ouiIsColeco() { return currentPlatform == PLATFORM_COLECO; } // cartridge browser like SMS
static bool ouiIsZx() { return currentPlatform == PLATFORM_ZX; }       // snapshot/tape browser
static bool ouiIsPcxt() { return currentPlatform == PLATFORM_PCXT; }   // disk-image browser
// The PC-XT has the A:/C: two-slot disk UI. ouiPcA/ouiPcC are the A: floppy / C: hard-disk image markers.
static String &ouiPcA() { return selectedPcFileName; }
static String &ouiPcC() { return selectedPcHdFileName; }

static std::vector<std::string> &ouiFiles()
{
  if (ouiIsC64()) return c64Files;
  if (ouiIsNES()) return nesFiles;
  if (ouiIsAtari()) return atariFiles;
  if (ouiIsMsx()) return msxFiles;
  if (ouiIsSms()) return smsFiles;
  if (ouiIsColeco()) return colecoFiles;
  if (ouiIsZx()) return zxFiles;
  if (ouiIsPcxt()) return pcFiles;
  return HdDisk ? hdFiles : diskFiles;
}

static std::string ouiSel()
{
  if (ouiIsC64()) return std::string(selectedC64FileName.c_str());
  if (ouiIsNES()) return std::string(selectedNesFileName.c_str());
  if (ouiIsAtari()) return std::string(selectedAtariFileName.c_str());
  if (ouiIsMsx()) return std::string(selectedMsxFileName.c_str());
  if (ouiIsSms()) return std::string(selectedSmsFileName.c_str());
  if (ouiIsColeco()) return std::string(selectedColecoFileName.c_str());
  if (ouiIsZx()) return std::string(selectedZxFileName.c_str());
  if (ouiIsPcxt()) return std::string(selectedPcFileName.c_str());
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

// Navigate the active browser into a directory entry (or up via "..") and refresh the list.
// Every core keeps its own browse directory, so this mirrors the ouiFiles() dispatch above.
// The Apple II has no browser of its own: it walks the disk or HD list, whichever is the device.
static void ouiBrowse(const std::string &entry)
{
  const bool up = (entry == "..");
  const char *p = entry.c_str();
  if      (ouiIsC64())     { if (up) c64BrowseUp();     else c64BrowseEnter(p); }
  else if (ouiIsNES())     { if (up) nesBrowseUp();     else nesBrowseEnter(p); }
  else if (ouiIsAtari())   { if (up) atariBrowseUp();   else atariBrowseEnter(p); }
  else if (ouiIsMsx())     { if (up) msxBrowseUp();     else msxBrowseEnter(p); }
  else if (ouiIsSms())     { if (up) smsBrowseUp();     else smsBrowseEnter(p); }
  else if (ouiIsColeco())  { if (up) colecoBrowseUp();  else colecoBrowseEnter(p); }
  else if (ouiIsZx())      { if (up) zxBrowseUp();      else zxBrowseEnter(p); }
  else if (ouiIsPcxt())    { if (up) pcxtBrowseUp();    else pcxtBrowseEnter(p); }
  else if (HdDisk)         { if (up) hdBrowseUp();      else hdBrowseEnter(p); }
  else                     { if (up) diskBrowseUp();    else diskBrowseEnter(p); }
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

// 2px outline marking the control the joystick/keyboard is focused on. Green instead of white
// while that control is being edited (keyboard only), so it is obvious that the arrows are
// changing a value rather than moving the focus -- and therefore that Escape backs out.
static void ouiFocusRing(int x, int y, int w, int h, int r)
{
  uint16_t c = ouiEditing ? OUI_ON : TFT_WHITE;
  tft.drawRoundRect(x,     y,     w,     h,     r, c);
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, r, c);
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
#if BOARD_HAS_SCREENFILL
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
  if (ouiIsMsx()) {                           // MSX grid: SOUND / JOYSTICK / VIDEO / SPEED + Z80 readout
    char mhz[12]; snprintf(mhz, sizeof(mhz), "%.1fMHz", msxMeasuredMhz);
    ouiDrawToggle(0, "SOUND",    sound ? "ON" : "MUTE",          OUI_TXT);
    ouiDrawToggle(1, "JOYSTICK", joystick ? "ON" : "OFF",        OUI_TXT);
    ouiDrawToggle(2, "VIDEO",    videoColor ? "COLOR" : "MONO",  OUI_TXT);
    ouiDrawToggle(3, "SPEED",    msxFast ? "FAST" : "NORMAL",    OUI_TXT);
    ouiDrawToggle(4, "Z80",      mhz,                            OUI_TXT);   // measured uncapped speed (read-only)
    for (int i = 5; i < 8; i++) ouiClearToggle(i);
    ouiDrawScreenToggle();
    return;
  }
  if (ouiIsSms() || ouiIsColeco() || ouiIsZx()) {   // SMS / Coleco / ZX grid: SOUND / JOYSTICK / VIDEO / SPEED + Z80 readout
    char mhz[12]; snprintf(mhz, sizeof(mhz), "%.1fMHz", ouiIsSms() ? smsMeasuredMhz : ouiIsZx() ? zxMeasuredMhz : colecoMeasuredMhz);
    const bool fast = ouiIsSms() ? smsFast : ouiIsZx() ? zxFast : colecoFast;
    ouiDrawToggle(0, "SOUND",    sound ? "ON" : "MUTE",          OUI_TXT);
    ouiDrawToggle(1, "JOYSTICK", joystick ? "ON" : "OFF",        OUI_TXT);
    ouiDrawToggle(2, "VIDEO",    videoColor ? "COLOR" : "MONO",  OUI_TXT);
    ouiDrawToggle(3, "SPEED",    fast ? "FAST" : "NORMAL",       OUI_TXT);
    ouiDrawToggle(4, "Z80",      mhz,                            OUI_TXT);   // measured uncapped speed (read-only)
    for (int i = 5; i < 8; i++) ouiClearToggle(i);
    ouiDrawScreenToggle();
    return;
  }
  if (ouiIsPcxt()) {                          // PCXT grid: SOUND / VIDEO + 8086 speed readout
    char mhz[12]; snprintf(mhz, sizeof(mhz), "%.1fMHz", pcMeasuredMhz);
    ouiDrawToggle(0, "SOUND",    sound ? "ON" : "MUTE",          OUI_TXT);
    ouiDrawToggle(1, "VIDEO",    videoColor ? "COLOR" : "MONO",  OUI_TXT);
    ouiDrawToggle(2, "8086",     mhz,                            OUI_TXT);   // measured equiv speed (read-only)
    for (int i = 3; i < 8; i++) ouiClearToggle(i);
    ouiDrawScreenToggle();
    return;
  }
  if (ouiIsNES() || ouiIsAtari()) {           // NES / Atari grid: SOUND / JOYSTICK / VIDEO
    ouiDrawToggle(0, "SOUND",    sound ? "ON" : "MUTE",          OUI_TXT);
    ouiDrawToggle(1, "JOYSTICK", joystick ? "ON" : "OFF",        OUI_TXT);
    ouiDrawToggle(2, "VIDEO",    videoColor ? "COLOR" : "MONO",  OUI_TXT);
    if (ouiIsNES()) {                         // NES: speed control + measured 2A03 readout
      ouiDrawToggle(3, "SPEED", nesFast ? "FAST" : "NORMAL", OUI_TXT);
      char mhz[12]; snprintf(mhz, sizeof(mhz), "%.2fMHz", nesMeasuredMhz);
      ouiDrawToggle(4, "2A03",  mhz,                        OUI_TXT);   // measured speed (read-only)
#if BOARD_DISPLAY_GFX
      const char *sv = (nesDisplaySkip <= 1) ? "OFF" : (nesDisplaySkip == 2) ? "2" : "3";
      ouiDrawToggle(5, "SKIP", sv, OUI_TXT);  // S3 only: display frame-skip (smoothness vs core-1 time)
#else
      ouiClearToggle(5);
#endif
      ouiClearToggle(6);
    } else {
      for (int i = 3; i < 8; i++) ouiClearToggle(i);   // Atari: no speed control
    }
    ouiDrawScreenToggle();   // slot 6 (S3); drawn after the clears
    return;
  }
  // No MACHINE card: II+ and IIe are two separate systems on the boot splash now, and the memory
  // map is built for the chosen one at startup (src/apple2/memory.cpp), so the model cannot be
  // flipped from here any more. Which one is running is in the title bar instead. The grid is
  // therefore DEVICE SPEED SOUND JOYSTICK VIDEO, slot 5 empty, 6 SCREEN, 7 HELP.
  ouiDrawToggle(0, "DEVICE",   HdDisk ? "HD" : "DISK",          OUI_TXT);
  ouiDrawToggle(1, "SPEED",    Fast1MhzSpeed ? "FAST" : "1MHz", OUI_TXT);
  ouiDrawToggle(2, "SOUND",    sound ? "ON" : "MUTE",           OUI_TXT);
  ouiDrawToggle(3, "JOYSTICK", joystick ? "ON" : "OFF",         OUI_TXT);
  ouiDrawToggle(4, "VIDEO",    videoColor ? "COLOR" : "MONO",   OUI_TXT);
  ouiClearToggle(5);
  ouiDrawScreenToggle();   // slot 6: SCREEN (FILL/ORIG); slot 7 = HELP. 6502 MHz shows in the title bar.
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
                         : ouiIsMsx() ? "MSX ROM/DSK"
                         : ouiIsSms() ? "SMS ROMS"
                         : ouiIsColeco() ? "CARTRIDGES"
                         : ouiIsZx() ? "SNA/Z80/TAP"
                         : ouiIsPcxt() ? "PC DISK IMG"
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
      bool mntA = ouiIsPcxt() && ouiPcA().length() && (files[idx] == std::string(ouiPcA().c_str()));
      bool mntC = ouiIsPcxt() && ouiPcC().length() && (files[idx] == std::string(ouiPcC().c_str()));
      bool mounted  = (files[idx] == sel) || mntA || mntC;
      uint16_t rowbg = selected ? OUI_SEL : OUI_CARD2;
      tft.fillRect(0, ry, 300, OUI_FB_ROWH, rowbg);
      if (mounted) tft.fillRect(0, ry, 3, OUI_FB_ROWH, OUI_ON);

      // Every browser can now be inside a subdirectory, so every row gets the same treatment:
      // ".." as-is, "[name]" for a directory, otherwise the basename with the path stripped.
      std::string nm = ouiDisplayName(files[idx]);
#if defined(BOARD_PICOCALC)
      // Proportional list font: truncate by measured width, not character count.
      const int maxw = (mntA || mntC) ? 262 : 286;   // leave room for the A:/C: chip on mounted rows
      if (tft.textWidth(nm.c_str(), OUI_FB_FONT) > maxw) {
        while (nm.size() > 1 && tft.textWidth((nm + "...").c_str(), OUI_FB_FONT) > maxw) nm.pop_back();
        nm += "...";
      }
#else
      size_t maxlen = (mntA || mntC) ? 40 : 46;   // leave room for the A:/C: chip on mounted rows
      if (nm.size() > maxlen) nm = nm.substr(0, maxlen - 3) + "...";
#endif
      tft.setTextDatum(ML_DATUM);
      uint16_t txtcol = ouiIsDir(files[idx]) ? tft.color565(120, 200, 255)
                      : (selected ? OUI_TXT : tft.color565(200, 205, 215));
      tft.setTextColor(txtcol, rowbg);
      tft.drawString(nm.c_str(), 9, ry + OUI_FB_ROWH / 2, OUI_FB_FONT);
      if (mntA || mntC) {                         // chip showing which drive this image is mounted in
        const char *tag = (mntA && mntC) ? "AC" : mntA ? "A" : "C";
        tft.fillRoundRect(278, ry + 2, 20, OUI_FB_ROWH - 4, 3, OUI_ON);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(OUI_BG, OUI_ON);
        tft.drawString(tag, 288, ry + OUI_FB_ROWH / 2, 1);
      }
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

#if defined(BOARD_PICOCALC)
// Keyboard-only board: while the file list has the focus, the action-button row is replaced by
// the keys that act on the highlighted row (the buttons are unreachable from there anyway -- the
// arrows are browsing the list). Drawn as runs of {key, what it does} pairs, keys in green.
static void ouiHintLine(int y, const char *const *seg, int n)
{
  int w = 0;
  for (int i = 0; i < n; i++) w += tft.textWidth(seg[i], OUI_FB_FONT);
  int x = 160 - w / 2;
  tft.setTextDatum(ML_DATUM);
  for (int i = 0; i < n; i++) {
    tft.setTextColor((i & 1) ? OUI_TXT : OUI_ON, OUI_CARD2);
    x += tft.drawString(seg[i], x, y, OUI_FB_FONT);
  }
}

static void ouiDrawFileHints()
{
  std::vector<std::string> &fl = ouiFiles();
  const bool dir = shownFile < fl.size() && ouiIsDir(fl[shownFile]);
  // What Enter (inside the list) and Ctrl-Enter do -- mirrors optionsUiKeyEnter / ouiMount.
  const char *enterAct = dir               ? "open folder"
                       : ouiIsPcxt()       ? "mount A:"
                       : (ouiIsC64() || ouiIsNES() || ouiIsAtari() || ouiIsMsx() || ouiIsSms() || ouiIsColeco() || ouiIsZx())
                                           ? "load & run" : "mount";
  const char *ctrlAct  = dir               ? NULL
                       : ouiIsPcxt()       ? "mount C:"
                       : (currentPlatform == PLATFORM_APPLE2) ? "mount + reboot" : NULL;

  const int y = OUI_ACT_TOP, h = OUI_ACT_H;
  tft.fillRect(0, y, 320, h, OUI_BG);
  tft.fillRoundRect(4, y, 312, h, 6, OUI_CARD2);
  tft.drawRoundRect(4, y, 312, h, 6, OUI_BORDER);
  const int y1 = y + h / 4 + 1, y2 = y + (3 * h) / 4 - 1;
  if (ouiEditing) {
    const char *l1[] = { "Enter ", enterAct, "    Ctrl+Enter ", ctrlAct };
    ouiHintLine(y1, l1, ctrlAct ? 4 : 2);
    const char *l2[] = { "Arrows ", "select", "    Esc ", "back" };
    ouiHintLine(y2, l2, 4);
  } else {
    const char *l1[] = { "Enter ", "browse list", "    Ctrl+Enter ", ctrlAct };
    ouiHintLine(y1, l1, ctrlAct ? 4 : 2);
    const char *l2[] = { "Arrows ", "move to other controls" };
    ouiHintLine(y2, l2, 2);
  }
}
#endif

static void ouiDrawActions()
{
#if defined(BOARD_PICOCALC)
  if (optionsUiFocus == OUI_FOC_FILES) { ouiDrawFileHints(); return; }
  tft.fillRect(0, OUI_ACT_TOP, 320, OUI_ACT_H, OUI_BG);   // wipe the hint panel from the gaps
#endif
  bool canMount = !ouiFiles().empty();
  uint16_t mc = canMount ? OUI_MOUNT : OUI_CARD2;
  uint16_t mt = canMount ? OUI_TXT : OUI_LBL;

  if (ouiIsPcxt()) {                       // PC-XT: MOUNT/EJECT A: | MOUNT/EJECT C: | REBOOT
    std::vector<std::string> &fl = ouiFiles();
    bool curA = shownFile < fl.size() && ouiPcA().length() && (fl[shownFile] == std::string(ouiPcA().c_str()));
    bool curC = shownFile < fl.size() && ouiPcC().length() && (fl[shownFile] == std::string(ouiPcC().c_str()));
    ouiActBtn(4,   102, curA ? "EJECT A:" : "MOUNT A:", curA ? OUI_RED : mc, curA ? OUI_TXT : mt, OUI_FOC_MOUNT);
    ouiActBtn(109, 102, curC ? "EJECT C:" : "MOUNT C:", curC ? OUI_RED : mc, curC ? OUI_TXT : mt, OUI_FOC_MNTREBOOT);
    ouiActBtn(214, 102, "REBOOT",   OUI_REBOOT, OUI_TXT, OUI_FOC_REBOOT);
    return;
  }
  if (ouiIsC64() || ouiIsNES() || ouiIsAtari() || ouiIsMsx() || ouiIsSms() || ouiIsColeco() || ouiIsZx()) {   // LOAD & RUN + REBOOT
    ouiActBtn(6,   120, "LOAD & RUN", mc, mt, OUI_FOC_MOUNT);
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
               : ouiIsAtari() ? "ATARI 2600  SETTINGS"
               : ouiIsMsx() ? "MSX1  SETTINGS"
               : ouiIsSms() ? "MASTER SYSTEM  SETTINGS"
               : ouiIsColeco() ? "COLECOVISION  SETTINGS"
               : ouiIsZx() ? "ZX SPECTRUM 48K  SETTINGS"
               : ouiIsPcxt() ? "PC-XT (8086)  SETTINGS"
               : AppleIIe ? "APPLE IIe  SETTINGS" : "APPLE II+  SETTINGS",
                 10, OUI_TITLE_H / 2, 2);
  int cw = OUI_TITLE_H, cx = 320 - cw;
  if (currentPlatform == PLATFORM_APPLE2) {     // 6502 speed readout (its grid slot is now SCREEN)
    char mhz[20]; snprintf(mhz, sizeof(mhz), "6502  %.2f MHz", appleMeasuredMhz);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(OUI_TXT, OUI_TITLE);
    tft.drawString(mhz, cx - 8, OUI_TITLE_H / 2, 1);
  }
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
  if (optionsUiFocus == OUI_FOC_HELP)
    ouiFocusRing(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5);
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
  y += OUI_HELP_ROWH;
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
#if defined(BOARD_PICOCALC)
  ouiHelpRow(y, "Ctrl-F1",       "Open / close menu");
  ouiHelpRow(y, "Ctrl-F6",       "System menu (Ct-Sh-F3)");
  ouiHelpRow(y, "Ctrl-Shift-F1", "Reboot the device");
#else
  ouiHelpRow(y, "F10",           "Open / close menu");
  ouiHelpRow(y, "Vol +/-",       "Volume (media keys)");
  ouiHelpRow(y, "Pad SEL+START", "Open / close menu");
#endif
  ouiHelpHdr(y, "IN THIS MENU");
  ouiHelpRow(y, "Arrows",        "Browse");
  ouiHelpRow(y, "Enter",         "Toggle / open / mount");
  ouiHelpRow(y, "Ctrl-Enter",    "Mount + reboot");
  ouiHelpRow(y, "Esc",           "Back, then close");

  if (ouiIsNES()) {
    ouiHelpHdr(y, "NES  -  KEYBOARD");
    ouiHelpRow(y, "Arrows",      "D-pad");
    ouiHelpRow(y, "X / Z",       "A / B");
    ouiHelpRow(y, "Enter / Tab", "Start / Select");
#if !defined(BOARD_PICOCALC)   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "NES  -  GAMEPAD");
    ouiHelpRow(y, "D-pad",       "D-pad");
    ouiHelpRow(y, "A / B",       "A / B");
    ouiHelpRow(y, "Start / Sel", "Start / Select");
#endif
  } else if (ouiIsAtari()) {
    ouiHelpHdr(y, "ATARI  -  KEYBOARD");
    ouiHelpRow(y, "Arrows",    "Joystick");
#if defined(BOARD_PICOCALC)
    ouiHelpRow(y, "F5 / Space", "Fire");
    ouiHelpRow(y, "F3 / Enter", "Reset switch");
    ouiHelpRow(y, "F4 / Tab",   "Select switch");
#else
    ouiHelpRow(y, "Space / X", "Fire");
    ouiHelpRow(y, "Enter",     "Reset switch");
    ouiHelpRow(y, "Tab",       "Select switch");
#endif
#if !defined(BOARD_PICOCALC)   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "ATARI  -  GAMEPAD");
    ouiHelpRow(y, "D-pad", "Joystick");
    ouiHelpRow(y, "A / B", "Fire / Select");
    ouiHelpRow(y, "Start", "Reset");
#endif
  } else if (ouiIsC64()) {
    ouiHelpHdr(y, "C64  -  KEYBOARD");
    ouiHelpRow(y, "Keys",      "C64 layout");
    ouiHelpRow(y, "Arrows",    "Cursor (JOY off)");
    ouiHelpRow(y, "Arr+Space", "Joystick (JOY on)");
#if !defined(BOARD_PICOCALC)   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "C64  -  GAMEPAD");
    ouiHelpRow(y, "D-pad", "Stick");
    ouiHelpRow(y, "A",     "Fire");
#endif
  } else if (ouiIsMsx()) {
    ouiHelpHdr(y, "MSX  -  KEYBOARD");
    ouiHelpRow(y, "Keys",      "MSX layout");
    ouiHelpRow(y, "Arrows",    "Cursor / Joystick");
    ouiHelpRow(y, "Space",     "Trigger / Space");
#if !defined(BOARD_PICOCALC)   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "MSX  -  GAMEPAD");
    ouiHelpRow(y, "D-pad", "Stick");
    ouiHelpRow(y, "A / B", "Trigger A / B");
#endif
  } else if (ouiIsSms()) {
#if !defined(BOARD_PICOCALC)   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "MASTER SYSTEM  -  GAMEPAD");
    ouiHelpRow(y, "D-pad",  "D-pad");
    ouiHelpRow(y, "A / B",  "Button 1 / 2");
#endif
    ouiHelpHdr(y, "SMS  -  KEYBOARD");
    ouiHelpRow(y, "Arrows", "D-pad");
    ouiHelpRow(y, "Z / X",  "Button 1 / 2");
  } else if (ouiIsColeco()) {
#if !defined(BOARD_PICOCALC)   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "COLECO  -  GAMEPAD");
    ouiHelpRow(y, "D-pad",  "Stick");
    ouiHelpRow(y, "A / B",  "Left / right fire");
    ouiHelpRow(y, "Btn 3/4","Keypad * / 1");
#endif
    ouiHelpHdr(y, "COLECO  -  KEYBOARD");
    ouiHelpRow(y, "Arrows", "Stick");
#if defined(BOARD_PICOCALC)
    ouiHelpRow(y, "Space/F4", "Left fire");
    ouiHelpRow(y, "X / F5",   "Right fire");
#else
    ouiHelpRow(y, "Space/Z", "Left fire");
    ouiHelpRow(y, "X",       "Right fire");
#endif
    ouiHelpRow(y, "0-9",    "Keypad");
    ouiHelpRow(y, "- / =",  "Keypad * / #");
    ouiHelpRow(y, "F12",    "Reset");
  } else if (ouiIsZx()) {
#if !defined(BOARD_PICOCALC)   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "ZX SPECTRUM  -  GAMEPAD");
    ouiHelpRow(y, "D-pad",  "Kempston stick");
    ouiHelpRow(y, "A / B",  "Kempston fire");
#endif
    ouiHelpHdr(y, "ZX SPECTRUM  -  KEYBOARD");
    ouiHelpRow(y, "Shift",     "CAPS SHIFT");
    ouiHelpRow(y, "Ctrl/Alt",  "SYMBOL SHIFT");
    ouiHelpRow(y, "Backspace", "DELETE");
    ouiHelpRow(y, "Esc",       "BREAK");
    ouiHelpRow(y, "Arrows",    "Cursor (JOYSTICK on: Kempston)");
    ouiHelpRow(y, "F12",       "Reset");
  } else if (ouiIsPcxt()) {
    ouiHelpHdr(y, "PC-XT  -  KEYBOARD");
    ouiHelpRow(y, "Keys",   "Type into DOS");
    ouiHelpRow(y, "F12",    "Reboot PC");
#if !defined(BOARD_PICOCALC)   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "PC-XT  -  GAMEPAD");
    ouiHelpRow(y, "D-pad",  "Arrow keys");
    ouiHelpRow(y, "A / B",  "Enter / Esc");
#endif
  } else {   // Apple II
    ouiHelpHdr(y, "APPLE  -  KEYBOARD");
    ouiHelpRow(y, "Keys",   "Type into Apple");
    ouiHelpRow(y, "Arrows", "Cursor / paddles");
#if defined(BOARD_PICOCALC)
    ouiHelpRow(y, "F4 / F5", "Button 0 / 1");
    ouiHelpRow(y, "Ctrl-F3", "Reset the Apple");
#else
    ouiHelpRow(y, "Alt / AltGr", "Button 0 / 1");
    ouiHelpRow(y, "F11",    "Reset");
#endif
#if !defined(BOARD_PICOCALC)
    // Omitted on the PicoCalc: it has no gamepad port (see the "no USB gamepad" line in
    // input_picocalc.cpp), so this section documented hardware that cannot be attached -- and
    // the page used to have only 240 rows, which this section and the row added above overran.
    ouiHelpHdr(y, "APPLE  -  GAMEPAD");
    ouiHelpRow(y, "D-pad", "Paddle / stick");
    ouiHelpRow(y, "A / B", "Button 0 / 1");
#endif
  }

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(OUI_LBL, OUI_BG);
#if defined(BOARD_PICOCALC)
  tft.drawString("Press any key to close", 160, OUI_SCR_H - 4, 1);
#else
  tft.drawString("Tap anywhere to close", 160, 236, 1);
#endif
}

void optionsUiRender()
{
#if defined(BOARD_PICOCALC)
  // Normally already on from optionsUiOpen(); if anything switched it off, repaint all of it.
  if (tft.setFullPanel(true)) { optionsUiFirstDraw = true; optionsUiDirty = true; }
#endif
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
#if BOARD_HAS_SCREENFILL
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
  if (ouiIsMsx()) {                       // MSX grid: SOUND / JOYSTICK / VIDEO / SPEED (Z80 readout = read-only)
    switch (idx) {
      case 0: sound = !sound;           break;
      case 1: joystick = !joystick;     break;
      case 2: videoColor = !videoColor; break;
      case 3: msxFast = !msxFast;       break;   // NORMAL (3.58 MHz) <-> FAST (uncapped)
      default: return;                            // slot 4 (Z80 MHz) is read-only
    }
    optionsUiDirty = true;
    return;
  }
  if (ouiIsSms()) {                       // SMS grid: SOUND / JOYSTICK / VIDEO / SPEED (Z80 readout = read-only)
    switch (idx) {
      case 0: sound = !sound;           break;
      case 1: joystick = !joystick;     break;
      case 2: videoColor = !videoColor; break;
      case 3: smsFast = !smsFast;       break;   // NORMAL (3.58 MHz) <-> FAST (uncapped)
      default: return;                            // slot 4 (Z80 MHz) is read-only
    }
    optionsUiDirty = true;
    return;
  }
  if (ouiIsColeco()) {                    // Coleco grid: same as the SMS one
    switch (idx) {
      case 0: sound = !sound;           break;
      case 1: joystick = !joystick;     break;
      case 2: videoColor = !videoColor; break;
      case 3: colecoFast = !colecoFast; break;
      default: return;
    }
    optionsUiDirty = true;
    return;
  }
  if (ouiIsZx()) {                        // ZX grid: same as the SMS one
    switch (idx) {
      case 0: sound = !sound;           break;
      case 1: joystick = !joystick;     break;
      case 2: videoColor = !videoColor; break;
      case 3: zxFast = !zxFast;         break;   // NORMAL (3.5 MHz) <-> FAST (uncapped)
      default: return;
    }
    optionsUiDirty = true;
    return;
  }
  if (ouiIsPcxt()) {                      // PCXT grid: SOUND / VIDEO (8086 MHz readout = read-only)
    switch (idx) {
      case 0: sound = !sound;           break;
      case 1: videoColor = !videoColor; break;
      default: return;                            // slot 2 (8086 MHz) is read-only
    }
    optionsUiDirty = true;
    return;
  }
  if (ouiIsNES() || ouiIsAtari()) {       // NES / Atari grid: SOUND / JOYSTICK / VIDEO (+ NES SPEED/SKIP)
    switch (idx) {
      case 0: sound = !sound;           break;
      case 1: joystick = !joystick;     break;
      case 2: videoColor = !videoColor; break;
      case 3: if (!ouiIsNES()) return;        // NES: SPEED NORMAL <-> FAST (slot 4 = 2A03 MHz, read-only)
              nesFast = !nesFast; break;
#if BOARD_DISPLAY_GFX
      case 5: if (!ouiIsNES()) return;        // NES (S3): cycle display frame-skip 1(off)->2->3
              nesDisplaySkip = (nesDisplaySkip >= 3) ? 1 : nesDisplaySkip + 1;
              break;
#endif
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
      // free heap on this line too: the scan below is the thing that runs out of it, and if it
      // dies before printing its own figure this is the last number we get.
      sprintf(buf, "DEVICE toggle -> %s (hdFiles=%d diskFiles=%d free heap=%u)",
              HdDisk ? "HD" : "DISK", (int)hdFiles.size(), (int)diskFiles.size(),
              (unsigned)ESP.getFreeHeap());
      printLog(buf);
      if (HdDisk) { if (hdFiles.empty())   loadHdFilesSync();   }
      else        { if (diskFiles.empty()) loadDiskFilesSync(); }
      printLog("DEVICE toggle done");
      shownFile = 0xff; firstShowFile = 0; optionsUiSyncSelection();
      break;
    case 1: Fast1MhzSpeed = !Fast1MhzSpeed; break;
    case 2: sound = !sound; break;
    case 3: joystick = !joystick; break;
    case 4: videoColor = !videoColor; break;
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
  if (files.empty() || shownFile >= files.size()) return;
  // Every browser can be sitting inside a subdirectory now, so a highlighted ".." or "name/"
  // row means navigate, never mount. Checked once here instead of in each per-core branch.
  if (ouiIsDir(files[shownFile])) { ouiBrowse(files[shownFile]); return; }
  if (ouiIsC64()) {                       // C64: load the highlighted image (.prg/.d64/.crt) + run
    if (shownFile >= files.size()) return;
    selectedC64FileName = files[shownFile].c_str();
    c64LoadAndRun(selectedC64FileName.c_str());
    showHideOptionsWindow();              // close -> CPU resumes -> reset -> loads at READY
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
  if (ouiIsMsx()) {                        // MSX: load the highlighted .rom cartridge + reset into it
    if (shownFile >= files.size()) return;
    if (msxLoadSelected(files[shownFile].c_str()))
      showHideOptionsWindow();            // close only on success (failure keeps the old cart)
    return;
  }
  if (ouiIsSms()) {                        // SMS: load the highlighted .sms/.bin ROM + reset into it
    if (shownFile >= files.size()) return;
    if (smsLoadSelected(files[shownFile].c_str()))
      showHideOptionsWindow();            // close only on success (failure keeps the old ROM)
    return;
  }
  if (ouiIsColeco()) {                     // Coleco: load the highlighted cartridge + reset into it
    if (shownFile >= files.size()) return;
    if (colecoLoadSelected(files[shownFile].c_str()))
      showHideOptionsWindow();            // close only on success (failure keeps the old cartridge)
    return;
  }
  if (ouiIsZx()) {                         // ZX: load the highlighted snapshot, or insert the tape + LOAD ""
    if (shownFile >= files.size()) return;
    if (zxLoadSelected(files[shownFile].c_str()))
      showHideOptionsWindow();            // close only on success (failure keeps the running program)
    return;
  }
  if (ouiIsPcxt()) {                       // PCXT: double-tap a file -> mount it as A: (floppy)
    if (shownFile >= files.size()) return;
    if (pcxtMountA(files[shownFile].c_str()))
      showHideOptionsWindow();            // close only on success (failure keeps the old disk)
    return;
  }
  if (HdDisk) setHdFile(); else setDiskFile();
  diskChanged = true;
  showHideOptionsWindow();   // mount selected image and close
}

// PCXT: toggle the highlighted image in A: (floppy) / C: (hard disk). If it's already mounted in that
// slot -> eject (stay in settings so the list/buttons update); otherwise mount it -> close settings.
static void ouiPcMountA()   // A: floppy
{
  std::vector<std::string> &files = ouiFiles();
  if (files.empty() || shownFile >= files.size()) return;
  if (ouiIsDir(files[shownFile])) { ouiBrowse(files[shownFile]); return; }  // dir row -> navigate
  bool isCur = ouiPcA().length() && files[shownFile] == std::string(ouiPcA().c_str());
  if (isCur) { pcxtUnmount(0); optionsUiDirty = true; }       // PC-XT: live mount/eject
  else if (pcxtMountA(files[shownFile].c_str())) showHideOptionsWindow();
}
static void ouiPcMountC()   // C: hard disk
{
  std::vector<std::string> &files = ouiFiles();
  if (files.empty() || shownFile >= files.size()) return;
  if (ouiIsDir(files[shownFile])) { ouiBrowse(files[shownFile]); return; }  // dir row -> navigate
  bool isCur = ouiPcC().length() && files[shownFile] == std::string(ouiPcC().c_str());
  if (isCur) { pcxtUnmount(2); optionsUiDirty = true; }
  else if (pcxtMountC(files[shownFile].c_str())) showHideOptionsWindow();
}

// Apple "MOUNT + REBOOT": apply the highlighted image as the boot device, save, then restart.
static void ouiMountReboot()
{
  if (ouiIsC64()) return;                 // C64 has no such button
  std::vector<std::string> &files = ouiFiles();
  // A directory row navigates instead: rebooting into a folder is not a thing.
  if (!files.empty() && shownFile < files.size() && ouiIsDir(files[shownFile]))
    { ouiBrowse(files[shownFile]); return; }
  if (!files.empty()) { if (HdDisk) setHdFile(); else setDiskFile(); }
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

// Whether focus may land on control f for the running platform. The toggle grid is drawn per
// platform (ouiDrawToggles) and leaves slots empty; the speed readouts (Z80 / 2A03 / 8086 MHz)
// are drawn but do nothing (ouiToggle ignores them); and only the Apple and the PC-XT have a
// middle action button. The focus ring skips all of those instead of vanishing onto them.
// Keep in step with ouiDrawToggles / ouiToggle / ouiDrawActions.
static bool ouiFocusable(int f)
{
  if (f >= 0 && f < OUI_TG_COUNT) {
    if (ouiIsMsx() || ouiIsSms() || ouiIsColeco() || ouiIsZx()) return f <= 3;   // 4 = Z80 readout
    if (ouiIsPcxt())  return f <= 1;                                              // 2 = 8086 readout
    if (ouiIsAtari()) return f <= 2;
    if (ouiIsNES())   return f <= 3 || (BOARD_DISPLAY_GFX && f == 5);             // 4 = 2A03 readout
    return f <= 4;                                                                // C64, Apple II
  }
  if (f == OUI_FOC_MNTREBOOT) return ouiIsPcxt() || currentPlatform == PLATFORM_APPLE2;
  return true;
}

// ---- Joystick navigation (called from joystick.ino, core 0) ----
// Left/right move the focus; up/down act on the focused control; fire activates it.
void optionsUiNav(int dir)            // dir: -1 = left, +1 = right
{
  if (ouiHelpOpen) { ouiCloseHelp(); return; }   // any input dismisses the help overlay
  for (int i = 0; i < OUI_FOC_COUNT; i++) {
    optionsUiFocus = (optionsUiFocus + dir + OUI_FOC_COUNT) % OUI_FOC_COUNT;
    if (ouiFocusable(optionsUiFocus)) break;
  }
  optionsUiDirty = true;
}

void optionsUiAdjust(int dir)         // dir: -1 = up, +1 = down
{
  if (ouiHelpOpen) { ouiCloseHelp(); return; }
  int f = optionsUiFocus;
  if (f >= 0 && f < OUI_TG_COUNT) { ouiToggle(f); return; }
#if BOARD_HAS_SCREENFILL
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
#if BOARD_HAS_SCREENFILL
  else if (f == OUI_FOC_SCREEN)    ouiToggleScreenFill();
#endif
  else if (f == OUI_FOC_FILES)     ouiMount();
  else if (f == OUI_FOC_MOUNT)     { if (ouiIsPcxt()) ouiPcMountA(); else ouiMount(); }       // PC: MOUNT A:
  else if (f == OUI_FOC_MNTREBOOT) { if (ouiIsPcxt()) ouiPcMountC(); else ouiMountReboot(); } // PC: MOUNT C:
  else if (f == OUI_FOC_REBOOT)    ouiReboot();
  else if (f == OUI_FOC_HELP)      ouiOpenHelp();
  // FOC_VOL: nothing (adjust with up/down)
}

// ---- Keyboard navigation -------------------------------------------------------------------
// Deliberately separate from the three joystick entry points above rather than a change to
// them. A joystick has two axes and one button and no Escape, so it uses left/right to move and
// up/down to change the focused value in place -- there is nothing to back out of. A keyboard
// has four arrows and an Escape, so it can afford the more conventional model asked for here:
// all four arrows browse, Enter opens (or toggles) the focused control, Escape leaves a control
// that was opened, and Escape with nothing open closes the window. Keeping the two apart means
// the CYD's analog stick and the touch handler keep behaving exactly as before.
//
// The rows mirror the drawn layout: a 4-wide toggle grid, then VOLUME, then the file list,
// then the action buttons side by side (PicoCalc: file list, buttons, grid, VOLUME).
static const uint8_t ouiRow0[] = { 0, 1, 2, 3 };
#if BOARD_HAS_SCREENFILL
static const uint8_t ouiRow1[] = { 4, 5, OUI_FOC_SCREEN, OUI_FOC_HELP };
#else
static const uint8_t ouiRow1[] = { 4, 5, OUI_FOC_HELP };
#endif
static const uint8_t ouiRow2[] = { OUI_FOC_VOL };
static const uint8_t ouiRow3[] = { OUI_FOC_FILES };
static const uint8_t ouiRow4[] = { OUI_FOC_MOUNT, OUI_FOC_MNTREBOOT, OUI_FOC_REBOOT };
#if defined(BOARD_PICOCALC)   // files, buttons, grid, volume -- the order this board draws them in
static const uint8_t *const ouiRows[]  = { ouiRow3, ouiRow4, ouiRow0, ouiRow1, ouiRow2 };
static const uint8_t        ouiRowLen[] = { (uint8_t)(sizeof(ouiRow3)), (uint8_t)(sizeof(ouiRow4)),
                                            (uint8_t)(sizeof(ouiRow0)), (uint8_t)(sizeof(ouiRow1)),
                                            (uint8_t)(sizeof(ouiRow2)) };
#else
static const uint8_t *const ouiRows[]  = { ouiRow0, ouiRow1, ouiRow2, ouiRow3, ouiRow4 };
static const uint8_t        ouiRowLen[] = { (uint8_t)(sizeof(ouiRow0)), (uint8_t)(sizeof(ouiRow1)),
                                            (uint8_t)(sizeof(ouiRow2)), (uint8_t)(sizeof(ouiRow3)),
                                            (uint8_t)(sizeof(ouiRow4)) };
#endif
#define OUI_ROW_COUNT ((int)(sizeof(ouiRows) / sizeof(ouiRows[0])))

static void ouiFindCell(int &row, int &col)
{
  for (int r = 0; r < OUI_ROW_COUNT; r++)
    for (int c = 0; c < (int)ouiRowLen[r]; c++)
      if (ouiRows[r][c] == optionsUiFocus) { row = r; col = c; return; }
  row = 0; col = 0;                       // focus is on something not in the map: start over
}

// The live cell in `row` closest to column `col` (the same column first, then the nearer side,
// left before right), or -1 if nothing in that row can take the focus on this platform.
static int ouiNearestLive(int row, int col)
{
  const int n = (int)ouiRowLen[row];
  for (int d = 0; d < n; d++) {
    if (col - d >= 0 && col - d < n && ouiFocusable(ouiRows[row][col - d])) return col - d;
    if (col + d < n && ouiFocusable(ouiRows[row][col + d]))                  return col + d;
  }
  return -1;
}

// dx/dy: -1 = left/up, +1 = right/down (0 = no movement on that axis).
void optionsUiKeyArrow(int dx, int dy)
{
  if (ouiHelpOpen) { ouiCloseHelp(); return; }

  if (ouiEditing) {
    if (optionsUiFocus == OUI_FOC_VOL) {
      // Up and Right both mean "more", which is the only arrangement that feels right against
      // a horizontal slider drawn left-to-right. optionsUiAdjust takes -1 as "louder".
      if (dy < 0 || dx > 0) optionsUiAdjust(-1);
      else if (dy > 0 || dx < 0) optionsUiAdjust(+1);
      return;
    }
    if (optionsUiFocus == OUI_FOC_FILES) {
      if (dy) optionsUiAdjust(dy);
      else if (dx) for (int i = 0; i < OUI_FB_ROWS; i++) optionsUiAdjust(dx);   // page; clamps
      return;
    }
    ouiEditing = false;                   // nothing editable under the focus any more
  }

  int row = 0, col = 0;
  ouiFindCell(row, col);
  if (dx) {
    const int n = (int)ouiRowLen[row];
    for (int i = 0; i < n; i++) {                         // wrap within the row, over live cells
      col = (col + dx + n) % n;
      if (ouiFocusable(ouiRows[row][col])) break;
    }
  }
  if (dy) {
    for (int i = 0; i < OUI_ROW_COUNT; i++) {             // wrap between rows, skipping dead ones
      row = (row + dy + OUI_ROW_COUNT) % OUI_ROW_COUNT;
      int c = ouiNearestLive(row, col);                   // keep the column where one is live
      if (c >= 0) { col = c; break; }
    }
  }
  optionsUiFocus = ouiRows[row][col];
  optionsUiDirty = true;
}

void optionsUiKeyEnter(bool ctrl)
{
  if (ouiHelpOpen) { ouiCloseHelp(); return; }
  int f = optionsUiFocus;

  // Ctrl-Enter on the file list is the MOUNT+REBOOT button without leaving the list -- the same
  // call the button makes, so the two can never drift apart. Deliberately does NOT require the
  // list to be opened with a plain Enter first: the highlight is drawn either way, so demanding
  // an Enter before the Ctrl-Enter would be a keystroke with nothing behind it. A directory row
  // still just navigates, which ouiMountReboot/ouiPcMountC check for themselves -- rebooting
  // into a folder is not a thing.
  if (f == OUI_FOC_FILES && ctrl) {
    if (ouiIsPcxt()) ouiPcMountC();
    else if (currentPlatform == PLATFORM_APPLE2) ouiMountReboot();
    else ouiMount();      // C64/NES/Atari/MSX/SMS: no reboot variant, Ctrl-Enter = LOAD & RUN
    return;
  }

  // VOLUME and the file list hold a value rather than perform an action, so Enter opens them
  // and the arrows then work inside them.
  if (f == OUI_FOC_VOL || f == OUI_FOC_FILES) {
    if (!ouiEditing) { ouiEditing = true; optionsUiDirty = true; return; }
    if (f == OUI_FOC_VOL) { ouiEditing = false; optionsUiDirty = true; return; }   // done
    ouiMount();       // file list: a directory row navigates, a file row mounts (and closes)
    return;
  }
  optionsUiActivate();   // toggles flip, action buttons fire -- no mode to enter
}

// Returns true if Escape was consumed here. False means "nothing was open" and the caller
// should close the settings window.
bool optionsUiKeyEscape()
{
  if (ouiHelpOpen) { ouiCloseHelp(); return true; }
  if (ouiEditing)  { ouiEditing = false; optionsUiDirty = true; return true; }
  return false;
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
#if BOARD_HAS_SCREENFILL
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
        if (ouiIsDir(files[idx])) ouiBrowse(files[idx]);   // enter dir / go up
        else { shownFile = (uint8_t)idx; optionsUiDirty = true; }
      }
      return;
    }
  }

  // action buttons
  if (y >= OUI_ACT_TOP && y < OUI_ACT_TOP + OUI_ACT_H) {
    if (ouiIsPcxt()) {                        // MOUNT A: (4..106) | MOUNT C: (109..211) | REBOOT (214..316)
      if (x >= 4 && x < 106)        ouiPcMountA();
      else if (x >= 109 && x < 211) ouiPcMountC();
      else if (x >= 214 && x < 316) ouiReboot();
    } else if (ouiIsC64() || ouiIsNES() || ouiIsAtari() || ouiIsMsx() || ouiIsSms() || ouiIsColeco() || ouiIsZx()) {   // LOAD & RUN (6..126) | REBOOT (132..314)
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
#if defined(BOARD_PICOCALC)
  tft.setFullPanel(true);   // before the scans below: their progress bar draws in the list area
#endif
  if (ouiIsC64() && c64Files.empty()) loadC64FilesSync();   // populate the .prg browser
  if (ouiIsNES() && nesFiles.empty()) nesScanFiles();       // populate the .nes browser
  if (ouiIsAtari() && atariFiles.empty()) atariScanFiles(); // populate the .a26/.bin browser
  if (ouiIsMsx() && msxFiles.empty()) msxScanFiles();       // populate the .rom/.dsk browser
  if (ouiIsSms() && smsFiles.empty()) smsScanFiles();       // populate the .sms/.bin browser
  if (ouiIsColeco() && colecoFiles.empty()) colecoScanFiles(); // populate the .col/.rom browser
  if (ouiIsZx() && zxFiles.empty()) zxScanFiles();          // populate the .sna/.z80/.tap/.tzx browser
  if (ouiIsPcxt() && pcFiles.empty()) pcxtScanFiles();      // populate the disk-image browser
  // Reopen where the menu was left: same focused control (and still inside VOL / the file list if
  // it was closed from there), same highlighted row. Only the very first open, or a list that no
  // longer has that row, falls back to the mounted file.
  static bool opened = false;
  std::vector<std::string> &files = ouiFiles();
  if (!opened || shownFile >= files.size()) optionsUiSyncSelection();
  else if (shownFile < firstShowFile || shownFile >= firstShowFile + OUI_FB_ROWS)
    firstShowFile = (shownFile >= OUI_FB_ROWS) ? shownFile - OUI_FB_ROWS + 1 : 0;
  opened = true;
  if (optionsUiFocus != OUI_FOC_VOL && optionsUiFocus != OUI_FOC_FILES) ouiEditing = false;
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
