#include "../../emu.h"
#include "snapshot.h"
#include "filebrowser.h"

// The PicoCalc's 320x320 keyboard-driven menu. The desktop build can simulate it
// (-DEMU8_SIM_PICOCALC=ON, see CMakeLists.txt) so layout changes can be checked without the board.
#if UI_PICOCALC
#define OUI_PICOCALC 1
#else
#define OUI_PICOCALC 0
#endif

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

#if OUI_PICOCALC
// ---- Layout (320 x 320) ----
// The PicoCalc panel is square: the emulators letterbox into its middle 320x240, but the menu
// switches the display to full-panel mode (tft.setFullPanel) and spends the extra 80 rows on a
// taller grid and, mostly, on more and taller file rows drawn in the larger list font (font 3).
// Order differs from the touch boards: the file list comes first (it is what the menu is opened
// for), then its MOUNT / REBOOT buttons, then the option grid. There is no VOLUME row: the board
// has its own hardware volume control, so the app volume is pinned at maximum (optionsUiOpen) and
// its 23 rows went to one more file row and taller grid cards. The keyboard arrows
// (ouiNextFocus, further down) move by these positions, so Up/Down walk the page top to bottom.
#define OUI_HAS_VOLUME 0
#define OUI_SCR_H     320
#define OUI_TITLE_H   28
#define OUI_FB_TOP    30          // file browser header
#define OUI_FB_HDR_H  16
#define OUI_FB_LIST   46          // file rows
#define OUI_FB_ROWH   17
#define OUI_FB_ROWS   9
#define OUI_FB_FONT   3           // display_picocalc.cpp: FreeSans at 4/5, between fonts 1 and 2
#define OUI_ACT_TOP   203         // action buttons (or the key hints while the list is focused)
#define OUI_ACT_H     30
#define OUI_TG_TOP    237         // toggle grid: 4 columns x 2 rows, down to the bottom edge
#define OUI_TG_W      80
#define OUI_TG_H      41
#define OUI_VOL_TOP   0           // unused (OUI_HAS_VOLUME 0)
#define OUI_VOL_H     0
#define OUI_HELP_ROWH 15
#else
// ---- Layout (320 x 240) ----
#define OUI_HAS_VOLUME 1
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
static bool ouiActTwoButtons();             // action row layout (defined with the keyboard navigation)
static uint8_t ouiRomMode        = 0;       // ROMS page: 0 closed, 1 ROM list, 2 SD browser for one ROM
static uint8_t ouiGenMode        = 0;       // general settings (over the system menu): 0 closed, see ouiDrawGeneral
static uint8_t ouiSnapMode       = 0;       // snapshot page (Ctrl-Shift-F5): 0 closed, 1 the list
static int ouiGenCount();

// Joystick focus: left/right moves between controls; the focused one gets a white
// border. Order: the 8 toggle-grid slots, then volume, file list, MOUNT, SAVE & REBOOT.
// A grid slot's focus id is its index. The last two slots are always ROMS (6, when the machine
// has system ROMs) and HELP (7); SCREEN (fill / original, S3 panel + desktop window only) takes
// slot 5 when ROMS is there, else slot 6. The platform toggles fill slots 0..5 from the left.
// Empty slots are skipped (ouiFocusable). CPU speed readouts are in the title bar.
#define OUI_TG_COUNT      8
#define OUI_FOC_ROMS      6   // grid slot 6: ROMS page (when the machine has system ROMs)
#define OUI_FOC_HELP      7   // grid slot 7: HELP overlay
#define OUI_FOC_VOL       8
#define OUI_FOC_FILES     9
#define OUI_FOC_MOUNT     10  // Apple: MOUNT          / C64: LOAD & RUN
#define OUI_FOC_MNTREBOOT 11  // Apple: MOUNT + REBOOT / C64: (unused)
#define OUI_FOC_REBOOT    12  // both:  REBOOT
#define OUI_FOC_COUNT     13
// HELP is deliberately left OUT of the ring optionsUiNav() walks, so the analog stick behaves
// exactly as before -- on those boards HELP is a tap away. The keyboard navigation does reach
// it, because the PicoCalc has no touchscreen.
#if OUI_PICOCALC
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
// Whether the running machine loads any system ROM the user can pick: the ROMS card (grid slot 5).
static bool ouiHasRoms()
{
  for (int i = 0; i < ROMSEL_COUNT; i++) if (romSlotVisible(i)) return true;
  return false;
}
// Grid slot of the SCREEN toggle (-1 = none on this board): just left of ROMS, or of HELP.
static int ouiScreenSlot()
{
#if BOARD_HAS_SCREENFILL
  return ouiHasRoms() ? 5 : 6;
#else
  return -1;
#endif
}
// The PC-XT has the A:/C: two-slot disk UI. ouiPcA/ouiPcC are the A: floppy / C: hard-disk image markers.
static String &ouiPcA() { return selectedPcFileName; }
static String &ouiPcC() { return selectedPcHdFileName; }

static FileList &ouiFiles()
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
  if (ouiIsPcxt()) return std::string((selectedPcFileName.length() ? selectedPcFileName : selectedPcHdFileName).c_str());
  return std::string((HdDisk ? selectedHdFileName : selectedDiskFileName).c_str());
}

// A browser entry is a directory if it's the ".." up-entry or ends with "/".
static bool ouiIsDir(const std::string &e) { return e == ".." || (!e.empty() && e.back() == '/'); }
// MSX with a .dsk highlighted: the action row becomes MOUNT/UNMOUNT | MOUNT&RUN | REBOOT
// (a .rom keeps LOAD & RUN | REBOOT).
static bool ouiMsxDsk()
{
  if (!ouiIsMsx()) return false;
  FileList &fl = ouiFiles();
  if (shownFile >= fl.size() || ouiIsDir(fl[shownFile])) return false;
  const std::string &n = fl[shownFile];
  return n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".dsk") == 0;
}
static bool ouiMsxDskCur() { return ouiMsxDsk() && msxDiskMounted(ouiFiles()[shownFile].c_str()); }
// The highlighted entry is loaded / mounted in the running machine (media.cpp): on every platform
// but the PC-XT (which has its own EJECT A: / EJECT C: pair) the first action button becomes
// UNMOUNT, and Del unmounts it from the list on every platform.
static bool ouiCurMounted()
{
  FileList &fl = ouiFiles();
  return shownFile < fl.size() && !ouiIsDir(fl[shownFile]) && mediaIsMounted(fl[shownFile].c_str());
}
// UNMOUNT the highlighted entry. Stays in settings so the list marker and the button update; a
// cartridge / program / snapshot restarts the machine (empty) when the window closes.
static void ouiUnmount()
{
  FileList &fl = ouiFiles();
  if (shownFile < fl.size() && !ouiIsDir(fl[shownFile]) && mediaUnmount(fl[shownFile].c_str()))
    optionsUiDirty = true;
}

// Why the active browser's entry idx cannot be loaded (drawn grey, reason in the list header), or
// nullptr. Only the C64 browser lists files it cannot load today.
static const char *ouiEntryProblem(int idx)
{
  if (idx < 0) return nullptr;
  if (ouiIsC64()) return c64FileProblem((size_t)idx);
  return nullptr;
}

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
  shownFile = SHOWN_NONE; firstShowFile = 0;
  optionsUiSyncSelection();
  optionsUiDirty = true;
}

// ---------------------------------------------------------------------------
// Selection / scrolling helpers
// ---------------------------------------------------------------------------
void optionsUiSyncSelection()
{
  FileList &files = ouiFiles();
  std::string sel = ouiSel();
  int idx = -1;
  for (int i = 0; i < (int)files.size(); i++)
    if (files[i] == sel) { idx = i; break; }
  if (idx < 0) idx = 0;
  shownFile = (uint16_t)idx;
  if (files.empty()) { firstShowFile = 0; return; }
  if (shownFile < firstShowFile) firstShowFile = shownFile;
  else if (shownFile >= firstShowFile + OUI_FB_ROWS) firstShowFile = shownFile - OUI_FB_ROWS + 1;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
// Incremental repaint. optionsUiDirty only says "something may have changed"; every element then
// compares a signature of what it would draw now (a hash of its text, colours and focus state)
// with the one it last drew, and leaves the panel alone when they match. Moving the focus thus
// repaints the two controls involved, a file-list step the two rows involved, and so on, instead
// of the whole page. A full-page wipe (optionsUiFirstDraw) must call ouiInvalidate().
#define OUI_SIG_NONE    0u            // nothing known on screen: always draw
#define OUI_SIG_NOFILES 1u            // file list showing "No images on SD card"
#define OUI_SIG_CLEARED 2u            // toggle slot blanked
#define OUI_SIG_EMPTY   3u            // file list / row showing no file
static uint32_t ouiSigTitle, ouiSigCard[8], ouiSigVol, ouiSigHdr, ouiSigRow[OUI_FB_ROWS], ouiSigAct;
static uint8_t  ouiRingDrawn;         // file-list focus ring as last drawn: 0 off, 1 white, 2 green
static bool     ouiScrollDrawn;
static uint32_t ouiSigSnapThumb, ouiSigSnapInfo, ouiSigSnapBtns;   // snapshot page (ouiDrawSnap)

static void ouiInvalidateRows()
{
  for (int i = 0; i < OUI_FB_ROWS; i++) ouiSigRow[i] = OUI_SIG_NONE;
  ouiRingDrawn = 0xff;
}

static void ouiInvalidate()
{
  ouiSigTitle = ouiSigVol = ouiSigHdr = ouiSigAct = OUI_SIG_NONE;
  for (int i = 0; i < 8; i++) ouiSigCard[i] = OUI_SIG_NONE;
  ouiInvalidateRows();
  ouiScrollDrawn = false;
  ouiSigSnapThumb = ouiSigSnapInfo = ouiSigSnapBtns = OUI_SIG_NONE;
}

static uint32_t ouiHash(const char *s, uint32_t h = 2166136261u)   // FNV-1a
{
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h > OUI_SIG_EMPTY ? h : h + 4;                            // never one of the markers
}

// True (and remembered) when `sig` differs from what `slot` last drew.
static bool ouiChanged(uint32_t &slot, uint32_t sig)
{
  if (slot == sig) return false;
  slot = sig;
  return true;
}

// Focus state of control f as it affects drawing: 0 unfocused, 1 focused, 2 focused + editing.
static int ouiFocusState(int f) { return optionsUiFocus == f ? 1 + (int)ouiEditing : 0; }

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
  char sig[48];
  snprintf(sig, sizeof(sig), "%s|%s|%u|%d", label, value, (unsigned)valColor, ouiFocusState(idx));
  if (!ouiChanged(ouiSigCard[idx], ouiHash(sig))) return;
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
  // Font 2, unless the value (SCREEN: ENLARGE / STRETCH) would run past the card.
  int vf = 2;
#if UI_PICOCALC
  if (tft.textWidth(value, vf) > OUI_TG_W - 12) vf = 3;
  if (tft.textWidth(value, vf) > OUI_TG_W - 12) vf = 1;
#else
  if (strlen(value) >= 7) vf = 1;
#endif
  tft.drawString(value, x + 8, y + OUI_TG_H - 7, vf);
}

// Blank a toggle slot (so stale Apple labels don't linger on the C64 screen).
static void ouiClearToggle(int idx)
{
  if (!ouiChanged(ouiSigCard[idx], OUI_SIG_CLEARED)) return;
  int col = idx % 4, row = idx / 4;
  int x = col * OUI_TG_W, y = OUI_TG_TOP + row * OUI_TG_H;
  tft.fillRect(x, y, OUI_TG_W, OUI_TG_H, OUI_BG);
}

// Blank the platform slots from `first` on. Slot 7 is always HELP, slot 6 ROMS when the machine
// has them, and SCREEN sits in 5 or 6 where the board has it: clearing those only for them to be
// drawn again would repaint them on every pass.
static void ouiClearToggles(int first)
{
  for (int i = first; i < OUI_FOC_HELP; i++) {
    if (i == ouiScreenSlot()) continue;
    if (i == OUI_FOC_ROMS && ouiHasRoms()) continue;   // ROMS card (ouiDrawRomsButton)
    ouiClearToggle(i);
  }
}

// Boards with BOARD_HAS_SCREENFILL: a SCREEN toggle (drawn after the platform toggles clear the
// rest). ORIG = native picture; ENLARGE = scaled up keeping its shape; STRETCH = the whole screen.
static void ouiDrawScreenToggle()
{
#if BOARD_HAS_SCREENFILL
  static const char *const kScreenMode[SCREEN_MODES] = { "ORIG", "ENLARGE", "STRETCH" };
  ouiDrawToggle(ouiScreenSlot(), "SCREEN", kScreenMode[screenFill < SCREEN_MODES ? screenFill : 0], OUI_TXT);
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
    ouiClearToggles(5);
    ouiDrawScreenToggle();
    return;
  }
  if (ouiIsMsx()) {                           // MSX grid: SOUND / JOYSTICK / VIDEO / SPEED / DISK ROM (Z80 MHz in the title)
    ouiDrawToggle(0, "SOUND",    sound ? "ON" : "MUTE",          OUI_TXT);
    ouiDrawToggle(1, "JOYSTICK", joystick ? "ON" : "OFF",        OUI_TXT);
    ouiDrawToggle(2, "VIDEO",    videoColor ? "COLOR" : "MONO",  OUI_TXT);
    ouiDrawToggle(3, "SPEED",    msxFast ? "FAST" : "NORMAL",    OUI_TXT);
    ouiDrawToggle(4, "DISK ROM", msxDiskRom == MSX_DISKROM_ON ? "ON" : msxDiskRom == MSX_DISKROM_OFF ? "OFF" : "AUTO", OUI_TXT);
    ouiClearToggles(5);
    ouiDrawScreenToggle();
    return;
  }
  if (ouiIsSms() || ouiIsColeco() || ouiIsZx()) {   // SMS / Coleco / ZX grid: SOUND / JOYSTICK / VIDEO / SPEED (Z80 MHz in the title)
    const bool fast = ouiIsSms() ? smsFast : ouiIsZx() ? zxFast : colecoFast;
    ouiDrawToggle(0, "SOUND",    sound ? "ON" : "MUTE",          OUI_TXT);
    ouiDrawToggle(1, "JOYSTICK", joystick ? "ON" : "OFF",        OUI_TXT);
    ouiDrawToggle(2, "VIDEO",    videoColor ? "COLOR" : "MONO",  OUI_TXT);
    ouiDrawToggle(3, "SPEED",    fast ? "FAST" : "NORMAL",       OUI_TXT);
    ouiClearToggles(4);
    ouiDrawScreenToggle();
    return;
  }
  if (ouiIsPcxt()) {                          // PCXT grid: SOUND / VIDEO (8086 MHz in the title)
    ouiDrawToggle(0, "SOUND",    sound ? "ON" : "MUTE",          OUI_TXT);
    ouiDrawToggle(1, "VIDEO",    videoColor ? "COLOR" : "MONO",  OUI_TXT);
    ouiClearToggles(2);
    ouiDrawScreenToggle();
    return;
  }
  if (ouiIsNES() || ouiIsAtari()) {           // NES / Atari grid: SOUND / JOYSTICK / VIDEO
    ouiDrawToggle(0, "SOUND",    sound ? "ON" : "MUTE",          OUI_TXT);
    ouiDrawToggle(1, "JOYSTICK", joystick ? "ON" : "OFF",        OUI_TXT);
    ouiDrawToggle(2, "VIDEO",    videoColor ? "COLOR" : "MONO",  OUI_TXT);
    if (ouiIsNES()) {                         // NES: speed control (2A03 MHz in the title)
      ouiDrawToggle(3, "SPEED", nesFast ? "FAST" : "NORMAL", OUI_TXT);
#if BOARD_DISPLAY_GFX
      const char *sv = (nesDisplaySkip <= 1) ? "OFF" : (nesDisplaySkip == 2) ? "2" : "3";
      ouiDrawToggle(4, "SKIP", sv, OUI_TXT);  // S3 only: display frame-skip (smoothness vs core-1 time)
      ouiClearToggles(5);
#else
      ouiClearToggles(4);
#endif
    } else {
#if defined(BOARD_PICOCALC_P4) || defined(BOARD_DESKTOP)
      ouiDrawToggle(3, "SPEED", atariFast ? "FAST" : "NORMAL", OUI_TXT);   // the hosts with an Atari pacer
      ouiClearToggles(4);
#else
      ouiClearToggles(3);   // Atari: no speed control
#endif
    }
    ouiDrawScreenToggle();   // slot 6 (S3); drawn after the clears
    return;
  }
  // No MACHINE card: II+ and IIe are two separate systems on the boot splash now, and the memory
  // map is built for the chosen one at startup (src/apple2/memory.cpp), so the model cannot be
  // flipped from here any more. Which one is running is in the title bar instead. The grid is
  // therefore DEVICE SPEED SOUND JOYSTICK VIDEO, 5 SCREEN, 6 ROMS, 7 HELP.
  ouiDrawToggle(0, "DEVICE",   HdDisk ? "HD" : "DISK",          OUI_TXT);
  ouiDrawToggle(1, "SPEED",    Fast1MhzSpeed ? "FAST" : "1MHz", OUI_TXT);
  ouiDrawToggle(2, "SOUND",    sound ? "ON" : "MUTE",           OUI_TXT);
  ouiDrawToggle(3, "JOYSTICK", joystick ? "ON" : "OFF",         OUI_TXT);
  ouiDrawToggle(4, "VIDEO",    videoColor ? "COLOR" : "MONO",   OUI_TXT);
  ouiClearToggles(5);
  ouiDrawScreenToggle();   // SCREEN (FILL/ORIG); 6 = ROMS, 7 = HELP. 6502 MHz shows in the title bar.
}

static void ouiDrawVolume()
{
  if (!OUI_HAS_VOLUME) return;
  if (!ouiChanged(ouiSigVol, 16u + (uint32_t)volume * 4u + (uint32_t)ouiFocusState(OUI_FOC_VOL))) return;
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
  FileList &files = ouiFiles();

  // header: the highlighted entry's problem, when it has one, in place of the list title
  const char *problem = (shownFile < files.size()) ? ouiEntryProblem(shownFile) : nullptr;
  char hdr[64];
  if (problem) snprintf(hdr, sizeof(hdr), "! %s", problem);
  else sprintf(hdr, "%s  (%d%s)", ouiIsC64() ? "PRG/D64/CRT" : ouiIsNES() ? "NES ROMS"
                         : ouiIsAtari() ? "A26/BIN ROMS"
                         : ouiIsMsx() ? "MSX ROM/DSK"
                         : ouiIsSms() ? "SMS ROMS"
                         : ouiIsColeco() ? "CARTRIDGES"
                         : ouiIsZx() ? "SNA/Z80/TAP"
                         : ouiIsPcxt() ? "PC DISK IMG"
                         : (HdDisk ? "HD IMAGES" : "DISK IMAGES"),
          (int)files.size(), fbListTruncated(files) ? "+" : "");   // "+": the folder holds more
  if (ouiChanged(ouiSigHdr, ouiHash(hdr))) {
    tft.fillRect(0, OUI_FB_TOP, 320, OUI_FB_HDR_H, OUI_BG);
    tft.setTextDatum(BL_DATUM);
    tft.setTextColor(problem ? tft.color565(255, 170, 60) : OUI_LBL, OUI_BG);
    tft.drawString(hdr, 7, OUI_FB_TOP + OUI_FB_HDR_H - 2, 1);
  }

  // The focus ring is drawn over the rows, so turning it off (or recolouring it) means repainting
  // every row underneath; while it stays on, only a repainted row needs it put back.
  const uint8_t ring = (uint8_t)ouiFocusState(OUI_FOC_FILES);
  bool ringNew = false;
  if (ring != ouiRingDrawn) { ouiInvalidateRows(); ouiRingDrawn = ring; ringNew = true; }
  bool rowsDrawn = false;
  // OUI_RING_INSET: while the ring is up the rows paint INSIDE it (2px in from every edge), so a
  // scroll repaints rows without touching the ring and the ring is drawn once, when it appears.
  // Elsewhere the rows fill the full width and the ring is put back after every repaint.
#if defined(BOARD_PICOCALC_P4)
  const int ix = ring ? 2 : 0;
#else
  const int ix = 0;
#endif
  auto rowFill = [&](int r, int ry, uint16_t c) {
    const int t = (r == 0) ? ix : 0, b = (r == OUI_FB_ROWS - 1) ? ix : 0;
    tft.fillRect(ix, ry + t, 300 - 2 * ix, OUI_FB_ROWH - t - b, c);
  };

  int listH = OUI_FB_ROWS * OUI_FB_ROWH;
  if (files.empty()) {
    if (ouiSigRow[0] != OUI_SIG_NOFILES) {
      for (int r = 0; r < OUI_FB_ROWS; r++) ouiSigRow[r] = OUI_SIG_NOFILES;
      tft.fillRect(ix, OUI_FB_LIST + ix, 300 - 2 * ix, listH - 2 * ix, OUI_CARD2);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(OUI_LBL, OUI_CARD2);
      tft.drawString("No images on SD card", 150, OUI_FB_LIST + listH / 2, 1);
      rowsDrawn = true;
    }
  } else {
    for (int r = 0; r < OUI_FB_ROWS; r++) {
      int idx = firstShowFile + r;
      int ry = OUI_FB_LIST + r * OUI_FB_ROWH;
      if (idx >= (int)files.size()) {
        if (ouiChanged(ouiSigRow[r], OUI_SIG_EMPTY)) { rowFill(r, ry, OUI_CARD2); rowsDrawn = true; }
        continue;
      }

      bool selected = (idx == shownFile);
      bool mntA = ouiIsPcxt() && ouiPcA().length() && (files[idx] == std::string(ouiPcA().c_str()));
      bool mntC = ouiIsPcxt() && ouiPcC().length() && (files[idx] == std::string(ouiPcC().c_str()));
      bool mounted  = mntA || mntC || (!ouiIsDir(files[idx]) && mediaIsMounted(files[idx].c_str()));
      const bool bad = ouiEntryProblem(idx) != nullptr;
      const char flags[] = { (char)('0' + selected), (char)('0' + mounted), (char)('0' + mntA), (char)('0' + mntC), (char)('0' + bad), 0 };
      if (!ouiChanged(ouiSigRow[r], ouiHash(flags, ouiHash(files[idx].c_str())))) continue;
      rowsDrawn = true;
      uint16_t rowbg = selected ? OUI_SEL : OUI_CARD2;
      rowFill(r, ry, rowbg);
      if (mounted) tft.fillRect(ix, ry + (r == 0 ? ix : 0), 3, OUI_FB_ROWH - (r == 0 || r == OUI_FB_ROWS - 1 ? ix : 0), OUI_ON);

      // Every browser can now be inside a subdirectory, so every row gets the same treatment:
      // ".." as-is, "[name]" for a directory, otherwise the basename with the path stripped.
      std::string nm = ouiDisplayName(files[idx]);
#if OUI_PICOCALC
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
                      : bad ? tft.color565(110, 114, 124)
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

  // scroll buttons (right column): static, so only after a wipe
  if (!ouiScrollDrawn) {
    int sX = 302, sW = 18, half = listH / 2;
    ouiSmallBtn(sX, OUI_FB_LIST, sW, half - 1, "^", OUI_CARD);
    ouiSmallBtn(sX, OUI_FB_LIST + half + 1, sW, half - 1, "v", OUI_CARD);
    ouiScrollDrawn = true;
  }

  if (ring && (ix ? ringNew : rowsDrawn)) ouiFocusRing(0, OUI_FB_LIST, 300, listH, 2);
}

// Called from the (slow) directory scan in the render task: shows a "Loading… N" bar in the
// file-list area and yields (vTaskDelay) so the scan doesn't block the task / trip the watchdog.
void uiDirScanProgress(int count)
{
  int y = OUI_FB_LIST, listH = OUI_FB_ROWS * OUI_FB_ROWH;
  tft.fillRect(0, y, 320, listH, OUI_CARD2);
  ouiInvalidateRows(); ouiScrollDrawn = false;   // painted over the rows and the scroll buttons
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_TXT, OUI_CARD2);
  char s[24];
  sprintf(s, "Loading...  %d", count);
  tft.drawString(s, 160, y + listH / 2 - 9, 2);
  int bx = 24, bw = 272, bh = 8, by = y + listH / 2 + 6;
  tft.drawRoundRect(bx, by, bw, bh, 3, OUI_BORDER);
  int fw = count >= FB_MAX_ENTRIES ? bw - 2 : (bw - 2) * count / FB_MAX_ENTRIES;   // toward the cap
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

#if OUI_PICOCALC
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
  FileList &fl = ouiFiles();
  const bool dir = shownFile < fl.size() && ouiIsDir(fl[shownFile]);
  // What Enter (inside the list) and Ctrl-Enter do -- mirrors optionsUiKeyEnter / ouiMount.
  const char *enterAct = dir               ? "open folder"
                       : ouiIsPcxt()       ? "mount A:"
                       : ouiIsC64()        ? "mount"
                       : (ouiIsC64() || ouiIsNES() || ouiIsAtari() || ouiIsMsx() || ouiIsSms() || ouiIsColeco() || ouiIsZx())
                                           ? "load & run" : "mount";
  const char *ctrlAct  = dir               ? NULL
                       : ouiIsPcxt()       ? "mount C:"
                       : ouiIsMsx()        ? "mount"
                       : ouiIsC64()        ? "mount & run"
                       : (currentPlatform == PLATFORM_APPLE2) ? "mount + reboot" : NULL;
  const bool delAct = !dir && ouiCurMounted();   // Del: unmount the highlighted (loaded) entry

  char sig[48];
  snprintf(sig, sizeof(sig), "H|%d|%s|%s|%d", (int)ouiEditing, enterAct, ctrlAct ? ctrlAct : "", (int)delAct);
  if (!ouiChanged(ouiSigAct, ouiHash(sig))) return;

  const int y = OUI_ACT_TOP, h = OUI_ACT_H;
  tft.fillRect(0, y, 320, h, OUI_BG);
  tft.fillRoundRect(4, y, 312, h, 6, OUI_CARD2);
  tft.drawRoundRect(4, y, 312, h, 6, OUI_BORDER);
  const int y1 = y + h / 4 + 1, y2 = y + (3 * h) / 4 - 1;
  if (ouiEditing) {
    const char *l1[] = { "Enter ", enterAct, "    Ctrl+Enter ", ctrlAct };
    ouiHintLine(y1, l1, ctrlAct ? 4 : 2);
    const char *l2[] = { "Arrows ", "select", "    Esc ", "back", "    Del ", "unmount" };
    ouiHintLine(y2, l2, delAct ? 6 : 4);
  } else {
    const char *l1[] = { "Enter ", "browse list", "    Ctrl+Enter ", ctrlAct };
    ouiHintLine(y1, l1, ctrlAct ? 4 : 2);
    const char *l2[] = { "Del ", "unmount", "    Arrows ", "other controls" };
    const char *l2n[] = { "Arrows ", "move to other controls" };
    if (delAct) ouiHintLine(y2, l2, 4); else ouiHintLine(y2, l2n, 2);
  }
}
#endif

static void ouiDrawActions()
{
#if OUI_PICOCALC
  if (optionsUiFocus == OUI_FOC_FILES) { ouiDrawFileHints(); return; }
#endif
  bool canMount = !ouiFiles().empty();
  uint16_t mc = canMount ? OUI_MOUNT : OUI_CARD2;
  uint16_t mt = canMount ? OUI_TXT : OUI_LBL;
  bool curA = false, curC = false;
  if (ouiIsPcxt()) {
    FileList &fl = ouiFiles();
    curA = shownFile < fl.size() && ouiPcA().length() && (fl[shownFile] == std::string(ouiPcA().c_str()));
    curC = shownFile < fl.size() && ouiPcC().length() && (fl[shownFile] == std::string(ouiPcC().c_str()));
  }
  const int f = optionsUiFocus;
  const bool actFocus = f == OUI_FOC_MOUNT || f == OUI_FOC_MNTREBOOT || f == OUI_FOC_REBOOT;
  const bool msxDsk = ouiMsxDsk(), cur = !ouiIsPcxt() && ouiCurMounted();
  char sig[48];
  snprintf(sig, sizeof(sig), "B|%d|%d|%d|%d|%d|%d|%d", (int)currentPlatform, (int)canMount, (int)curA, (int)curC,
           (int)msxDsk, (int)cur, actFocus ? f : -1);
  if (!ouiChanged(ouiSigAct, ouiHash(sig))) return;
#if OUI_PICOCALC
  tft.fillRect(0, OUI_ACT_TOP, 320, OUI_ACT_H, OUI_BG);   // wipe the hint panel from the gaps
#endif

  if (ouiIsPcxt()) {                       // PC-XT: MOUNT/EJECT A: | MOUNT/EJECT C: | REBOOT
    ouiActBtn(4,   102, curA ? "EJECT A:" : "MOUNT A:", curA ? OUI_RED : mc, curA ? OUI_TXT : mt, OUI_FOC_MOUNT);
    ouiActBtn(109, 102, curC ? "EJECT C:" : "MOUNT C:", curC ? OUI_RED : mc, curC ? OUI_TXT : mt, OUI_FOC_MNTREBOOT);
    ouiActBtn(214, 102, "REBOOT",   OUI_REBOOT, OUI_TXT, OUI_FOC_REBOOT);
    return;
  }
  if (ouiActTwoButtons()) {                // loaders, nothing loaded highlighted: LOAD & RUN | REBOOT
    ouiActBtn(6,   120, "LOAD & RUN", mc, mt, OUI_FOC_MOUNT);
    ouiActBtn(132, 182, "REBOOT",     OUI_REBOOT, OUI_TXT, OUI_FOC_REBOOT);
    return;
  }
  // MOUNT, or UNMOUNT when the highlighted entry is the loaded one (every platform) | the core's
  // own mount-and-start (MSX .dsk: MOUNT&RUN, Apple II: M+REBOOT, the loaders: LOAD & RUN) | REBOOT
  const char *mid = msxDsk ? "MOUNT&RUN" : (currentPlatform == PLATFORM_APPLE2) ? "M+REBOOT" : "LOAD & RUN";
  ouiActBtn(4,   102, cur ? "UNMOUNT" : "MOUNT", cur ? OUI_RED : mc, cur ? OUI_TXT : mt, OUI_FOC_MOUNT);
  ouiActBtn(109, 102, mid,      mc,         mt,      OUI_FOC_MNTREBOOT);
  ouiActBtn(214, 102, "REBOOT", OUI_REBOOT, OUI_TXT, OUI_FOC_REBOOT);
}

static int ouiRomTextW(const char *s, int font);

#if OUI_PICOCALC
// Battery gauge for the title bars and the system menu: "87%" then a small cell icon filled to the
// charge, green / amber / red by level, cyan while charging (with "CHG" in front of the number).
uint32_t uiBatterySig()
{
  return (uint32_t)(picocalcBatteryPercent() + 1) | (picocalcBatteryCharging() ? 0x1000u : 0u);
}

int uiDrawBattery(int rightX, int cy, uint16_t bg)
{
  const int pct = picocalcBatteryPercent();
  if (pct < 0) return rightX;
  const bool chg = picocalcBatteryCharging();
  const int bw = 18, bh = 9, nub = 2;                 // cell body + positive terminal
  const int bx = rightX - nub - bw, by = cy - bh / 2;
  uint16_t fill = chg ? tft.color565(60, 200, 230)
                : pct > 50 ? tft.color565(40, 200, 95)
                : pct > 20 ? tft.color565(230, 170, 40)
                           : tft.color565(230, 60, 50);
  tft.fillRect(bx - 40, cy - 6, rightX - bx + 40, 13, bg);   // room for "CHG 100%"
  tft.fillRect(bx, by, bw, 1, TFT_WHITE);            // outline (DisplayGFX has no drawRect)
  tft.fillRect(bx, by + bh - 1, bw, 1, TFT_WHITE);
  tft.fillRect(bx, by, 1, bh, TFT_WHITE);
  tft.fillRect(bx + bw - 1, by, 1, bh, TFT_WHITE);
  tft.fillRect(bx + bw, cy - 2, nub, 4, TFT_WHITE);
  int w = (bw - 4) * pct / 100;
  if (w < 1 && pct > 0) w = 1;
  if (w > 0) tft.fillRect(bx + 2, by + 2, w, bh - 4, fill);
  char s[12];
  snprintf(s, sizeof(s), chg ? "CHG %d%%" : "%d%%", pct);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(s, bx - 3, cy, 1);
  return bx - 3 - tft.textWidth(s, 1);
}
#endif

#define OUI_SNAP_BTN_W 50   // touch boards: the title bar's SNAP button, left of X

static void ouiDrawTitle()
{
  const char *title = ouiIsC64() ? "COMMODORE 64"
               : ouiIsNES() ? "NINTENDO  NES"
               : ouiIsAtari() ? "ATARI 2600"
               : ouiIsMsx() ? "MSX1"
               : ouiIsSms() ? "MASTER SYSTEM"
               : ouiIsColeco() ? "COLECOVISION"
               : ouiIsZx() ? "ZX SPECTRUM 48K"
               : ouiIsPcxt() ? "PC-XT"
               : AppleIIe ? "APPLE IIe" : "APPLE II+";
  // Every CPU speed readout lives here, right-aligned before the close button, followed by the
  // panel frame rate (fpsFrameShown() in video.cpp). Atari has no CPU readout, only the fps.
  char mhz[32] = "";
  int  n = 0;
  if (currentPlatform == PLATFORM_APPLE2)       // live, measured in cpuLoop
    n = snprintf(mhz, sizeof(mhz), "6502 %.2fMHz", appleMeasuredMhz);
  else if (ouiIsC64())                          // live, measured in cpuLoop
    n = snprintf(mhz, sizeof(mhz), "6510 %.2fMHz", c64MeasuredMhz);
  else if (ouiIsNES())                          // derived from fps x cycles-per-frame
    n = snprintf(mhz, sizeof(mhz), "2A03 %.2fMHz", nesMeasuredMhz);
  else if (ouiIsMsx() || ouiIsSms() || ouiIsColeco() || ouiIsZx())   // live, speedmeter.h
    n = snprintf(mhz, sizeof(mhz), "Z80 %.2fMHz", ouiIsMsx() ? msxMeasuredMhz : ouiIsSms() ? smsMeasuredMhz
                                              : ouiIsZx() ? zxMeasuredMhz : colecoMeasuredMhz);
  else if (ouiIsPcxt())                         // 8086-equivalent, live, speedmeter.h
    n = snprintf(mhz, sizeof(mhz), "8086 %.2fMHz", pcMeasuredMhz);
  if (n < 0 || n >= (int)sizeof(mhz)) n = (int)strlen(mhz);
  snprintf(mhz + n, sizeof(mhz) - n, "%s%dfps", n ? "  " : "", (int)(emuFps + 0.5f));
  uint32_t sig = ouiHash(mhz, ouiHash(title));
#if OUI_PICOCALC
  sig = ouiHash("bat", sig ^ uiBatterySig());
#endif
  if (!ouiChanged(ouiSigTitle, sig)) return;

  tft.fillRect(0, 0, 320, OUI_TITLE_H, OUI_TITLE);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(OUI_TXT, OUI_TITLE);
  tft.drawString(title, 10, OUI_TITLE_H / 2, 2);
  int cw = OUI_TITLE_H, cx = 320 - cw;
  int rx = cx - 8;                                    // right edge for the readouts
#if OUI_PICOCALC
  int bl = uiDrawBattery(rx, OUI_TITLE_H / 2, OUI_TITLE);
  if (bl != rx) rx = bl - 8;
#else
  // Touch boards have no Ctrl-Shift-F5: SNAP opens the snapshot page.
  if (snapSystemName()) {
    const int sx = cx - OUI_SNAP_BTN_W - 4;
    ouiSmallBtn(sx, 3, OUI_SNAP_BTN_W, OUI_TITLE_H - 6, "SNAP", tft.color565(40, 130, 75));
    rx = sx - 8;
  }
#endif
  if (mhz[0]) {
    // Too wide for the gap after the title (long names like "ZX SPECTRUM 48K")? Drop the CPU name,
    // and if that is still too wide, keep only the fps.
    const char *speed = mhz;
    const int room = rx - (10 + ouiRomTextW(title, 2) + 10);
    if (ouiRomTextW(speed, 1) > room && strchr(mhz, ' ') && strstr(mhz, "MHz")) speed = strchr(mhz, ' ') + 1;
    if (ouiRomTextW(speed, 1) > room && strrchr(speed, ' ')) speed = strrchr(speed, ' ') + 1;
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(OUI_TXT, OUI_TITLE);
    tft.drawString(speed, rx, OUI_TITLE_H / 2, 1);
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
  int idx = OUI_FOC_HELP, col = idx % 4, row = idx / 4;
  if (!ouiChanged(ouiSigCard[idx], 16u + (uint32_t)ouiFocusState(idx))) return;
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

// ROMS button: grid slot 6, just left of HELP, on every machine that loads system ROMs (all but
// NES / Atari / SMS). Opens the ROMS page, where each ROM can be pointed at another file on the SD card.
static void ouiDrawRomsButton()
{
  if (!ouiHasRoms()) return;
  int idx = OUI_FOC_ROMS, col = idx % 4, row = idx / 4;
  if (!ouiChanged(ouiSigCard[idx], 32u + (uint32_t)ouiFocusState(idx))) return;
  int x = col * OUI_TG_W, y = OUI_TG_TOP + row * OUI_TG_H;
  uint16_t face = tft.color565(92, 70, 130);
  tft.fillRoundRect(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5, face);
  tft.drawRoundRect(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5, OUI_BORDER);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_TXT, face);
  tft.drawString("ROMS", x + OUI_TG_W / 2, y + OUI_TG_H / 2, 2);
  if (optionsUiFocus == idx) ouiFocusRing(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5);
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
#if OUI_PICOCALC
  ouiHelpRow(y, "Ctrl-F1",       "Open / close menu");
  ouiHelpRow(y, "Ctrl-Shift-F1", "System menu");
  ouiHelpRow(y, "Ctrl-Shift-F3", "Hard reset (same system)");
  ouiHelpRow(y, "Ctrl-Shift-F5", "Snapshots: save / load state");
#else
  ouiHelpRow(y, "F10 / SEL+START", "Open / close menu");
  ouiHelpRow(y, "Vol +/-",       "Volume (media keys)");
  ouiHelpRow(y, "Ctrl-Shift-F5", "Snapshots: save / load state");
#endif
  ouiHelpHdr(y, "IN THIS MENU");
  ouiHelpRow(y, "Arrows",        "Browse");
  ouiHelpRow(y, "Enter",         "Toggle / open / mount");
  ouiHelpRow(y, "Ctrl-Enter",    "Mount + reboot");
  ouiHelpRow(y, "Del",           "Unmount the loaded file");
  ouiHelpRow(y, "Esc",           "Back, then close");

  if (ouiIsNES()) {
    ouiHelpHdr(y, "NES  -  KEYBOARD");
    ouiHelpRow(y, "Arrows",      "D-pad");
    ouiHelpRow(y, "X / Z",       "A / B");
    ouiHelpRow(y, "Enter / Tab", "Start / Select");
#if !OUI_PICOCALC   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "NES  -  GAMEPAD");
    ouiHelpRow(y, "D-pad",       "D-pad");
    ouiHelpRow(y, "A / B",       "A / B");
    ouiHelpRow(y, "Start / Sel", "Start / Select");
#endif
  } else if (ouiIsAtari()) {
    ouiHelpHdr(y, "ATARI  -  KEYBOARD");
    ouiHelpRow(y, "Arrows",    "Joystick");
#if OUI_PICOCALC
    ouiHelpRow(y, "F5 / Space", "Fire");
    ouiHelpRow(y, "F3 / Enter", "Reset switch");
    ouiHelpRow(y, "F4 / Tab",   "Select switch");
#else
    ouiHelpRow(y, "Space / X", "Fire");
    ouiHelpRow(y, "Enter",     "Reset switch");
    ouiHelpRow(y, "Tab",       "Select switch");
#endif
#if !OUI_PICOCALC   // no gamepad port on this board; see the note at the Apple section
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
    ouiHelpRow(y, "Enter",      "Files: mount");
    ouiHelpRow(y, "Ctrl-Enter", "Files: mount + run");
#if !OUI_PICOCALC   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "C64  -  GAMEPAD");
    ouiHelpRow(y, "D-pad", "Stick");
    ouiHelpRow(y, "A",     "Fire");
#endif
  } else if (ouiIsMsx()) {
    ouiHelpHdr(y, "MSX  -  KEYBOARD");
    ouiHelpRow(y, "Keys",      "MSX layout");
    ouiHelpRow(y, "Arrows",    "Cursor / Joystick");
    ouiHelpRow(y, "Space",     "Trigger / Space");
    ouiHelpRow(y, "Ctrl-Enter", "Files: mount only");
    ouiHelpRow(y, "Del",        "Files: unmount");
#if !OUI_PICOCALC   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "MSX  -  GAMEPAD");
    ouiHelpRow(y, "D-pad", "Stick");
    ouiHelpRow(y, "A / B", "Trigger A / B");
#endif
  } else if (ouiIsSms()) {
#if !OUI_PICOCALC   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "MASTER SYSTEM  -  GAMEPAD");
    ouiHelpRow(y, "D-pad",  "D-pad");
    ouiHelpRow(y, "A / B",  "Button 1 / 2");
#endif
    ouiHelpHdr(y, "SMS  -  KEYBOARD");
    ouiHelpRow(y, "Arrows", "D-pad");
#if OUI_PICOCALC
    ouiHelpRow(y, "F4 / F5",  "Button 1 / 2");
    ouiHelpRow(y, "Space / X","Button 1 / 2");
    ouiHelpRow(y, "Ctrl-F3",  "PAUSE");
#else
    ouiHelpRow(y, "Z / X",  "Button 1 / 2");
    ouiHelpRow(y, "F11",    "PAUSE");
    ouiHelpRow(y, "F12",    "Reset");
#endif
  } else if (ouiIsColeco()) {
#if !OUI_PICOCALC   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "COLECO  -  GAMEPAD");
    ouiHelpRow(y, "D-pad",  "Stick");
    ouiHelpRow(y, "A / B",  "Left / right fire");
    ouiHelpRow(y, "Btn 3/4","Keypad * / 1");
#endif
    ouiHelpHdr(y, "COLECO  -  KEYBOARD");
    ouiHelpRow(y, "Arrows", "Stick");
#if OUI_PICOCALC
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
#if !OUI_PICOCALC   // no gamepad port on this board; see the note at the Apple section
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
#if !OUI_PICOCALC   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "PC-XT  -  GAMEPAD");
    ouiHelpRow(y, "D-pad",  "Arrow keys");
    ouiHelpRow(y, "A / B",  "Enter / Esc");
#endif
  } else {   // Apple II
    ouiHelpHdr(y, "APPLE  -  KEYBOARD");
    ouiHelpRow(y, "Keys",   "Type into Apple");
    ouiHelpRow(y, "Arrows", "Cursor / paddles");
#if OUI_PICOCALC
    ouiHelpRow(y, "F4 / F5", "Button 0 / 1");
    ouiHelpRow(y, "Ctrl-F3", "Reset the Apple");
#else
    ouiHelpRow(y, "Alt / AltGr", "Button 0 / 1");
    ouiHelpRow(y, "F11",    "Reset");
#endif
#if !OUI_PICOCALC
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
#if OUI_PICOCALC
  tft.drawString("Press any key to close", 160, OUI_SCR_H - 4, 1);
#else
  tft.drawString("Tap anywhere to close", 160, 236, 1);
#endif
}

// --- ROMS page: pick the SD-card file each system ROM loads from (romsel.cpp) ---
// Mode 1 lists the ROMs the running machine needs with the file each one will load; Enter (or a
// tap) opens an SD browser for that ROM (mode 2), whose first row puts the ROM back on its default
// name and whose second leaves it with no file at all (the system then reports it missing). A picked file is size-checked against what the loader accepts and saved to EEPROM at once;
// the loaders only read it at boot, so it takes effect on the next reboot.
#define OUI_RS_HDR    (OUI_TITLE_H + 2)
#define OUI_RS_LIST   (OUI_TITLE_H + 20)
#define OUI_RS_FOOT   (OUI_SCR_H - 32)
#define OUI_RS_ROWS   ((OUI_RS_FOOT - 2 - OUI_RS_LIST) / OUI_FB_ROWH)
// The snapshot page: the list on the left, the selected one's picture on the right, the buttons
// along the bottom.
#define OUI_SN_BTN_H  30
#define OUI_SN_BTN_Y  (OUI_SCR_H - OUI_SN_BTN_H - 4)
#define OUI_SN_LIST_W 134                                  // rows; the scroll buttons follow
#define OUI_SN_ROWS   ((OUI_SN_BTN_Y - 4 - OUI_RS_LIST) / OUI_FB_ROWH)
#define OUI_SN_TX     (320 - 4 - SNAP_THUMB_W)               // the thumbnail
#define OUI_SN_TY     (OUI_RS_LIST + 1)
#define OUI_SN_INFO   (OUI_SN_TY + SNAP_THUMB_H + 5)         // name + message under it
#define OUI_RS_FIXED  2           // mode 2 rows above the files: "use default", "no ROM"
#if OUI_PICOCALC
#define OUI_RS_LBLW   84          // label column (mode 1)
#else
#define OUI_RS_LBLW   64
#endif
static int      ouiRomSel = 0, ouiRomFirst = 0;  // highlighted row / first row shown
static int      ouiRomSlot = -1;                 // mode 2: the ROM being picked
static int      ouiRomTapped = -1;               // touch: row tapped once (a second tap picks it)
static char     ouiRomMsg[48];                   // footer status line ("" = show the key hints)
static bool     ouiRomMsgErr = false;
static uint32_t ouiRomRowSig[OUI_RS_ROWS];
static FileList ouiRomFiles;
static FileBrowser ouiRomBrowser = { "ROMS", &ouiRomFiles, nullptr, nullptr, 200, "/" };

// The slots shown in mode 1, in table order.
static int ouiRomSlots(int *out)
{
  int n = 0;
  for (int i = 0; i < ROMSEL_COUNT; i++) if (romSlotVisible(i)) out[n++] = i;
  return n;
}

static int ouiListRows() { return ouiSnapMode ? OUI_SN_ROWS : OUI_RS_ROWS; }

static int ouiRomCount()
{
  if (ouiGenMode) return ouiGenCount();
  if (ouiSnapMode) return (int)ouiRomFiles.size();
  if (ouiRomMode == 2) return OUI_RS_FIXED + (int)ouiRomFiles.size();
  int s[ROMSEL_COUNT];
  return ouiRomSlots(s);
}

static int ouiRomTextW(const char *s, int font)
{
#if OUI_PICOCALC || defined(BOARD_DESKTOP)
  return tft.textWidth(s, font);
#else
  return (int)strlen(s) * (font == 2 ? 8 : 6);   // GLCD font 1 is 6px a cell; font 2 averages ~8
#endif
}

// Fit `s` into `maxw` pixels, cutting from the LEFT (the end of a path is the part that matters).
static std::string ouiRomFit(const char *s, int maxw, int font)
{
  std::string t = s;
  if (ouiRomTextW(t.c_str(), font) <= maxw) return t;
  while (t.size() > 1 && ouiRomTextW(("..." + t).c_str(), font) > maxw) t.erase(0, 1);
  return "..." + t;
}

static void ouiRomSetMsg(const char *m, bool err) { snprintf(ouiRomMsg, sizeof(ouiRomMsg), "%s", m); ouiRomMsgErr = err; }

// Every page / folder change repaints the whole screen; ouiInvalidate() doesn't know these rows.
static void ouiRomRedraw()
{
  for (int i = 0; i < OUI_RS_ROWS; i++) ouiRomRowSig[i] = OUI_SIG_NONE;
  optionsUiFirstDraw = true; optionsUiDirty = true;
}

static void ouiOpenRoms()
{
  ouiRomMode = 1; ouiRomSel = 0; ouiRomFirst = 0; ouiRomSlot = -1; ouiRomTapped = -1;
  ouiRomMsg[0] = 0;
  ouiRomRedraw();
}

// Leave the ROMS page entirely and give the browser's list back to the heap.
static void ouiRomClose()
{
  ouiRomMode = 0; ouiRomSlot = -1;
  ouiRomFiles.release();
}

static void ouiRomKeepVisible()
{
  if (ouiRomSel < ouiRomFirst) ouiRomFirst = ouiRomSel;
  else if (ouiRomSel >= ouiRomFirst + ouiListRows()) ouiRomFirst = ouiRomSel - ouiListRows() + 1;
}

static void ouiRomMove(int d)
{
  int n = ouiRomCount();
  if (n <= 0) return;
  int s = ouiRomSel + d;
  if (s < 0) s = 0;
  if (s > n - 1) s = n - 1;
  ouiRomSel = s; ouiRomTapped = -1;
  ouiRomKeepVisible();
  optionsUiDirty = true;
}

static void ouiRomScroll(int d)
{
  const int rows = ouiListRows();
  int maxFirst = ouiRomCount() - rows;
  if (maxFirst < 0) maxFirst = 0;
  ouiRomFirst += d * (rows - 1);
  if (ouiRomFirst > maxFirst) ouiRomFirst = maxFirst;
  if (ouiRomFirst < 0) ouiRomFirst = 0;
  if (ouiRomSel < ouiRomFirst) ouiRomSel = ouiRomFirst;
  if (ouiRomSel >= ouiRomFirst + rows) ouiRomSel = ouiRomFirst + rows - 1;
  ouiRomTapped = -1;
  optionsUiDirty = true;
}

static void ouiDrawRoms();

// Mode 1 -> 2: browse from the folder of the file the ROM currently loads (a missing folder
// just lists "..", so the user can walk up from there).
static void ouiRomBrowse(int slot)
{
  ouiRomMode = 2; ouiRomSlot = slot; ouiRomSel = 0; ouiRomFirst = 0; ouiRomTapped = -1;
  ouiRomMsg[0] = 0;
  String cur = romPath(slot);
  int sl = cur.lastIndexOf('/');
  ouiRomBrowser.dir = (sl <= 0) ? String("/") : cur.substring(0, sl);
  ouiRomFiles.release();
  tft.fillScreen(OUI_BG); ouiInvalidate(); ouiRomRedraw(); optionsUiFirstDraw = false;
  ouiDrawRoms();                             // the scan's progress bar then sits on this page
  fbScan(ouiRomBrowser);
  for (int i = 0; i < (int)ouiRomFiles.size(); i++)   // preselect the current pick if it is here
    if (ouiRomFiles[i] == std::string(cur.c_str())) { ouiRomSel = i + OUI_RS_FIXED; break; }
  ouiRomKeepVisible();
  ouiRomRedraw();
}

// Back one level: browser -> ROM list -> settings page.
static void ouiRomBack()
{
  if (ouiRomMode == 2) {
    int s[ROMSEL_COUNT], n = ouiRomSlots(s);
    ouiRomMode = 1; ouiRomSel = 0;
    for (int i = 0; i < n; i++) if (s[i] == ouiRomSlot) ouiRomSel = i;
    ouiRomFirst = 0; ouiRomKeepVisible(); ouiRomTapped = -1; ouiRomMsg[0] = 0;
    ouiRomFiles.release();
    ouiRomRedraw();
    return;
  }
  ouiRomClose();
  ouiRomRedraw();
}

// Size of an SD file, -1 if it cannot be opened, -2 if the card stays busy. Timed lock, not
// busTake(): on the PicoCalc this runs in the render task (see fbScanBody).
static long ouiRomFileSize(const char *path)
{
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) return -2;
  File f = FSTYPE.open(path, FILE_READ);
  long n = f ? (long)f.size() : -1;
  if (f) f.close();
  if (gBusLock) xSemaphoreGive(gBusLock);
  return n;
}

static void ouiRomPick(const char *path)   // path == nullptr: back to the default name; ROMSEL_NONE: no ROM
{
  const RomSlotInfo &si = romSlotInfo(ouiRomSlot);
  char m[48];
  if (path && strcmp(path, ROMSEL_NONE) != 0) {
    if (strlen(path) >= ROMSEL_SLOT_LEN) { ouiRomSetMsg("Path too long (max 63 chars)", true); optionsUiDirty = true; return; }
    long n = ouiRomFileSize(path);
    if (n < 0) { ouiRomSetMsg(n == -2 ? "SD card busy - try again" : "Cannot open that file", true); optionsUiDirty = true; return; }
    if ((uint32_t)n < si.minSize || (uint32_t)n > si.maxSize) {
      if (si.minSize == si.maxSize) snprintf(m, sizeof(m), "Wrong size: %ld, need %lu", n, (unsigned long)si.minSize);
      else snprintf(m, sizeof(m), "Wrong size: %ld (%lu-%lu)", n, (unsigned long)si.minSize, (unsigned long)si.maxSize);
      ouiRomSetMsg(m, true); optionsUiDirty = true; return;
    }
  }
  const int slot = ouiRomSlot;
  ouiRomBack();                              // back to the list
  if (!romSetOverride(slot, path)) { ouiRomSetMsg("Could not save", true); return; }
  snprintf(m, sizeof(m), "%s saved - reboot to apply", si.label);
  ouiRomSetMsg(m, false);
}

static void ouiRomEnter()
{
  if (ouiRomMode == 1) {
    int s[ROMSEL_COUNT], n = ouiRomSlots(s);
    if (ouiRomSel < n) ouiRomBrowse(s[ouiRomSel]);
    return;
  }
  if (ouiRomSel == 0) { ouiRomPick(nullptr); return; }
  if (ouiRomSel == 1) { ouiRomPick(ROMSEL_NONE); return; }
  int i = ouiRomSel - OUI_RS_FIXED;
  if (i >= (int)ouiRomFiles.size()) return;
  const std::string e = ouiRomFiles[i];      // copy: fbEnter/fbUp rebuild the vector
  if (ouiIsDir(e)) {
    if (e == "..") fbUp(ouiRomBrowser); else fbEnter(ouiRomBrowser, e.c_str());
    ouiRomSel = 0; ouiRomFirst = 0; ouiRomTapped = -1; ouiRomMsg[0] = 0;
    ouiRomRedraw();
    return;
  }
  ouiRomPick(e.c_str());
}

static void ouiRomTap(int16_t x, int16_t y)
{
  if (y < OUI_TITLE_H && x >= 320 - OUI_TITLE_H) { ouiRomBack(); return; }   // X = back
  const int listH = OUI_RS_ROWS * OUI_FB_ROWH;
  if (y < OUI_RS_LIST || y >= OUI_RS_LIST + listH) return;
  if (x >= 302) { ouiRomScroll(y < OUI_RS_LIST + listH / 2 ? -1 : 1); return; }
  int idx = ouiRomFirst + (y - OUI_RS_LIST) / OUI_FB_ROWH;
  if (idx >= ouiRomCount()) return;
  // The ROM list and folders open on the first tap; a file (or "use default") is highlighted
  // first and taken by a second tap, so a stray tap cannot rewrite a ROM.
  bool dirRow = ouiRomMode == 2 && idx >= OUI_RS_FIXED && ouiIsDir(ouiRomFiles[idx - OUI_RS_FIXED]);
  ouiRomSel = idx;
  if (ouiRomMode == 1 || dirRow || ouiRomTapped == idx) { ouiRomTapped = -1; ouiRomEnter(); return; }
  ouiRomTapped = idx;
  optionsUiDirty = true;
}

// One row of a list page: an optional label column `a`, the text `b` in colour `bc`.
typedef void (*OuiListRow)(int idx, char *a, size_t na, char *b, size_t nb, uint16_t &bc);

// The list pages (ROMS, and the general settings over the system menu) share this layout: title bar
// with an X, a one-line header, the rows with touch scroll buttons, and a footer with the last
// result or the key hints. Rows come from ouiRomCount() / `row`; selection state is ouiRomSel etc.
static void ouiDrawList(const char *title, const char *hdr, const char *hint, OuiListRow row, int lblW)
{
  uint32_t tsig = ouiHash(title);
#if OUI_PICOCALC
  tsig = ouiHash("bat", tsig ^ uiBatterySig());
#endif
  if (ouiChanged(ouiSigTitle, tsig)) {
    tft.fillRect(0, 0, 320, OUI_TITLE_H, OUI_TITLE);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(OUI_TXT, OUI_TITLE);
    tft.drawString(title, 10, OUI_TITLE_H / 2, 2);
    int cw = OUI_TITLE_H, cx = 320 - cw;
#if OUI_PICOCALC
    uiDrawBattery(cx - 8, OUI_TITLE_H / 2, OUI_TITLE);
#endif
    tft.fillRect(cx, 0, cw, OUI_TITLE_H, OUI_RED);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(OUI_TXT, OUI_RED);
    tft.drawString("X", cx + cw / 2, OUI_TITLE_H / 2, 2);
  }

  if (ouiChanged(ouiSigHdr, ouiHash(hdr))) {
    tft.fillRect(0, OUI_RS_HDR, 320, OUI_RS_LIST - OUI_RS_HDR, OUI_BG);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(OUI_LBL, OUI_BG);
    tft.drawString(ouiRomFit(hdr, 306, 1).c_str(), 7, OUI_RS_HDR + (OUI_RS_LIST - OUI_RS_HDR) / 2 - 1, 1);
  }

  const int count = ouiRomCount();
  for (int r = 0; r < OUI_RS_ROWS; r++) {
    const int idx = ouiRomFirst + r, ry = OUI_RS_LIST + r * OUI_FB_ROWH;
    char a[24] = "", b[80] = "";
    uint16_t bc = OUI_TXT;
    if (idx < count) row(idx, a, sizeof(a), b, sizeof(b), bc);
    char rs[128];
    snprintf(rs, sizeof(rs), "%d|%d|%d|%s|%s|%u", idx < count, idx == ouiRomSel, idx == ouiRomTapped, a, b, (unsigned)bc);
    if (!ouiChanged(ouiRomRowSig[r], ouiHash(rs))) continue;
    const bool sel = idx < count && idx == ouiRomSel;
    uint16_t bg = sel ? OUI_SEL : OUI_CARD2;
    tft.fillRect(0, ry, 300, OUI_FB_ROWH, bg);
    if (idx >= count) continue;
    if (ouiRomTapped == idx) tft.fillRect(0, ry, 3, OUI_FB_ROWH, OUI_ON);
    tft.setTextDatum(ML_DATUM);
    int bx = 9;
    if (a[0]) {
      tft.setTextColor(OUI_TXT, bg);
      tft.drawString(a, 9, ry + OUI_FB_ROWH / 2, OUI_FB_FONT);
      bx = lblW;
    }
    tft.setTextColor(sel ? OUI_TXT : bc, bg);
    tft.drawString(ouiRomFit(b, 296 - bx, OUI_FB_FONT).c_str(), bx, ry + OUI_FB_ROWH / 2, OUI_FB_FONT);
  }
  if (!ouiScrollDrawn) {                   // touch scroll buttons beside the list
    const int half = OUI_RS_ROWS * OUI_FB_ROWH / 2;
    ouiSmallBtn(302, OUI_RS_LIST, 16, half - 1, "^", OUI_CARD);
    ouiSmallBtn(302, OUI_RS_LIST + half + 1, 16, half - 1, "v", OUI_CARD);
    ouiScrollDrawn = true;
  }

  // Footer: the last result, else the keys.
  const char *foot = ouiRomMsg[0] ? ouiRomMsg : hint;
  uint16_t fc = ouiRomMsg[0] ? (ouiRomMsgErr ? OUI_RED : OUI_ON) : OUI_LBL;
  char fs[64];
  snprintf(fs, sizeof(fs), "%s|%u", foot, (unsigned)fc);
  if (ouiChanged(ouiSigAct, ouiHash(fs))) {
    tft.fillRect(0, OUI_RS_FOOT, 320, OUI_SCR_H - OUI_RS_FOOT, OUI_BG);
    tft.fillRoundRect(4, OUI_RS_FOOT + 2, 312, 28, 6, OUI_CARD2);
    tft.drawRoundRect(4, OUI_RS_FOOT + 2, 312, 28, 6, OUI_BORDER);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(fc, OUI_CARD2);
    tft.drawString(foot, 160, OUI_RS_FOOT + 16, 1);
  }
}

static void ouiRomRow(int idx, char *a, size_t na, char *b, size_t nb, uint16_t &bc)
{
  if (ouiRomMode == 1) {
    int slots[ROMSEL_COUNT], ns = ouiRomSlots(slots);
    if (idx < ns) {
      const char *o = romOverride(slots[idx]);
      snprintf(a, na, "%s", romSlotInfo(slots[idx]).label);
      if (o && !*o) { snprintf(b, nb, "(none)"); bc = OUI_RED; }
      else if (o) { snprintf(b, nb, "%s", o); bc = OUI_ON; }
      else   { snprintf(b, nb, "%s (default)", romDefaultPath(slots[idx])); bc = OUI_LBL; }
    }
  } else if (idx == 0) {
    const char *d = romDefaultPath(ouiRomSlot), *base = strrchr(d, '/');
    snprintf(b, nb, "< use default: %s >", base ? base + 1 : d);
    bc = OUI_REBOOT;
  } else if (idx == 1) {
    snprintf(b, nb, "< no ROM >");
    bc = OUI_REBOOT;
  } else {
    const std::string &e = ouiRomFiles[idx - OUI_RS_FIXED];
    snprintf(b, nb, "%s", ouiDisplayName(e).c_str());
    bc = ouiIsDir(e) ? tft.color565(90, 200, 255)
       : (e == std::string(romPath(ouiRomSlot))) ? OUI_ON : OUI_TXT;
  }
}

static void ouiDrawRoms()
{
  char hdr[96];
  if (ouiRomMode == 1) snprintf(hdr, sizeof(hdr), "File each ROM loads at boot");
  else snprintf(hdr, sizeof(hdr), "%s: %s", romSlotInfo(ouiRomSlot).label, ouiRomBrowser.dir.c_str());
  ouiDrawList(ouiRomMode == 1 ? "SYSTEM ROMS" : "PICK ROM FILE", hdr,
              ouiRomMode == 1 ? "Enter/tap: choose file    Esc/X: back"
                              : "Enter/2nd tap: use file    Esc/X: back",
              ouiRomRow, OUI_RS_LBLW);
}

// --- General settings: opened with Ctrl-F1 (F10 on a USB keyboard, the SETUP button on a touch
// panel) from the system menu, so they belong to the device rather than to one system. ---
//   mode 1  the list: ROM folder, apps folder, snapshot folder, reset settings, help
//   mode 2  a folder picker for one of the three folders (folders only)
//   mode 3  "erase everything?" confirmation
//   mode 4  the general help page
// The list pages' state (ouiRomSel, ouiRomFiles, the footer message...) is shared with the ROMS
// page, which cannot be open at the same time.
#define OUI_GEN_ROWS 5
#define OUI_GEN_RESET 3                       // mode 1 rows after the three folders
#define OUI_GEN_HELP  4
static int ouiGenTarget = 0;                  // mode 2: 0 = ROM folder, 1 = apps folder, 2 = snapshot folder
static bool ouiGenNoFiles(const std::string &) { return false; }   // the picker lists folders only
static FileBrowser ouiGenBrowser = { "FOLDER", &ouiRomFiles, ouiGenNoFiles, nullptr, 200, "/" };

static int ouiGenCount()
{
  switch (ouiGenMode) {
    case 1: return OUI_GEN_ROWS;
    case 2: return 2 + (int)ouiRomFiles.size();   // row 0 = this folder, row 1 = the default
    case 3: return 2;
    default: return 0;
  }
}

static const char *ouiGenCurrent(int target) { return target == 2 ? snapBaseDir() : target ? appsBaseDir() : romBaseDir(); }
static const char *ouiGenDefault(int target) { return target == 2 ? SNAP_DEFAULT_DIR : target ? "/" : "/roms"; }
static const char *ouiGenTitle(int target)   { return target == 2 ? "SNAPSHOT FOLDER" : target ? "APPS FOLDER" : "ROM FOLDER"; }

static void ouiGenRow(int idx, char *a, size_t na, char *b, size_t nb, uint16_t &bc)
{
  if (ouiGenMode == 1) {
    switch (idx) {
      case 0: snprintf(a, na, "ROM FOLDER");  snprintf(b, nb, "%s", romBaseDir());  bc = OUI_ON; break;
      case 1: snprintf(a, na, "APPS FOLDER"); snprintf(b, nb, "%s", appsBaseDir()); bc = OUI_ON; break;
      case 2: snprintf(a, na, "SNAPSHOTS");   snprintf(b, nb, "%s", snapBaseDir()); bc = OUI_ON; break;
      case OUI_GEN_RESET: snprintf(a, na, "RESET"); snprintf(b, nb, "Erase all saved settings"); bc = OUI_REBOOT; break;
      case OUI_GEN_HELP:  snprintf(a, na, "HELP");  snprintf(b, nb, "Keys, folders, ROMs"); bc = OUI_LBL; break;
    }
  } else if (ouiGenMode == 2) {
    if (idx == 0)      { snprintf(b, nb, "< use this folder: %s >", ouiGenBrowser.dir.c_str()); bc = OUI_ON; }
    else if (idx == 1) { snprintf(b, nb, "< use default: %s >", ouiGenDefault(ouiGenTarget)); bc = OUI_REBOOT; }
    else { snprintf(b, nb, "%s", ouiDisplayName(ouiRomFiles[idx - 2]).c_str()); bc = tft.color565(90, 200, 255); }
  } else if (ouiGenMode == 3) {
    if (idx == 0) { snprintf(b, nb, "No - keep my settings"); bc = OUI_TXT; }
    else          { snprintf(b, nb, "Yes - erase everything and reboot"); bc = OUI_RED; }
  }
}

static void ouiGenHelpPage()
{
  tft.fillScreen(OUI_BG);
  tft.fillRect(0, 0, 320, OUI_TITLE_H, OUI_TITLE);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(OUI_TXT, OUI_TITLE);
  tft.drawString("HELP  /  GENERAL", 10, OUI_TITLE_H / 2, 2);
  int cw = OUI_TITLE_H, cx = 320 - cw;
  tft.fillRect(cx, 0, cw, OUI_TITLE_H, OUI_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_TXT, OUI_RED);
  tft.drawString("X", cx + cw / 2, OUI_TITLE_H / 2, 2);

  int y = OUI_TITLE_H + 6;
#if OUI_PICOCALC
  ouiHelpHdr(y, "SYSTEM MENU  (Ctrl-Shift-F1)");
  ouiHelpRow(y, "Arrows / Enter", "Pick / start a system");
  ouiHelpRow(y, "Esc",            "Back to the running one");
  ouiHelpRow(y, "Ctrl-F1",        "These general settings");
  ouiHelpHdr(y, "WHILE A SYSTEM RUNS");
  ouiHelpRow(y, "Ctrl-F1",        "Its settings: files, ROMS");
  ouiHelpRow(y, "Ctrl-F3",        "Reset the machine");
  ouiHelpRow(y, "Ctrl-Shift-F3",  "Hard reset (same system)");
  ouiHelpRow(y, "Ctrl-Shift-Up",  "UF2 Loader menu");
#else
  ouiHelpHdr(y, "SYSTEM MENU");
  ouiHelpRow(y, "Tap a system",   "Start it");
  ouiHelpRow(y, "SETUP / F10",    "These general settings");
  ouiHelpHdr(y, "WHILE A SYSTEM RUNS");
  ouiHelpRow(y, "F10 / SEL+START","Its settings: files, ROMS");
  ouiHelpRow(y, "F11",            "Reset the machine");
#endif
  ouiHelpHdr(y, "FOLDERS ON THE SD CARD");
  ouiHelpRow(y, "ROM folder",     "System ROMs (default /roms)");
  ouiHelpRow(y, "",               "Names: README, ROM sources");
  ouiHelpRow(y, "Apps folder",    "Where file lists start (/)");
  ouiHelpRow(y, "Snapshots",      "Save states, one folder/system");
  ouiHelpRow(y, "ROMS page",      "Pick any other ROM file");
  ouiHelpRow(y, "Reset",          "Erase settings, then reboot");

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(OUI_LBL, OUI_BG);
#if OUI_PICOCALC
  tft.drawString("Press any key to close", 160, OUI_SCR_H - 4, 1);
#else
  tft.drawString("Tap anywhere to close", 160, 236, 1);
#endif
}

static void ouiDrawGeneral()
{
  if (ouiGenMode == 4) { ouiGenHelpPage(); return; }
  char hdr[96];
  const char *title = "GENERAL SETTINGS", *hint = "Enter/tap: open    Esc/X: back to systems";
  if (ouiGenMode == 1) snprintf(hdr, sizeof(hdr), "Saved on this device, for every system");
  else if (ouiGenMode == 2) {
    title = ouiGenTitle(ouiGenTarget);
    hint  = "Enter/2nd tap: choose    Esc/X: back";
    snprintf(hdr, sizeof(hdr), "Now: %s    Browsing: %s", ouiGenCurrent(ouiGenTarget), ouiGenBrowser.dir.c_str());
  } else {
    title = "RESET SETTINGS";
    hint  = "Enter/2nd tap: choose    Esc/X: back";
    snprintf(hdr, sizeof(hdr), "Every setting, disk, ROM pick and folder");
  }
  ouiDrawList(title, hdr, hint, ouiGenRow, 130);   // label column fits "APPS FOLDER" in every font
}

static void ouiGenGoto(uint8_t mode, int sel)
{
  if (mode != 2) ouiRomFiles.release();
  ouiGenMode = mode; ouiRomSel = sel; ouiRomFirst = 0; ouiRomTapped = -1;
  ouiRomKeepVisible();
  ouiRomRedraw();
}

// Mode 1 -> 2: browse from the folder currently set (a missing one lists only "..").
static void ouiGenBrowse(int target)
{
  ouiGenTarget = target; ouiRomMsg[0] = 0;
  ouiGenBrowser.dir = ouiGenCurrent(target);
  ouiRomFiles.release();
  ouiGenGoto(2, 0);
  tft.fillScreen(OUI_BG); ouiInvalidate(); optionsUiFirstDraw = false;
  ouiDrawGeneral();                          // the scan's progress bar then sits on this page
  fbScan(ouiGenBrowser);
  ouiRomRedraw();
}

static void ouiGenPick(const char *dir)      // dir == nullptr: back to the default
{
  const int t = ouiGenTarget;
  if (dir && strlen(dir) >= ROMSEL_SLOT_LEN) { ouiRomSetMsg("Path too long (max 63 chars)", true); optionsUiDirty = true; return; }
  const String d = dir ? dir : "";           // dir may point into the browser, which ouiGenGoto frees
  ouiGenGoto(1, t);
  const char *p = dir ? d.c_str() : nullptr;
  if (!(t == 2 ? setSnapBaseDir(p) : t ? setAppsBaseDir(p) : setRomBaseDir(p))) { ouiRomSetMsg("Could not save", true); return; }
  ouiRomSetMsg(t == 2 ? "Snapshot folder saved" : t ? "Apps folder saved - used from next boot" : "ROM folder saved - used from next boot", false);
}

static void ouiGenEnter()
{
  if (ouiGenMode == 4) { ouiGenGoto(1, OUI_GEN_HELP); return; }
  if (ouiGenMode == 1) {
    ouiRomMsg[0] = 0;
    if (ouiRomSel <= 2)                  ouiGenBrowse(ouiRomSel);
    else if (ouiRomSel == OUI_GEN_RESET) ouiGenGoto(3, 0);             // "No" preselected
    else                                 ouiGenGoto(4, 0);
    return;
  }
  if (ouiGenMode == 3) {
    if (ouiRomSel == 0) { ouiGenGoto(1, OUI_GEN_RESET); return; }
    tft.fillScreen(OUI_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(OUI_TXT, OUI_BG);
    tft.drawString("Erasing settings...", 160, OUI_SCR_H / 2, 2);
    displayFlush();
    eepromFactoryReset();
    requestSplashOnNextBoot();                // come back up in the system menu
    delay(300);
    ESP.restart();
    return;
  }
  // mode 2
  if (ouiRomSel == 0) { const String d = ouiGenBrowser.dir; ouiGenPick(d.c_str()); return; }
  if (ouiRomSel == 1) { ouiGenPick(nullptr); return; }
  const int i = ouiRomSel - 2;
  if (i >= (int)ouiRomFiles.size()) return;
  const std::string e = ouiRomFiles[i];      // copy: fbEnter/fbUp rebuild the vector
  if (e == "..") fbUp(ouiGenBrowser); else fbEnter(ouiGenBrowser, e.c_str());
  ouiRomSel = 0; ouiRomFirst = 0; ouiRomTapped = -1;
  ouiRomRedraw();
}

// Esc / X: one level back. False at the top: the caller closes the page (back to the systems).
static bool ouiGenBack()
{
  switch (ouiGenMode) {
    case 2: ouiGenGoto(1, ouiGenTarget); return true;
    case 3: ouiGenGoto(1, OUI_GEN_RESET); return true;
    case 4: ouiGenGoto(1, OUI_GEN_HELP); return true;
    default: return false;
  }
}

static void ouiGenTap(int16_t x, int16_t y)
{
  if (ouiGenMode == 4) { ouiGenBack(); return; }                  // help: any tap closes it
  if (y < OUI_TITLE_H && x >= 320 - OUI_TITLE_H) { if (!ouiGenBack()) optionsUiCloseGeneral(); return; }
  const int listH = OUI_RS_ROWS * OUI_FB_ROWH;
  if (y < OUI_RS_LIST || y >= OUI_RS_LIST + listH) return;
  if (x >= 302) { ouiRomScroll(y < OUI_RS_LIST + listH / 2 ? -1 : 1); return; }
  int idx = ouiRomFirst + (y - OUI_RS_LIST) / OUI_FB_ROWH;
  if (idx >= ouiRomCount()) return;
  // The list and folders open on the first tap; a choice that saves (or erases) is highlighted
  // first and taken by a second tap.
  bool now = ouiGenMode == 1 || (ouiGenMode == 2 && idx >= 2) || (ouiGenMode == 3 && idx == 0);
  ouiRomSel = idx;
  if (now || ouiRomTapped == idx) { ouiRomTapped = -1; ouiGenEnter(); return; }
  ouiRomTapped = idx;
  optionsUiDirty = true;
}

// Called instead of the per-system settings while the system menu is up (showHideOptionsWindow).
// OptionsWindow is set so the keyboards route their keys here, but no core is paused or resumed:
// the render loop keeps the splash branch, which draws this page while it is open.
void optionsUiOpenGeneral()
{
  if (OptionsWindow) return;
  OptionsWindow = true;
#if OUI_PICOCALC
  tft.setFullPanel(true);
#endif
  ouiHelpOpen = false; ouiRomClose();
  ouiRomMsg[0] = 0;
  ouiGenGoto(1, 0);
  optionsUiFirstDraw   = true;
  optionsUiDirty       = true;
  optionsUiWaitRelease = true;               // don't treat the opening tap as a click
  optionsUiPrevDown    = true;
}

void optionsUiCloseGeneral()
{
  if (!ouiGenMode) return;
  ouiGenMode = 0;
  ouiRomFiles.release();
  OptionsWindow = false;
  splashRepaint();
}

bool optionsUiGeneralOpen() { return ouiGenMode != 0; }

// --- Snapshot page: Ctrl-Shift-F5 (snapshotWindowToggle) over the paused machine ---
// The running system's save states, newest first (snapshot.cpp), each with the picture it was
// taken on. Picking works like the file list: a tap (or Up/Down) only selects -- its picture shows
// on the right -- and the buttons below act: SAVE always saves a NEW one (S), LOAD loads the
// selected one and resumes (L), DELETE twice deletes it (D twice). Esc / X resumes.
// Shares the list pages' state (ouiRomFiles holds the paths, ouiRomSel...) with ROMS / general.
static int ouiSnapDelArm = -1;               // row D was pressed on once: a second D deletes it
static uint32_t ouiSnapResumeAt = 0;         // after a load: millis() at which the page closes by itself
static uint32_t ouiSnapRestartAt = 0;        // its settings were restored: millis() at which to reboot

static int ouiSnapNum(const std::string &e)
{
  const size_t sl = e.find_last_of('/');
  return atoi(e.c_str() + (sl == std::string::npos ? 0 : sl + 1));
}

static void ouiSnapRescan(int sel)
{
  snapList(ouiRomFiles);
  ouiRomSel = sel; ouiRomFirst = 0; ouiRomTapped = -1; ouiSnapDelArm = -1;
  if (ouiRomSel > (int)ouiRomFiles.size() - 1) ouiRomSel = (int)ouiRomFiles.size() - 1;
  if (ouiRomSel < 0) ouiRomSel = 0;
  ouiRomKeepVisible();
  ouiRomRedraw();
}

// "/emu8-snapshot/c64/0007-GIANA.snp" -> num = "0007", name = "GIANA"
static void ouiSnapName(int idx, char *num, size_t nn, char *name, size_t nm)
{
  const std::string &e = ouiRomFiles[idx];
  const size_t sl = e.find_last_of('/');
  std::string n = e.substr(sl == std::string::npos ? 0 : sl + 1);
  if (n.size() > 4) n.erase(n.size() - 4);          // ".snp"
  const size_t dash = n.find('-');
  snprintf(num, nn, "%s", n.substr(0, dash).c_str());
  snprintf(name, nm, "%s", dash == std::string::npos ? snapSystemName() : n.c_str() + dash + 1);
}

// Up to `maxLines` lines of `s`, broken at spaces to fit `w` pixels in font 1, from (x, y).
static void ouiSnapWrap(const char *s, int x, int y, int w, int maxLines, uint16_t fg)
{
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(fg, OUI_BG);
  std::string rest = s;
  for (int l = 0; l < maxLines && !rest.empty(); l++) {
    size_t cut = rest.size();
    while (cut > 1 && ouiRomTextW(rest.substr(0, cut).c_str(), 1) > w) {
      size_t sp = rest.rfind(' ', cut - 1);
      cut = (sp == std::string::npos || sp == 0) ? cut - 1 : sp;
    }
    tft.drawString(rest.substr(0, cut).c_str(), x, y + l * 12, 1);
    rest.erase(0, cut);
    while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
  }
}

static void ouiSnapBtn(int x, int w, const char *label, uint16_t face, uint16_t txt)
{
  tft.fillRoundRect(x, OUI_SN_BTN_Y, w, OUI_SN_BTN_H, 6, face);
  tft.drawRoundRect(x, OUI_SN_BTN_Y, w, OUI_SN_BTN_H, 6, OUI_BORDER);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(txt, face);
  tft.drawString(label, x + w / 2, OUI_SN_BTN_Y + OUI_SN_BTN_H / 2, 2);
}

static void ouiDrawSnap()
{
  // Title bar (the list pages' one).
  uint32_t tsig = ouiHash("SNAPSHOTS");
#if OUI_PICOCALC
  tsig = ouiHash("bat", tsig ^ uiBatterySig());
#endif
  if (ouiChanged(ouiSigTitle, tsig)) {
    tft.fillRect(0, 0, 320, OUI_TITLE_H, OUI_TITLE);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(OUI_TXT, OUI_TITLE);
    tft.drawString("SNAPSHOTS", 10, OUI_TITLE_H / 2, 2);
    int cw = OUI_TITLE_H, cx = 320 - cw;
#if OUI_PICOCALC
    uiDrawBattery(cx - 8, OUI_TITLE_H / 2, OUI_TITLE);
#endif
    tft.fillRect(cx, 0, cw, OUI_TITLE_H, OUI_RED);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(OUI_TXT, OUI_RED);
    tft.drawString("X", cx + cw / 2, OUI_TITLE_H / 2, 2);
  }

  const int count = (int)ouiRomFiles.size();
  char hdr[96];
  const String dir = snapDir();
  if (!count) snprintf(hdr, sizeof(hdr), "None yet - SAVE makes one in %s", dir.c_str());
  else snprintf(hdr, sizeof(hdr), "%d in %s, newest first", count, dir.c_str());
  if (ouiChanged(ouiSigHdr, ouiHash(hdr))) {
    tft.fillRect(0, OUI_RS_HDR, 320, OUI_RS_LIST - OUI_RS_HDR, OUI_BG);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(OUI_LBL, OUI_BG);
    tft.drawString(ouiRomFit(hdr, 306, 1).c_str(), 7, OUI_RS_HDR + (OUI_RS_LIST - OUI_RS_HDR) / 2 - 1, 1);
  }

  // The list: "0007 GIANA", the newest in green, the one armed for delete in red.
  for (int r = 0; r < OUI_SN_ROWS; r++) {
    const int idx = ouiRomFirst + r, ry = OUI_RS_LIST + r * OUI_FB_ROWH;
    char num[16] = "", name[48] = "", rs[96];
    if (idx < count) ouiSnapName(idx, num, sizeof(num), name, sizeof(name));
    const bool sel = idx < count && idx == ouiRomSel;
    snprintf(rs, sizeof(rs), "%d|%d|%d|%s|%s", idx < count, sel, idx == ouiSnapDelArm, num, name);
    if (!ouiChanged(ouiRomRowSig[r], ouiHash(rs))) continue;
    const uint16_t bg = sel ? OUI_SEL : OUI_CARD2;
    tft.fillRect(0, ry, OUI_SN_LIST_W, OUI_FB_ROWH, bg);
    if (idx >= count) continue;
    const uint16_t fg = idx == ouiSnapDelArm ? OUI_RED : sel ? OUI_TXT : idx == 0 ? OUI_ON : OUI_TXT;
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(sel ? OUI_TXT : OUI_LBL, bg);
    tft.drawString(num, 4, ry + OUI_FB_ROWH / 2, 1);
    const int nx = 4 + ouiRomTextW(num, 1) + 5;
    tft.setTextColor(fg, bg);
    tft.drawString(ouiRomFit(name, OUI_SN_LIST_W - 2 - nx, OUI_FB_FONT).c_str(), nx, ry + OUI_FB_ROWH / 2, OUI_FB_FONT);
  }
  if (!ouiScrollDrawn) {                   // touch scroll buttons beside the list
    const int half = OUI_SN_ROWS * OUI_FB_ROWH / 2;
    ouiSmallBtn(OUI_SN_LIST_W + 2, OUI_RS_LIST, 16, half - 1, "^", OUI_CARD);
    ouiSmallBtn(OUI_SN_LIST_W + 2, OUI_RS_LIST + half + 1, 16, half - 1, "v", OUI_CARD);
    ouiScrollDrawn = true;
  }

  // The selected one's picture.
  // (FileList::operator[] builds a temporary: keep the string, a c_str() of it would dangle)
  const std::string selStr = ouiRomSel >= 0 && ouiRomSel < count ? ouiRomFiles[ouiRomSel] : std::string();
  const char *selPath = selStr.c_str();
  if (ouiChanged(ouiSigSnapThumb, ouiHash(selPath, 77u))) {
    tft.fillRect(OUI_SN_TX - 1, OUI_SN_TY - 1, SNAP_THUMB_W + 2, 1, OUI_BORDER);
    tft.fillRect(OUI_SN_TX - 1, OUI_SN_TY + SNAP_THUMB_H, SNAP_THUMB_W + 2, 1, OUI_BORDER);
    tft.fillRect(OUI_SN_TX - 1, OUI_SN_TY, 1, SNAP_THUMB_H, OUI_BORDER);
    tft.fillRect(OUI_SN_TX + SNAP_THUMB_W, OUI_SN_TY, 1, SNAP_THUMB_H, OUI_BORDER);
    static uint32_t retrySig = OUI_SIG_NONE;
    static uint8_t retries = 0;
    const bool drawn = *selPath && snapThumbDraw(selPath, OUI_SN_TX, OUI_SN_TY);
    if (!drawn && *selPath) {             // a failed read (SD busy right after a save...) tries again
      if (retrySig != ouiSigSnapThumb) { retrySig = ouiSigSnapThumb; retries = 0; }
      if (retries < 2) { retries++; ouiSigSnapThumb = OUI_SIG_NONE; optionsUiDirty = true; }
    }
    if (!drawn) {
      tft.fillRect(OUI_SN_TX, OUI_SN_TY, SNAP_THUMB_W, SNAP_THUMB_H, OUI_CARD2);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(OUI_LBL, OUI_CARD2);
      tft.drawString(*selPath ? "No picture" : "No snapshots", OUI_SN_TX + SNAP_THUMB_W / 2, OUI_SN_TY + SNAP_THUMB_H / 2, 2);
    }
  }

  // Under it: the selected name, then the last result, the delete question, or the keys.
#if OUI_PICOCALC
  const char *hint = "S: save   L: load   D: delete   Esc: back";
#else
  const char *hint = "Tap a snapshot to see it, then LOAD or DELETE it. SAVE adds a new one.";
#endif
  char selName[64] = "";
  if (*selPath) {
    char num[16], name[48];
    ouiSnapName(ouiRomSel, num, sizeof(num), name, sizeof(name));
    snprintf(selName, sizeof(selName), "%s %s", num, name);
  }
  const bool armed = ouiSnapDelArm >= 0 && ouiSnapDelArm == ouiRomSel;
  const char *msg = ouiRomMsg[0] ? ouiRomMsg : armed ? "Press DELETE (D) again to delete it" : hint;
  const uint16_t mc = ouiRomMsg[0] ? (ouiRomMsgErr ? OUI_RED : OUI_ON) : armed ? OUI_RED : OUI_LBL;
  char is[192];
  snprintf(is, sizeof(is), "%s|%s|%u", selName, msg, (unsigned)mc);
  if (ouiChanged(ouiSigSnapInfo, ouiHash(is))) {
    const int ix = OUI_SN_TX - 1, iw = SNAP_THUMB_W + 2;
    tft.fillRect(ix, OUI_SN_INFO, iw, OUI_SN_BTN_Y - 2 - OUI_SN_INFO, OUI_BG);
    int y = OUI_SN_INFO;
    if (selName[0]) {
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(OUI_TXT, OUI_BG);
      tft.drawString(ouiRomFit(selName, iw, 1).c_str(), ix, y, 1);
      y += 14;
    }
    ouiSnapWrap(msg, ix, y, iw, (OUI_SN_BTN_Y - 2 - y) / 12, mc);
  }

  // The buttons.
  const bool busy = ouiSnapResumeAt || ouiSnapRestartAt;
  const uint32_t bsig = 16u + (count ? 1u : 0u) + (armed ? 2u : 0u) + (busy ? 4u : 0u);
  if (ouiChanged(ouiSigSnapBtns, bsig)) {
    const uint16_t off = OUI_CARD, offTxt = OUI_LBL;
    const bool none = busy || !count;
    ouiSnapBtn(4,   102, "SAVE (S)",   busy ? off : tft.color565(40, 130, 75), busy ? offTxt : OUI_TXT);
    ouiSnapBtn(109, 102, "LOAD (L)",   none ? off : OUI_SEL, none ? offTxt : OUI_TXT);
    ouiSnapBtn(214, 102, armed ? "CONFIRM (D)" : "DELETE (D)",
               none ? off : armed ? OUI_RED : tft.color565(120, 58, 52), none ? offTxt : OUI_TXT);
  }
}

// S: save the machine as the next number, then show it at the top of the list.
static void ouiSnapSave()
{
  const std::string media = ouiSel();
  const int newest = ouiRomFiles.empty() ? 0 : ouiSnapNum(ouiRomFiles[0]);
  String saved;
  const char *err = nullptr;
  if (!snapSave(newest, media.c_str(), saved, err)) { ouiSnapDelArm = -1; ouiRomSetMsg(err, true); ouiRomRedraw(); return; }
  ouiSnapRescan(0);
  const int sl = saved.lastIndexOf('/');
  char m[64];
  snprintf(m, sizeof(m), "Saved %s", saved.c_str() + sl + 1);
  ouiRomSetMsg(m, false);
}

// L: load the highlighted snapshot and resume the machine from it.
static void ouiSnapLoadPath(const std::string &path);

static void ouiSnapLoad()
{
  ouiSnapDelArm = -1;
  if (ouiRomSel < 0 || ouiRomSel >= (int)ouiRomFiles.size()) { ouiRomSetMsg("Nothing to load - Enter saves one", true); ouiRomRedraw(); return; }
  ouiSnapLoadPath(ouiRomFiles[ouiRomSel]);
}

static void ouiSnapLoadPath(const std::string &path)
{
  const char *err = nullptr;
  bool restart = false;
  if (!snapLoad(path.c_str(), err, &restart)) {
    if (restart) {
      // Its settings (disk, model...) are written: say so, then reboot from optionsUiRender().
      ouiRomSetMsg("Restoring its disk & settings - restarting", false);
      ouiRomRedraw();
      ouiSnapRestartAt = millis() + 400;
      if (!ouiSnapRestartAt) ouiSnapRestartAt = 1;
      return;
    }
    ouiRomSetMsg(err, true); ouiRomRedraw(); return;
  }
  // Say so before resuming: a load taken right after its save resumes on the very same picture,
  // and a page that just vanished looked like nothing had happened. optionsUiRender() closes it.
  const size_t sl = path.find_last_of('/');
  char m[64];
  snprintf(m, sizeof(m), "Loaded %s", path.c_str() + (sl == std::string::npos ? 0 : sl + 1));
  ouiRomSetMsg(m, false);
  ouiRomRedraw();
  ouiSnapResumeAt = millis() + 1500;
  if (!ouiSnapResumeAt) ouiSnapResumeAt = 1;
}

static void ouiSnapDelete()
{
  if (ouiRomSel < 0 || ouiRomSel >= (int)ouiRomFiles.size()) return;
  if (ouiSnapDelArm != ouiRomSel) { ouiSnapDelArm = ouiRomSel; ouiRomMsg[0] = 0; optionsUiDirty = true; return; }
  const std::string path = ouiRomFiles[ouiRomSel];
  const bool ok = snapDelete(path.c_str());
  ouiSnapRescan(ouiRomSel);
  ouiRomSetMsg(ok ? "Snapshot deleted" : "Could not delete it", !ok);
}

static void ouiSnapMove(int d)
{
  ouiSnapDelArm = -1; ouiRomMsg[0] = 0;
  ouiRomMove(d);
}

static void ouiSnapTap(int16_t x, int16_t y)
{
  if (y < OUI_TITLE_H && x >= 320 - OUI_TITLE_H) { showHideOptionsWindow(); return; }   // X = resume
  if (ouiSnapResumeAt || ouiSnapRestartAt) return;   // a load is finishing
  if (y >= OUI_SN_BTN_Y && y < OUI_SN_BTN_Y + OUI_SN_BTN_H) {                           // the buttons
    if (x < 107) ouiSnapSave(); else if (x < 212) ouiSnapLoad(); else ouiSnapDelete();
    return;
  }
  const int listH = OUI_SN_ROWS * OUI_FB_ROWH;
  if (y < OUI_RS_LIST || y >= OUI_RS_LIST + listH || x >= OUI_SN_LIST_W + 20) return;
  if (x >= OUI_SN_LIST_W) { ouiRomScroll(y < OUI_RS_LIST + listH / 2 ? -1 : 1); return; }
  const int idx = ouiRomFirst + (y - OUI_RS_LIST) / OUI_FB_ROWH;
  if (idx >= ouiRomCount() || idx == ouiRomSel) return;
  ouiRomSel = idx; ouiSnapDelArm = -1; ouiRomMsg[0] = 0;   // select only: LOAD / DELETE act on it
  optionsUiDirty = true;
}

// Open the page. The window is already up and the machine paused (snapshotWindowToggle).
void optionsUiOpenSnap()
{
#if OUI_PICOCALC
  tft.setFullPanel(true);                    // before the scan: its progress bar draws on the page
#endif
  ouiHelpOpen = false; ouiRomClose(); ouiEditing = false;
  ouiSnapMode = 1; ouiRomMsg[0] = 0;
  tft.fillScreen(OUI_BG); ouiInvalidate(); ouiRomRedraw(); optionsUiFirstDraw = false;
  ouiSnapRescan(0);
  optionsUiFirstDraw   = true;
  optionsUiDirty       = true;
  optionsUiWaitRelease = true;               // don't treat the opening tap as a click
  optionsUiPrevDown    = true;
}

bool optionsUiSnapOpen() { return ouiSnapMode != 0; }
bool optionsUiSnapRestarting() { return ouiSnapRestartAt != 0; }

// showHideOptionsWindow() closed the window: give the list pages' memory back to the running
// machine (the C64 on the RP2040 has none to spare).
void optionsUiClosed()
{
  ouiSnapMode = 0; ouiSnapDelArm = -1; ouiSnapResumeAt = 0;
  ouiRomClose();
  snapThumbForget();                         // the machine runs on: the picture taken is stale
}

void optionsUiRender()
{
#if OUI_PICOCALC
  // Normally already on from optionsUiOpen(); if anything switched it off, repaint all of it.
  if (tft.setFullPanel(true)) { optionsUiFirstDraw = true; optionsUiDirty = true; }
#endif
  // A load restored its settings and the message has been drawn: reboot into them.
  if (ouiSnapRestartAt && !optionsUiDirty && (int32_t)(millis() - ouiSnapRestartAt) >= 0) { snapRestart(); return; }
  // A snapshot was loaded and its "Loaded ..." line has been on screen long enough: resume.
  if (ouiSnapResumeAt && !optionsUiDirty && (int32_t)(millis() - ouiSnapResumeAt) >= 0) {
    showHideOptionsWindow();                 // close + resume (optionsUiClosed clears the timer)
    return;
  }
  if (!optionsUiDirty) return;
  if (optionsUiFirstDraw) { tft.fillScreen(OUI_BG); ouiInvalidate(); optionsUiFirstDraw = false; }
  if (ouiGenMode)  { ouiDrawGeneral(); optionsUiDirty = false; return; }
  if (ouiSnapMode) { optionsUiDirty = false; ouiDrawSnap(); return; }   // (a thumbnail retry re-arms it)
  if (ouiHelpOpen) { ouiDrawHelp(); optionsUiDirty = false; return; }
  if (ouiRomMode)  { ouiDrawRoms(); optionsUiDirty = false; return; }
  ouiDrawTitle();
  ouiDrawToggles();
  ouiDrawRomsButton();
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
static void ouiToggleScreenFill() { screenFill = (uint8_t)((screenFill + 1) % SCREEN_MODES); optionsUiDirty = true; }
#endif

// Any grid slot (0..7): platform toggles, SCREEN, ROMS and HELP.
static void ouiToggle(int idx)
{
  if (idx == OUI_FOC_HELP) { ouiOpenHelp(); return; }                  // grid slot 7 = HELP
  if (idx == OUI_FOC_ROMS && ouiHasRoms()) { ouiOpenRoms(); return; }  // grid slot 6 = ROMS on these machines
#if BOARD_HAS_SCREENFILL
  if (idx == ouiScreenSlot()) { ouiToggleScreenFill(); return; }
#endif
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
  if (ouiIsMsx()) {                       // MSX grid: SOUND / JOYSTICK / VIDEO / SPEED / DISK ROM
    switch (idx) {
      case 0: sound = !sound;           break;
      case 1: joystick = !joystick;     break;
      case 2: videoColor = !videoColor; break;
      case 3: msxFast = !msxFast;       break;   // NORMAL (3.58 MHz) <-> FAST (uncapped)
      case 4: msxDiskRom = (msxDiskRom + 1) % 3; msxApplyDiskRom(); break;   // AUTO -> ON -> OFF (resets the MSX)
      default: return;
    }
    optionsUiDirty = true;
    return;
  }
  if (ouiIsSms()) {                       // SMS grid: SOUND / JOYSTICK / VIDEO / SPEED
    switch (idx) {
      case 0: sound = !sound;           break;
      case 1: joystick = !joystick;     break;
      case 2: videoColor = !videoColor; break;
      case 3: smsFast = !smsFast;       break;   // NORMAL (3.58 MHz) <-> FAST (uncapped)
      default: return;
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
  if (ouiIsPcxt()) {                      // PCXT grid: SOUND / VIDEO
    switch (idx) {
      case 0: sound = !sound;           break;
      case 1: videoColor = !videoColor; break;
      default: return;
    }
    optionsUiDirty = true;
    return;
  }
  if (ouiIsNES() || ouiIsAtari()) {       // NES / Atari grid: SOUND / JOYSTICK / VIDEO (+ NES SPEED/SKIP)
    switch (idx) {
      case 0: sound = !sound;           break;
      case 1: joystick = !joystick;     break;
      case 2: videoColor = !videoColor; break;
      case 3: if (ouiIsNES()) { nesFast = !nesFast; break; }   // NES: SPEED NORMAL <-> FAST
#if defined(BOARD_PICOCALC_P4) || defined(BOARD_DESKTOP)
              atariFast = !atariFast; break;                  // Atari: SPEED NORMAL <-> FAST
#else
              return;
#endif
#if BOARD_DISPLAY_GFX
      case 4: if (!ouiIsNES()) return;        // NES (S3): cycle display frame-skip 1(off)->2->3
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
      shownFile = SHOWN_NONE; firstShowFile = 0; optionsUiSyncSelection();
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
  FileList &files = ouiFiles();
  int maxStart = (int)files.size() - OUI_FB_ROWS;
  if (maxStart < 0) maxStart = 0;
  int fs = (int)firstShowFile + dir;
  if (fs < 0) fs = 0;
  if (fs > maxStart) fs = maxStart;
  firstShowFile = (uint16_t)fs;
  optionsUiDirty = true;
}

// Sound off while a load runs (the PicoCalc makes noise while it stalls); a no-op elsewhere.
struct OuiLoadMute {
#if defined(BOARD_PICOCALC)
  OuiLoadMute()  { ampMute(true); }
  ~OuiLoadMute() { ampMute(false); }
#endif
};

// `run` matters only on the C64: false = mount (Enter in the list), true = mount & run (Ctrl-Enter,
// the LOAD & RUN button). The other loaders always start what they load.
static void ouiMount(bool run = true)
{
  FileList &files = ouiFiles();
  // Nothing listed (a failed scan): Enter still goes up a level. At the root that is a no-op.
  if (files.empty()) { ouiBrowse(".."); return; }
  if (shownFile >= files.size()) return;
  // Every browser can be sitting inside a subdirectory now, so a highlighted ".." or "name/"
  // row means navigate, never mount. Checked once here instead of in each per-core branch.
  if (ouiIsDir(files[shownFile])) { ouiBrowse(files[shownFile]); return; }
  OuiLoadMute mute;
  if (ouiIsC64()) {                       // C64: mount, or mount & run, the highlighted .prg/.d64/.crt
    if (shownFile >= files.size()) return;
    if (ouiEntryProblem(shownFile)) { optionsUiDirty = true; return; }   // greyed: reason is in the header
    const std::string pathS = files[shownFile];   // [i] is a temporary: keep the string alive
    const char *path = pathS.c_str();
    if (!(run ? c64LoadAndRun(path) : c64Mount(path))) {
      // Only a .crt fails here (a .prg/.d64 loads later, at READY). Grey it out with the reason
      // and stay open. If it had already pulled the previous cart out, the machine resets to
      // BASIC when the window closes (see c64LoadCRT).
      c64SetFileProblem(shownFile, c64CrtLastError());
      optionsUiDirty = true;
      return;
    }
    selectedC64FileName = files[shownFile].c_str();
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
  if (HdDisk) setHdFile();
  else {
    setDiskFile();
    // Hot swap: also re-detect the new image's DOS/ProDOS sector order (apple2InsertDisk), or a
    // ProDOS disk is read with the old disk's interleave and never boots.
    if (selectedDiskFileName != "/") apple2InsertDisk(selectedDiskFileName.c_str());
  }
  diskChanged = true;
  showHideOptionsWindow();   // mount selected image and close
}

// PCXT: toggle the highlighted image in A: (floppy) / C: (hard disk). If it's already mounted in that
// slot -> eject (stay in settings so the list/buttons update); otherwise mount it -> close settings.
static void ouiPcMountA()   // A: floppy
{
  FileList &files = ouiFiles();
  if (files.empty() || shownFile >= files.size()) return;
  if (ouiIsDir(files[shownFile])) { ouiBrowse(files[shownFile]); return; }  // dir row -> navigate
  bool isCur = ouiPcA().length() && files[shownFile] == std::string(ouiPcA().c_str());
  if (isCur) { pcxtUnmount(0); optionsUiDirty = true; }       // PC-XT: live mount/eject
  else { OuiLoadMute mute; if (pcxtMountA(files[shownFile].c_str())) showHideOptionsWindow(); }
}
static void ouiPcMountC()   // C: hard disk
{
  FileList &files = ouiFiles();
  if (files.empty() || shownFile >= files.size()) return;
  if (ouiIsDir(files[shownFile])) { ouiBrowse(files[shownFile]); return; }  // dir row -> navigate
  bool isCur = ouiPcC().length() && files[shownFile] == std::string(ouiPcC().c_str());
  if (isCur) { pcxtUnmount(2); optionsUiDirty = true; }
  else { OuiLoadMute mute; if (pcxtMountC(files[shownFile].c_str())) showHideOptionsWindow(); }
}

// MSX .dsk: MOUNT swaps the disk into the running machine, or UNMOUNTs it if it is the mounted one
// (stays in settings so the button flips); MOUNT & RUN boots it. Both close settings on success.
static void ouiMsxMount(bool run)
{
  FileList &files = ouiFiles();
  if (files.empty() || shownFile >= files.size()) return;
  if (ouiIsDir(files[shownFile])) { ouiBrowse(files[shownFile]); return; }  // dir row -> navigate
  if (!run && ouiMsxDskCur()) { ouiUnmount(); return; }
  OuiLoadMute mute;
  if (msxMountDisk(files[shownFile].c_str(), run)) showHideOptionsWindow();
}

// MSX Ctrl-Enter: mount the highlighted .dsk / .rom without running it -- never a reset, never a
// toggle back out (Del is the unmount). Already mounted: nothing to do but close.
static void ouiMsxMountOnly()
{
  FileList &files = ouiFiles();
  if (files.empty() || shownFile >= files.size()) return;
  if (ouiIsDir(files[shownFile])) { ouiBrowse(files[shownFile]); return; }  // dir row -> navigate
  const std::string pathS = files[shownFile];   // [i] is a temporary: keep the string alive
  const char *path = pathS.c_str();
  if (ouiCurMounted()) { showHideOptionsWindow(); return; }
  OuiLoadMute mute;
  if (ouiMsxDsk() ? msxMountDisk(path, false) : msxMountCart(path)) showHideOptionsWindow();
}

// Apple "MOUNT + REBOOT": apply the highlighted image as the boot device, save, then restart.
static void ouiMountReboot()
{
  if (ouiIsC64()) return;                 // C64 has no such button
  FileList &files = ouiFiles();
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
// platform (ouiDrawToggles) and leaves slots empty; and only the Apple, the PC-XT and the MSX
// (with a .dsk highlighted) have a middle action button. The focus ring skips all of those instead of vanishing onto them.
// Keep in step with ouiDrawToggles / ouiToggle / ouiDrawActions.
static bool ouiFocusable(int f)
{
  if (f >= 0 && f < OUI_TG_COUNT) {
    if (f == OUI_FOC_HELP) return true;
    if (f == OUI_FOC_ROMS && ouiHasRoms()) return true;                           // ROMS card
    if (f == ouiScreenSlot()) return true;                                        // SCREEN
    if (ouiIsMsx()) return f <= 4;                                                // 4 = DISK ROM
    if (ouiIsSms() || ouiIsColeco() || ouiIsZx()) return f <= 3;
    if (ouiIsPcxt())  return f <= 1;
#if defined(BOARD_PICOCALC_P4) || defined(BOARD_DESKTOP)
    if (ouiIsAtari()) return f <= 3;                                              // 3 = SPEED
#else
    if (ouiIsAtari()) return f <= 2;
#endif
    if (ouiIsNES())   return f <= 3 || (BOARD_DISPLAY_GFX && f == 4);             // 4 = SKIP (S3)
    return f <= 4;                                                                // C64, Apple II
  }
  if (f == OUI_FOC_MNTREBOOT) return !ouiActTwoButtons();
  if (f == OUI_FOC_VOL) return OUI_HAS_VOLUME;
  return true;
}

// The action row's first two buttons (see ouiDrawActions); joystick, keyboard and touch all come
// through here so the three can never disagree about what a button does.
static void ouiActFirst()   // MOUNT A: / UNMOUNT / MOUNT / LOAD & RUN
{
  if (ouiIsPcxt())          ouiPcMountA();
  else if (ouiCurMounted()) ouiUnmount();
  else if (ouiMsxDsk())     ouiMsxMount(false);
  else                      ouiMount();
}
static void ouiActMiddle()  // MOUNT C: / MOUNT&RUN / M+REBOOT / LOAD & RUN
{
  if (ouiIsPcxt())                             ouiPcMountC();
  else if (ouiMsxDsk())                        ouiMsxMount(true);
  else if (currentPlatform == PLATFORM_APPLE2) ouiMountReboot();
  else                                         ouiMount();
}

// ---- Joystick navigation (called from joystick.ino, core 0) ----
// Left/right move the focus; up/down act on the focused control; fire activates it.
void optionsUiNav(int dir)            // dir: -1 = left, +1 = right
{
  if (ouiGenMode)  { if (dir < 0) { if (!ouiGenBack()) optionsUiCloseGeneral(); } else ouiGenEnter(); return; }
  if (ouiSnapMode) { if (dir < 0) showHideOptionsWindow(); else ouiSnapLoad(); return; }
  if (ouiHelpOpen) { ouiCloseHelp(); return; }   // any input dismisses the help overlay
  if (ouiRomMode)  { if (dir < 0) ouiRomBack(); else ouiRomEnter(); return; }   // ROMS page
  for (int i = 0; i < OUI_FOC_COUNT; i++) {
    optionsUiFocus = (optionsUiFocus + dir + OUI_FOC_COUNT) % OUI_FOC_COUNT;
    if (optionsUiFocus != OUI_FOC_HELP && ouiFocusable(optionsUiFocus)) break;
  }
  optionsUiDirty = true;
}

void optionsUiAdjust(int dir)         // dir: -1 = up, +1 = down
{
  if (ouiHelpOpen) { ouiCloseHelp(); return; }
  if (ouiSnapMode) { ouiSnapMove(dir < 0 ? -1 : 1); return; }
  if (ouiRomMode || ouiGenMode) { ouiRomMove(dir < 0 ? -1 : 1); return; }
  int f = optionsUiFocus;
  if (f == OUI_FOC_HELP) return;          // opens on fire, not on up/down
  if (f >= 0 && f < OUI_TG_COUNT) { ouiToggle(f); return; }
  if (f == OUI_FOC_VOL) {
    if (dir < 0) { if (volume < 0xf0) volume += 0x10; }
    else         { if (volume > 0) { volume -= 0x10; if (volume > 0xf0) volume = 0; } }
    optionsUiDirty = true;
    return;
  }
  if (f == OUI_FOC_FILES) {
    FileList &files = ouiFiles();
    if (files.empty()) return;
    int idx = (int)shownFile + (dir < 0 ? -1 : 1);
    if (idx < 0) idx = 0;
    if (idx > (int)files.size() - 1) idx = (int)files.size() - 1;
    shownFile = (uint16_t)idx;
    if (shownFile < firstShowFile) firstShowFile = shownFile;
    else if (shownFile >= firstShowFile + OUI_FB_ROWS) firstShowFile = shownFile - OUI_FB_ROWS + 1;
    optionsUiDirty = true;
  }
  // MOUNT / REBOOT have nothing to adjust
}

void optionsUiActivate()              // joystick fire button on the focused control
{
  if (ouiGenMode)  { ouiGenEnter(); return; }
  if (ouiSnapMode) { if (ouiSnapResumeAt) showHideOptionsWindow(); else ouiSnapSave(); return; }   // fire = save; joystick right = load
  if (ouiHelpOpen) { ouiCloseHelp(); return; }
  if (ouiRomMode)  { ouiRomEnter(); return; }
  int f = optionsUiFocus;
  if (f >= 0 && f < OUI_TG_COUNT)  ouiToggle(f);   // incl. SCREEN, ROMS, HELP
  else if (f == OUI_FOC_FILES)     ouiMount();
  else if (f == OUI_FOC_MOUNT)     ouiActFirst();
  else if (f == OUI_FOC_MNTREBOOT) ouiActMiddle();
  else if (f == OUI_FOC_REBOOT)    ouiReboot();
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
// The arrows move by where the controls are actually drawn (ouiFocusRect), not by a fixed row
// map: the action row has two buttons on some machines and three on others, and the grid leaves
// different slots empty per platform, so a column index into a table never quite lined up with
// the screen. Up/Down go to the nearest row in that direction and, within it, to the control
// under the current horizontal position; Left/Right stay in the row. Both wrap around.

// Whether the action row is LOAD & RUN | REBOOT (two buttons) rather than three: the loaders, unless
// the highlighted entry is loaded (then UNMOUNT joins in). Mirrors ouiDrawActions.
static bool ouiActTwoButtons()
{
  if (ouiIsPcxt() || ouiMsxDsk() || ouiCurMounted()) return false;
  return ouiIsC64() || ouiIsNES() || ouiIsAtari() || ouiIsMsx() || ouiIsSms() || ouiIsColeco() || ouiIsZx();
}

// Screen rectangle of control f as drawn now, or false if it is not on the page.
static bool ouiFocusRect(int f, int &x, int &y, int &w, int &h)
{
  if (f >= 0 && f < OUI_TG_COUNT) {
    x = (f % 4) * OUI_TG_W; y = OUI_TG_TOP + (f / 4) * OUI_TG_H; w = OUI_TG_W; h = OUI_TG_H;
    return true;
  }
  if (f == OUI_FOC_VOL)   { if (!OUI_HAS_VOLUME) return false; x = 0; y = OUI_VOL_TOP; w = 320; h = OUI_VOL_H; return true; }
  if (f == OUI_FOC_FILES) { x = 0; y = OUI_FB_LIST; w = 320; h = OUI_FB_ROWS * OUI_FB_ROWH; return true; }
  y = OUI_ACT_TOP; h = OUI_ACT_H;
  if (ouiActTwoButtons()) {               // LOAD & RUN (6..126) | REBOOT (132..314)
    if (f == OUI_FOC_MOUNT)  { x = 6;   w = 120; return true; }
    if (f == OUI_FOC_REBOOT) { x = 132; w = 182; return true; }
    return false;
  }
  if (f == OUI_FOC_MOUNT)     { x = 4;   w = 102; return true; }
  if (f == OUI_FOC_MNTREBOOT) { x = 109; w = 102; return true; }
  if (f == OUI_FOC_REBOOT)    { x = 214; w = 102; return true; }
  return false;
}

// Horizontal position Up/Down aim for, kept while moving vertically so that passing through the
// full-width VOLUME / file list comes back out in the same column. Valid only for ouiNavFor.
static int ouiNavX = -1, ouiNavFor = -1;

static int ouiNextFocus(int dx, int dy)
{
  int cx, cy, cw, ch;
  if (!ouiFocusRect(optionsUiFocus, cx, cy, cw, ch)) return OUI_FOC_FILES;   // lost: start over
  const int px = (dy && ouiNavFor == optionsUiFocus && ouiNavX >= 0) ? ouiNavX : cx + cw / 2;
  const int py = cy + ch / 2;
  int best = -1;
  long bestScore = 0;
  for (int pass = 0; pass < 2 && best < 0; pass++) {       // pass 1 = nothing that way: wrap
    for (int f = 0; f < OUI_FOC_COUNT; f++) {
      if (f == optionsUiFocus || !ouiFocusable(f)) continue;
      int x, y, w, h;
      if (!ouiFocusRect(f, x, y, w, h)) continue;
      const bool sameRow = y < cy + ch && cy < y + h;
      const int fx = x + w / 2, fy = y + h / 2;
      long along, across;
      if (dy) {
        if (sameRow) continue;
        along  = (long)(fy - py) * dy;                     // > 0: in the direction pressed
        across = px < x ? x - px : px >= x + w ? px - (x + w - 1) : 0;   // 0 = right under us
      } else {
        if (!sameRow) continue;
        along  = (long)(fx - (cx + cw / 2)) * dx;
        across = 0;
      }
      if (pass == 0 && along <= 0) continue;
      // Nearest row (or neighbour) first; on a wrap the farthest one, i.e. the other end.
      long score = along * 1000000L + across * 1000L + abs(fx - px);
      if (best < 0 || score < bestScore) { best = f; bestScore = score; }
    }
  }
  if (best < 0) return optionsUiFocus;                     // alone in its row
  if (dy) { ouiNavX = px; ouiNavFor = best; } else ouiNavFor = -1;
  return best;
}

// dx/dy: -1 = left/up, +1 = right/down (0 = no movement on that axis).
void optionsUiKeyArrow(int dx, int dy)
{
  if (ouiGenMode == 4) { ouiGenBack(); return; }   // general help: any key closes it
  if (ouiHelpOpen) { ouiCloseHelp(); return; }
  if (ouiSnapMode) {                      // Up/Down move, Left/Right page
    if (dy) ouiSnapMove(dy);
    else if (dx) ouiSnapMove(dx * OUI_SN_ROWS);
    return;
  }
  if (ouiRomMode || ouiGenMode) {         // list pages: Up/Down move, Left/Right page                       // ROMS page: Up/Down move, Left/Right page
    if (dy) ouiRomMove(dy);
    else if (dx) ouiRomMove(dx * OUI_RS_ROWS);
    return;
  }

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

  if (dx) optionsUiFocus = ouiNextFocus(dx, 0);
  if (dy) optionsUiFocus = ouiNextFocus(0, dy);
  optionsUiDirty = true;
}

void optionsUiKeyEnter(bool ctrl, bool shift)
{
  if (ouiGenMode)  { ouiGenEnter(); return; }
  if (ouiSnapMode) {
    if (ouiSnapResumeAt) showHideOptionsWindow();   // just loaded: Enter resumes now (S/L/D act)
    return;
  }
  if (ouiHelpOpen) { ouiCloseHelp(); return; }
  if (ouiRomMode)  { ouiRomEnter(); return; }
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
    else if (ouiIsMsx()) ouiMsxMountOnly();      // MSX: mount only (.dsk live, .rom at next reset); Del unmounts
    else ouiMount(true);  // C64/NES/Atari/SMS: no reboot variant, Ctrl-Enter = LOAD & RUN
    return;
  }

  // VOLUME and the file list hold a value rather than perform an action, so Enter opens them
  // and the arrows then work inside them.
  if (f == OUI_FOC_VOL || f == OUI_FOC_FILES) {
    if (!ouiEditing) { ouiEditing = true; optionsUiDirty = true; return; }
    if (f == OUI_FOC_VOL) { ouiEditing = false; optionsUiDirty = true; return; }   // done
    ouiMount(false);  // file list: a directory row navigates, a file row mounts (and closes)
    return;
  }
  optionsUiActivate();   // toggles flip, action buttons fire -- no mode to enter
}

// Del / Backspace: unmount the highlighted file-list entry if it is the loaded one -- the UNMOUNT
// button without leaving the list, the same on every platform (the PC-XT ejects it from A: and/or
// C:). The highlight is always drawn, so it works whichever control has the focus; on the ROMS,
// system-menu and help pages it does nothing.
// S / L / D on the snapshot page: the SAVE (S) / LOAD (L) / DELETE (D) buttons. Elsewhere: nothing.
bool optionsUiSnapKey(char c)
{
  if (!ouiSnapMode) return false;
  if (ouiSnapResumeAt) { showHideOptionsWindow(); return true; }   // just loaded: resume now
  switch (c) {
    case 's': ouiSnapSave();   break;
    case 'l': ouiSnapLoad();   break;
    case 'd': ouiSnapDelete(); break;
  }
  return true;
}

// A letter or digit typed while the file list has the focus: jump to the next entry whose name starts
// with it (again = the one after that, wrapping), or, when none does, to the first name that sorts
// after it -- the nearest. Directories go by their own name, not the brackets; ".." is skipped.
static char ouiInitial(const std::string &e)
{
  if (e == "..") return 0;
  for (char ch : ouiDisplayName(e)) {
    if (ch >= 'A' && ch <= 'Z') return (char)(ch - 'A' + 'a');
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) return ch;
  }
  return 0;
}

void optionsUiKeyLetter(char c)
{
  if (ouiHelpOpen || ouiSnapMode || ouiRomMode || ouiGenMode || optionsUiFocus != OUI_FOC_FILES) return;
  if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  FileList &files = ouiFiles();
  const int n = (int)files.size();
  if (n == 0) return;
  int hit = -1;
  for (int k = 1; k <= n && hit < 0; k++) {           // starting after the current row, wrapping
    const int i = ((int)shownFile + k) % n;
    if (ouiInitial(files[i]) == c) hit = i;
  }
  if (hit < 0) {                                      // none: the nearest name after the letter
    char best = 0;
    for (int i = 0; i < n; i++) {
      const char ini = ouiInitial(files[i]);
      if (ini > c && (!best || ini < best)) { best = ini; hit = i; }
    }
  }
  if (hit < 0 || hit == (int)shownFile) return;
  shownFile = (uint16_t)hit;
  if (shownFile < firstShowFile || shownFile >= firstShowFile + OUI_FB_ROWS) {
    int top = hit - OUI_FB_ROWS / 2;                  // land it mid-list, so its neighbours show
    if (top > n - OUI_FB_ROWS) top = n - OUI_FB_ROWS;
    firstShowFile = (uint16_t)(top < 0 ? 0 : top);
  }
  ouiEditing = true;                                  // the arrows now move inside the list
  optionsUiDirty = true;
}

void optionsUiKeyUnmount()
{
  if (ouiSnapMode) return;                   // the snapshot page deletes with D
  if (ouiGenMode || ouiHelpOpen || ouiRomMode) return;
  ouiUnmount();
}

// Returns true if Escape was consumed here. False means "nothing was open" and the caller
// should close the settings window.
bool optionsUiKeyEscape()
{
  if (ouiGenMode)  return ouiGenBack();
  if (ouiSnapMode) {                         // a pending delete first, then close (= resume)
    if (ouiSnapDelArm >= 0) { ouiSnapDelArm = -1; optionsUiDirty = true; return true; }
    return false;
  }
  if (ouiHelpOpen) { ouiCloseHelp(); return true; }
  if (ouiRomMode)  { ouiRomBack(); return true; }
  if (ouiEditing)  { ouiEditing = false; optionsUiDirty = true; return true; }
  return false;
}

// HELP overlay open/close. Opening/closing forces a full repaint (the pages don't overlap).
static void ouiOpenHelp()  { ouiHelpOpen = true;  optionsUiFirstDraw = true; optionsUiDirty = true; }
static void ouiCloseHelp() { ouiHelpOpen = false; optionsUiFirstDraw = true; optionsUiDirty = true; }

static void ouiHandleTap(int16_t x, int16_t y)
{
  if (ouiGenMode)  { ouiGenTap(x, y); return; }   // general settings (over the system menu)
  if (ouiSnapMode) { ouiSnapTap(x, y); return; }  // snapshot page
  // HELP overlay is modal: any tap returns to the settings page.
  if (ouiHelpOpen) { ouiCloseHelp(); return; }
  if (ouiRomMode)  { ouiRomTap(x, y); return; }   // ROMS page is modal too

  // close button
  if (y < OUI_TITLE_H && x >= 320 - OUI_TITLE_H) { showHideOptionsWindow(); return; }
#if !OUI_PICOCALC
  if (y < OUI_TITLE_H && x >= 320 - OUI_TITLE_H - OUI_SNAP_BTN_W - 4 && snapSystemName()) { optionsUiOpenSnap(); return; }
#endif

  // toggle grid
  if (y >= OUI_TG_TOP && y < OUI_TG_TOP + 2 * OUI_TG_H) {
    int col = x / OUI_TG_W, row = (y - OUI_TG_TOP) / OUI_TG_H;
    int idx = row * 4 + col;
    if (idx >= 0 && idx < OUI_TG_COUNT) ouiToggle(idx);   // incl. SCREEN, ROMS, HELP
    return;
  }

  // volume +/-
  if (OUI_HAS_VOLUME && y >= OUI_VOL_TOP && y < OUI_VOL_TOP + OUI_VOL_H) {
    if (x >= 120 && x < 148) { if (volume > 0) { volume -= 0x10; if (volume > 0xf0) volume = 0; } optionsUiDirty = true; return; }
    if (x >= 288 && x < 316) { if (volume < 0xf0) volume += 0x10; optionsUiDirty = true; return; }
  }

  // file list / scroll
  int listH = OUI_FB_ROWS * OUI_FB_ROWH;
  if (y >= OUI_FB_LIST && y < OUI_FB_LIST + listH) {
    if (x >= 302) { ouiScroll(y < OUI_FB_LIST + listH / 2 ? -1 : 1); return; }
    if (x < 300) {
      FileList &files = ouiFiles();
      int idx = firstShowFile + (y - OUI_FB_LIST) / OUI_FB_ROWH;
      if (idx < (int)files.size()) {
        if (ouiIsDir(files[idx])) ouiBrowse(files[idx]);   // enter dir / go up
        else { shownFile = (uint16_t)idx; optionsUiDirty = true; }
      }
      return;
    }
  }

  // action buttons
  if (y >= OUI_ACT_TOP && y < OUI_ACT_TOP + OUI_ACT_H) {
    if (ouiActTwoButtons()) {                 // LOAD & RUN (6..126) | REBOOT (132..314)
      if (x >= 6 && x < 126)        ouiActFirst();
      else if (x >= 132 && x < 314) ouiReboot();
    } else {                                  // first (4..106) | middle (109..211) | REBOOT (214..316)
      if (x >= 4 && x < 106)        ouiActFirst();
      else if (x >= 109 && x < 211) ouiActMiddle();
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
#if OUI_PICOCALC
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
  // Reopen on the same focused control (and still inside VOL / the file list if it was closed from
  // there), but always with the mounted image highlighted and scrolled into view. If the list is
  // showing another folder, go into the mounted image's folder first.
  {
    const std::string sel = ouiSel();
    FileList &files = ouiFiles();
    bool listed = false;
    for (const std::string &f : files) if (f == sel) { listed = true; break; }
    if (!listed && sel.size() > 1 && sel[0] == '/' && sel.back() != '/') {
      const size_t sl = sel.find_last_of('/');
      ouiBrowse(sl == 0 ? std::string("/") : sel.substr(0, sl + 1));   // rescans, then syncs
    }
    optionsUiSyncSelection();
  }
#if !OUI_HAS_VOLUME
  volume = 0xf0;                 // the board's own volume knob does the job; keep the app at max
  if (optionsUiFocus == OUI_FOC_VOL) optionsUiFocus = OUI_FOC_FILES;
#endif
  if (optionsUiFocus != OUI_FOC_VOL && optionsUiFocus != OUI_FOC_FILES) ouiEditing = false;
  ouiHelpOpen          = false;  // always open on the settings page, not the help overlay
  ouiRomClose();                 // ...nor the ROMS page
  ouiSnapMode          = 0;      // ...nor the snapshot page
  ouiSnapResumeAt      = 0;
  optionsUiFirstDraw   = true;
  optionsUiDirty       = true;
  optionsUiWaitRelease = true;   // don't treat the opening tap as a click
  optionsUiPrevDown    = true;
}

// Called by the PS/2 menu handlers (optionsScreenRender/listFiles) so keyboard
// changes refresh the touch UI and keep the selected file visible.
void optionsUiMarkDirty()
{
  if (shownFile != SHOWN_NONE) {
    if (shownFile < firstShowFile) firstShowFile = shownFile;
    else if (shownFile >= firstShowFile + OUI_FB_ROWS) firstShowFile = shownFile - OUI_FB_ROWS + 1;
  }
  optionsUiDirty = true;
}
