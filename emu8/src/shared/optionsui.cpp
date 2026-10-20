#include "../../emu.h"
#include "snapshot.h"
#include "filebrowser.h"
#if BOARD_INPUT_USB
#include "c64keymap.h"
#include "neskeymap.h"
#include "smskeymap.h"
#include "colecokeymap.h"
#include "atarikeymap.h"
#include "c64joymap.h"
#include "apple2keymap.h"
#include "apple2joymap.h"
#include "msxkeymap.h"
#include "msxjoymap.h"
#include "zxkeymap.h"
#include "zxjoymap.h"
#endif
#include "apple2joycal.h"

// The PicoCalc's 320x320 keyboard-driven menu. The desktop build can simulate it
// (-DEMU8_SIM_PICOCALC=ON, see CMakeLists.txt) so layout changes can be checked without the board.
#if UI_PICOCALC
#define OUI_PICOCALC 1
#else
#define OUI_PICOCALC 0
#endif
// The C64 KEYBOARD page as a picture of both keyboards (PicoCalc only: it draws the PicoCalc's keys).
#if OUI_PICOCALC && BOARD_INPUT_USB
#define OUI_KV 1
#else
#define OUI_KV 0
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
static uint8_t ouiCfgMode        = 0;       // C64 CONFIG page: 0 closed, see ouiDrawCfg
static int ouiGenCount();
static int ouiCfgCount();
static void ouiCfgGoto(uint8_t mode, int sel);
static int ouiCfgRowOf(int item);
static void ouiCfgClose();
static void ouiKvInvalidate();

// Joystick focus: left/right moves between controls; the focused one gets a white
// border. Order: the 8 toggle-grid slots, then volume, file list, MOUNT, SAVE & REBOOT.
// A grid slot's focus id is its index. The last slot (7) is always HELP / CONFIG (CONFIG holds
// ROMS on every machine with system ROMs); SCREEN (fill / original, S3 panel + desktop window
// only) takes slot 6. The platform toggles fill slots 0..5 from the left.
// Empty slots are skipped (ouiFocusable). CPU speed readouts are in the title bar.
#define OUI_TG_COUNT      8
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
static bool ouiIsApple() { return currentPlatform == PLATFORM_APPLE2; }
// Whether the running machine loads any system ROM the user can pick: the ROMS entry in CONFIG.
static bool ouiHasRoms()
{
  for (int i = 0; i < ROMSEL_COUNT; i++) if (romSlotVisible(i)) return true;
  return false;
}
// Whether the running machine has a keymap the CONFIG > KEYBOARD pages can edit (PC-XT: none).
static bool ouiHasKeymap() { return ouiIsC64() || ouiIsNES() || ouiIsSms() || ouiIsColeco() || ouiIsAtari() || ouiIsApple() || ouiIsMsx() || ouiIsZx(); }
// Whether grid slot 7 is CONFIG (KEYBOARD / JOYSTICK / ROMS / HELP) rather than HELP: every
// machine with system ROMs, plus those with a keyboard to map.
static bool ouiHasCfg() { return ouiIsC64() || ouiHasRoms() || (BOARD_INPUT_USB && ouiHasKeymap()) || (A2CAL_ANALOG && ouiIsApple()); }
// Grid slot of the SCREEN toggle (-1 = none on this board): just left of HELP / CONFIG.
static int ouiScreenSlot()
{
#if BOARD_HAS_SCREENFILL
  return 6;
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
  ouiKvInvalidate();
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

// Blank the platform slots from `first` on. Slot 7 is always HELP / CONFIG, and SCREEN sits in 6
// where the board has it: clearing those only for them to be drawn again would repaint them on
// every pass.
static void ouiClearToggles(int first)
{
  for (int i = first; i < OUI_FOC_HELP; i++) {
    if (i == ouiScreenSlot()) continue;
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
  // therefore DEVICE SPEED SOUND ARROWS VIDEO, 6 SCREEN, 7 CONFIG.
  ouiDrawToggle(0, "DEVICE",   HdDisk ? "HD" : "DISK",          OUI_TXT);
  ouiDrawToggle(1, "SPEED",    Fast1MhzSpeed ? "FAST" : "1MHz", OUI_TXT);
  ouiDrawToggle(2, "SOUND",    sound ? "ON" : "MUTE",           OUI_TXT);
  static const char *const kArrows[APPLE_ARROWS_MODES] = { "JOY", "KBD", "BOTH" };
  ouiDrawToggle(3, "ARROWS",   kArrows[appleArrows < APPLE_ARROWS_MODES ? appleArrows : 0], OUI_TXT);
  ouiDrawToggle(4, "VIDEO",    videoColor ? "COLOR" : "MONO",   OUI_TXT);
  ouiClearToggles(5);
  ouiDrawScreenToggle();   // SCREEN (FILL/ORIG) in 6, 7 = CONFIG. 6502 MHz shows in the title bar.
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
// controls cheat-sheet overlay (ouiDrawHelp). Drawn for every platform; where ouiHasCfg() it is
// CONFIG, the page that holds KEYBOARD, JOYSTICK, ROMS and HELP (ouiDrawCfg).
static void ouiDrawHelpButton()
{
  int idx = OUI_FOC_HELP, col = idx % 4, row = idx / 4;
  const bool cfg = ouiHasCfg();
  if (!ouiChanged(ouiSigCard[idx], (cfg ? 64u : 16u) + (uint32_t)ouiFocusState(idx))) return;
  int x = col * OUI_TG_W, y = OUI_TG_TOP + row * OUI_TG_H;
  uint16_t face = cfg ? tft.color565(92, 70, 130) : tft.color565(58, 92, 130);
  tft.fillRoundRect(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5, face);
  tft.drawRoundRect(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5, OUI_BORDER);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_TXT, face);
  tft.drawString(cfg ? "CONFIG" : "HELP", x + OUI_TG_W / 2, y + OUI_TG_H / 2, 2);
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
    ouiHelpRow(y, "] / [",       "A / B");
    ouiHelpRow(y, "/ / `",       "Start / Select");
#if BOARD_INPUT_USB
    ouiHelpRow(y, "Remap",       "CONFIG>KEYBOARD");
#endif
#if !OUI_PICOCALC   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "NES  -  GAMEPAD");
    ouiHelpRow(y, "D-pad",       "D-pad");
    ouiHelpRow(y, "A / B",       "A / B");
    ouiHelpRow(y, "Start / Sel", "Start / Select");
#endif
  } else if (ouiIsAtari()) {
    ouiHelpHdr(y, "ATARI  -  KEYBOARD");
    ouiHelpRow(y, "Arrows",    "Joystick");
    ouiHelpRow(y, "]",         "Fire");
    ouiHelpRow(y, "/",         "Reset switch");
    ouiHelpRow(y, "`",         "Select switch");
#if BOARD_INPUT_USB
    ouiHelpRow(y, "Remap",     "CONFIG>KEYBOARD");
#endif
#if !OUI_PICOCALC   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "ATARI  -  GAMEPAD");
    ouiHelpRow(y, "D-pad", "Joystick");
    ouiHelpRow(y, "A / B", "Fire / Select");
    ouiHelpRow(y, "Start", "Reset");
#endif
  } else if (ouiIsC64()) {
    ouiHelpHdr(y, "C64  -  KEYBOARD");
    ouiHelpRow(y, "Keys",      "C64 layout; CONFIG>KEYBOARD");
    ouiHelpRow(y, "Arrows",    "Cursor (JOY off)");
    ouiHelpRow(y, "Arrows+] [ =", "Joystick (JOY on)");
    ouiHelpRow(y, "Enter",      "Files: mount");
    ouiHelpRow(y, "Ctrl-Enter", "Files: mount + run");
#if !OUI_PICOCALC   // no gamepad port on this board; see the note at the Apple section
    ouiHelpHdr(y, "C64  -  GAMEPAD");
    ouiHelpRow(y, "D-pad", "Stick");
    ouiHelpRow(y, "A",     "Fire");
#endif
  } else if (ouiIsMsx()) {
    ouiHelpHdr(y, "MSX  -  KEYBOARD");
#if BOARD_INPUT_USB
    ouiHelpRow(y, "Keys",      "MSX layout; CONFIG>KEYBOARD");
#else
    ouiHelpRow(y, "Keys",      "MSX layout");
#endif
    ouiHelpRow(y, "Arrows",    "Cursor / Joystick");
    ouiHelpRow(y, "] / [",     "Trigger A / B");
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
    ouiHelpRow(y, "[ / ]",  "Button 1 / 2");
#if OUI_PICOCALC
    ouiHelpRow(y, "Ctrl-F3",  "PAUSE");
#else
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
    ouiHelpRow(y, "[ / ]",  "Left / right fire");
    ouiHelpRow(y, "0-9",    "Keypad");
    ouiHelpRow(y, "- / =",  "Keypad * / #");
    ouiHelpRow(y, "F12",    "Reset");
#if BOARD_INPUT_USB
    ouiHelpRow(y, "Remap",  "CONFIG>KEYBOARD");
#endif
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
    ouiHelpRow(y, "]",         "Kempston fire (JOYSTICK on)");
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
#if BOARD_INPUT_USB
    ouiHelpRow(y, "Keys",   AppleIIe ? "IIe layout; CONFIG>KEYBOARD" : "II+ layout; CONFIG>KEYBOARD");
#else
    ouiHelpRow(y, "Keys",   "Type into Apple");
#endif
    ouiHelpRow(y, "Arrows", "ARROWS: JOY/KBD/BOTH");
    ouiHelpRow(y, "] / [",   "Button 0 / 1");
#if OUI_PICOCALC
    ouiHelpRow(y, "Ctrl-F3", "Reset the Apple");
#else
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
  if (!ouiRomMode && ouiCfgMode) return ouiCfgCount();   // ROMS opens over CONFIG
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
  if (ouiCfgMode) { ouiRomMsg[0] = 0; ouiCfgGoto(1, ouiCfgRowOf(1)); return; }   // opened from CONFIG: back there
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
// The list pages' title bar (title, battery, the X) and footer (the last result, else `hint`); the
// KEYBOARD picture page (ouiDrawKv) has them too.
static void ouiDrawListTitle(const char *title)
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
}

static void ouiDrawListFoot(const char *hint)
{
  const char *foot = ouiRomMsg[0] ? ouiRomMsg : hint;
  uint16_t fc = ouiRomMsg[0] ? (ouiRomMsgErr ? OUI_RED : OUI_ON) : OUI_LBL;
  char fs[80];
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

static void ouiDrawList(const char *title, const char *hdr, const char *hint, OuiListRow row, int lblW)
{
  ouiDrawListTitle(title);

  if (ouiChanged(ouiSigHdr, ouiHash(hdr))) {
    tft.fillRect(0, OUI_RS_HDR, 320, OUI_RS_LIST - OUI_RS_HDR, OUI_BG);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(OUI_LBL, OUI_BG);
    const int hy = OUI_RS_HDR + (OUI_RS_LIST - OUI_RS_HDR) / 2 - 1;
    const char *tab = strchr(hdr, '\t');   // "left\tright": headings over the label and text columns
    if (tab) {
      std::string l(hdr, tab - hdr);
      tft.drawString(ouiRomFit(l.c_str(), lblW - 12, 1).c_str(), 9, hy, 1);
      tft.drawString(ouiRomFit(tab + 1, 306 - lblW, 1).c_str(), lblW, hy, 1);
    } else {
      tft.drawString(ouiRomFit(hdr, 306, 1).c_str(), 7, hy, 1);
    }
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

  ouiDrawListFoot(hint);   // the last result, else the keys
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
//   mode 1  the list: ROM folder, apps folder, snapshot folder, SD manager, reset settings, help
//   mode 2  a folder picker for one of the three folders (folders only)
//   mode 3  "erase everything?" confirmation
//   mode 4  the general help page
// The list pages' state (ouiRomSel, ouiRomFiles, the footer message...) is shared with the ROMS
// page, which cannot be open at the same time.
#if defined(BOARD_DESKTOP)
#define OUI_GEN_SDMGR -1                      // no SD Manager: the "card" is a host folder already
#define OUI_GEN_RESET 3                       // mode 1 rows after the three folders
#else
#define OUI_GEN_SDMGR 3                       // reboot into the SD card file manager (one boot)
#define OUI_GEN_RESET 4
#endif
#define OUI_GEN_HELP  (OUI_GEN_RESET + 1)
#define OUI_GEN_ROWS  (OUI_GEN_HELP + 1)
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
      case OUI_GEN_SDMGR: snprintf(a, na, "SD MANAGER"); snprintf(b, nb, "Reboot into the SD card file manager"); bc = OUI_REBOOT; break;
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
    else if (ouiRomSel == OUI_GEN_SDMGR) {
      tft.fillScreen(OUI_BG);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(OUI_TXT, OUI_BG);
      tft.drawString("Starting SD Manager...", 160, OUI_SCR_H / 2, 2);
      displayFlush();
      saveConfig();                            // keeps the emulator's byte; the one-boot flag picks the mode
      rebootToSdManager();
    }
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
  ouiHelpOpen = false; ouiRomClose(); ouiCfgClose();
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

// --- CONFIG page (grid slot 7, see ouiHasCfg): the machine's setup pages behind one card ---
//   mode 1  the list: KEYBOARD (boards with a USB / PicoCalc keyboard), ROMS, HELP
//   mode 2  KEYBOARD: every host key and its SHIFT+ variant with the C64 key it presses
//           (c64keymap.cpp); green = changed from the default. The first row arms "press a key
//           to find it": the next key typed jumps to its row (optionsUiKeyCapture).
//   mode 3  the C64 key for one of them: the default, NONE, RESTORE, then the 64 keys
//           (NES: the default, NONE, then the 8 buttons; a pick saves at once)
//   mode 4  what that key does to the C64's SHIFT (not on the NES)
//   mode 5  "reset every key?" confirmation
// A change is live at once and saved to /emu8/c64/keymap.txt (/emu8/nes/keymap.txt).
// JOYSTICK (C64) is the same pages over the joystick map (c64joymap.cpp, /emu8/c64/joymap.txt): each host
// key and the stick direction / FIRE it gives, picked like an NES button (no SHIFT step). ROMS and HELP open over this page
// (ouiRomMode / ouiHelpOpen are checked first everywhere) and come back to it.
// Shares the list pages' state (ouiRomSel, ouiRomMsg...) like ROMS / general.
#define OUI_CFG_KEYB 0
#define OUI_CFG_ROMS 1   // (ouiRomBack knows this one by number)
#define OUI_CFG_HELP 2
#define OUI_CFG_JOY  3   // C64: the keys on its joystick (c64joymap.cpp), the KEYBOARD pages again
#define OUI_CFG_CAL  4   // Apple II: calibrate the analog stick (apple2joycal.cpp), cfg mode OUI_CAL_MODE
#define OUI_CAL_MODE 9   // the CALIBRATE page (modes 6-8 are the KEYBOARD picture)
#define OUI_CAL_STEP  0
#define OUI_CAL_RAW   1
#define OUI_CAL_X     2
#define OUI_CAL_Y     3
#define OUI_CAL_PDL   4
#define OUI_CAL_RESET 5
#define OUI_CAL_ROWS  6
static bool ouiCfgKv() { return ouiCfgMode >= 6 && ouiCfgMode <= 8; }
static int ouiCfgItems(int *out)
{
  int n = 0;
#if BOARD_INPUT_USB
  if (ouiHasKeymap()) out[n++] = OUI_CFG_KEYB;
  if (ouiIsC64() || ouiIsApple() || ouiIsMsx() || ouiIsZx()) out[n++] = OUI_CFG_JOY;
#endif
  if (A2CAL_ANALOG && ouiIsApple()) out[n++] = OUI_CFG_CAL;
  if (ouiHasRoms()) out[n++] = OUI_CFG_ROMS;
  out[n++] = OUI_CFG_HELP;
  return n;
}
static int ouiCfgRowOf(int item)
{
  int it[5], n = ouiCfgItems(it);
  for (int i = 0; i < n; i++) if (it[i] == item) return i;
  return 0;
}

// Scroll so the selected row sits mid-list (a jump, not a step).
static void ouiCfgCenter()
{
  const int rows = ouiListRows(), n = ouiRomCount();
  int top = ouiRomSel - rows / 2;
  if (top > n - rows) top = n - rows;
  ouiRomFirst = top < 0 ? 0 : top;
  optionsUiDirty = true;
}

#if BOARD_INPUT_USB
#define OUI_KM_FIND  0           // mode 2 rows above the keys: "find a key",
#define OUI_KM_ONLY  1           // "show only changed keys" on/off,
#define OUI_KM_RESET 2           // "reset all"
#define OUI_KM_FIXED 3
#define OUI_KP_FIXED 3           // mode 3 rows above the 64 keys: default, NONE, RESTORE
static uint8_t ouiKmHid = 0;     // modes 3-4: the host key being changed...
static bool    ouiKmShift = false;   // ...its SHIFT+ rule?
static uint8_t ouiKmKey = 0;     // mode 4: the C64 key (matrix index) picked
static int     ouiKmRow = 0;     // its mode 2 row, to come back to
static bool    ouiKmCapture = false;
static bool    ouiKmOnly = false;    // mode 2 lists only the keys that differ from the default

// The map the KEYBOARD pages edit: the C64's (c64keymap.cpp), the NES's (neskeymap.cpp) or the
// SMS's (smskeymap.cpp). All share the host keys and the NONE / UNMAP codes; the NES and the SMS
// have no SHIFT+ rules and no SHIFT choice.
// The C64's JOYSTICK pages (c64joymap.cpp) are these pages too: a map like the NES's (one rule per
// key, no SHIFT), with SHIFT itself as one more host key.
static bool    ouiKmJoyPage = false;   // the KEYBOARD pages are showing the C64 joystick map
static bool    ouiKmNes()    { return ouiIsNES(); }
static bool    ouiKmSms()    { return ouiIsSms(); }
static bool    ouiKmAtari()  { return ouiIsAtari(); }
// The ColecoVision (colecokeymap.cpp): a pick saves at once like the NES, but it keeps SHIFT+ rules
// (Shift-8 / Shift-3 are the keypad's '*' / '#', as printed and as the PicoCalc sends them).
static bool    ouiKmCol()    { return ouiIsColeco(); }
static bool    ouiKmJoy()    { return ouiKmJoyPage && (ouiIsC64() || ouiIsApple() || ouiIsMsx() || ouiIsZx()); }
// The Apple II's pages (apple2keymap.cpp / apple2joymap.cpp): its keyboard map is the C64's kind
// (SHIFT+ rules, a SHIFT step that also offers +CTRL, RESET where the C64 has RESTORE), its joystick
// map the C64 joystick's kind.
static bool    ouiKmA2()     { return ouiIsApple() && !ouiKmJoyPage; }
static bool    ouiKmA2J()    { return ouiIsApple() && ouiKmJoyPage; }
// The ZX Spectrum's pages (zxkeymap.cpp / zxjoymap.cpp): its keyboard map is the C64's kind (SHIFT+
// rules, a SHIFT step: as typed / +CAPS / -CAPS / +SYMBOL, no RESTORE), its Kempston map the C64
// joystick's kind without SHIFT as a key (SHIFT is CAPS SHIFT, and switches the picture's layer).
// The MSX's pages (msxkeymap.cpp / msxjoymap.cpp): the C64's kinds again, its keys coded
// mode * 72 + matrix index (msxkeymap.h) and no RESTORE.
static bool    ouiKmMsx()    { return ouiIsMsx() && !ouiKmJoyPage; }
static bool    ouiKmMsxJ()   { return ouiIsMsx() && ouiKmJoyPage; }
static bool    ouiKmZx()     { return ouiIsZx() && !ouiKmJoyPage; }
static bool    ouiKmZxJ()    { return ouiIsZx() && ouiKmJoyPage; }
static bool    ouiKmJoyShift() { return ouiKmJoy() && !ouiKmZxJ(); }   // SHIFT is a joystick key to map
static bool    ouiKmSimple() { return ouiKmNes() || ouiKmSms() || ouiKmCol() || ouiKmAtari() || ouiKmJoy(); }   // a pick saves at once, no SHIFT step
static int     ouiKmItem()   { return ouiKmJoy() ? OUI_CFG_JOY : OUI_CFG_KEYB; }
static uint8_t ouiKmRule(uint8_t h, bool s, bool *o)
{
  return ouiKmCol() ? colecoKeymapRule(h, s, o) : ouiKmNes() ? nesKeymapRule(h, o) : ouiKmSms() ? smsKeymapRule(h, o) : ouiKmAtari() ? atariKeymapRule(h, o)
       : ouiKmMsxJ() ? msxJoymapRule(h, o) : ouiKmA2J() ? apple2JoymapRule(h, o) : ouiKmZxJ() ? zxJoymapRule(h, o) : ouiKmJoy() ? c64JoymapRule(h, o)
       : ouiKmMsx() ? msxKeymapRule(h, s, o) : ouiKmA2() ? apple2KeymapRule(h, s, o) : ouiKmZx() ? zxKeymapRule(h, s, o) : c64KeymapRule(h, s, o);
}
static uint8_t ouiKmDefault(uint8_t h, bool s)
{
  return ouiKmCol() ? colecoKeymapDefault(h, s) : ouiKmNes() ? nesKeymapDefault(h) : ouiKmSms() ? smsKeymapDefault(h) : ouiKmAtari() ? atariKeymapDefault(h)
       : ouiKmMsxJ() ? msxJoymapDefault(h) : ouiKmA2J() ? apple2JoymapDefault(h) : ouiKmZxJ() ? zxJoymapDefault(h) : ouiKmJoy() ? c64JoymapDefault(h)
       : ouiKmMsx() ? msxKeymapDefault(h, s) : ouiKmA2() ? apple2KeymapDefault(h, s) : ouiKmZx() ? zxKeymapDefault(h, s) : c64KeymapDefault(h, s);
}
static uint8_t ouiKmLookup(uint8_t h, bool s)
{
  return ouiKmCol() ? colecoKeymapLookup(h, s) : ouiKmNes() ? nesKeymapLookup(h) : ouiKmSms() ? smsKeymapLookup(h) : ouiKmAtari() ? atariKeymapLookup(h)
       : ouiKmMsxJ() ? msxJoymapLookup(h) : ouiKmA2J() ? apple2JoymapLookup(h) : ouiKmZxJ() ? zxJoymapLookup(h) : ouiKmJoy() ? c64JoymapLookup(h)
       : ouiKmMsx() ? msxKeymapLookup(h, s) : ouiKmA2() ? apple2KeymapLookup(h, s) : ouiKmZx() ? zxKeymapLookup(h, s) : c64KeymapLookup(h, s);
}
static void    ouiKmDescribe(uint8_t c, bool s, char *o, size_t n)
{
  if (ouiKmCol()) colecoKeymapDescribe(c, o, n);
  else if (ouiKmNes()) nesKeymapDescribe(c, o, n);
  else if (ouiKmSms()) smsKeymapDescribe(c, o, n);
  else if (ouiKmAtari()) atariKeymapDescribe(c, o, n);
  else if (ouiKmMsxJ()) msxJoymapDescribe(c, o, n);
  else if (ouiKmA2J()) apple2JoymapDescribe(c, o, n);
  else if (ouiKmZxJ()) zxJoymapDescribe(c, o, n);
  else if (ouiKmJoy()) c64JoymapDescribe(c, o, n);
  else if (ouiKmMsx()) msxKeymapDescribe(c, s, o, n);
  else if (ouiKmA2()) apple2KeymapDescribe(c, o, n);
  else if (ouiKmZx()) zxKeymapDescribe(c, s, o, n);
  else c64KeymapDescribe(c, s, o, n);
}
static bool    ouiKmSet(uint8_t h, bool s, uint8_t c)
{
  return ouiKmCol() ? colecoKeymapSet(h, s, c) : ouiKmNes() ? nesKeymapSet(h, c) : ouiKmSms() ? smsKeymapSet(h, c) : ouiKmAtari() ? atariKeymapSet(h, c)
       : ouiKmMsxJ() ? msxJoymapSet(h, c) : ouiKmA2J() ? apple2JoymapSet(h, c) : ouiKmZxJ() ? zxJoymapSet(h, c) : ouiKmJoy() ? c64JoymapSet(h, c)
       : ouiKmMsx() ? msxKeymapSet(h, s, c) : ouiKmA2() ? apple2KeymapSet(h, s, c) : ouiKmZx() ? zxKeymapSet(h, s, c) : c64KeymapSet(h, s, c);
}
static bool    ouiKmSave()
{
  return ouiKmCol() ? colecoKeymapSave() : ouiKmNes() ? nesKeymapSave() : ouiKmSms() ? smsKeymapSave() : ouiKmAtari() ? atariKeymapSave()
       : ouiKmMsxJ() ? msxJoymapSave() : ouiKmA2J() ? apple2JoymapSave() : ouiKmZxJ() ? zxJoymapSave() : ouiKmJoy() ? c64JoymapSave()
       : ouiKmMsx() ? msxKeymapSave() : ouiKmA2() ? apple2KeymapSave() : ouiKmZx() ? zxKeymapSave() : c64KeymapSave();
}
static void    ouiKmResetAll()
{
  if (ouiKmCol()) colecoKeymapResetAll(); else if (ouiKmNes()) nesKeymapResetAll(); else if (ouiKmSms()) smsKeymapResetAll(); else if (ouiKmAtari()) atariKeymapResetAll();
  else if (ouiKmMsxJ()) msxJoymapResetAll(); else if (ouiKmA2J()) apple2JoymapResetAll(); else if (ouiKmZxJ()) zxJoymapResetAll(); else if (ouiKmJoy()) c64JoymapResetAll();
  else if (ouiKmMsx()) msxKeymapResetAll(); else if (ouiKmA2()) apple2KeymapResetAll(); else if (ouiKmZx()) zxKeymapResetAll(); else c64KeymapResetAll();
}
static int     ouiKmChanged()
{
  return ouiKmCol() ? colecoKeymapChangedCount() : ouiKmNes() ? nesKeymapChangedCount() : ouiKmSms() ? smsKeymapChangedCount() : ouiKmAtari() ? atariKeymapChangedCount()
       : ouiKmMsxJ() ? msxJoymapChangedCount() : ouiKmA2J() ? apple2JoymapChangedCount() : ouiKmZxJ() ? zxJoymapChangedCount() : ouiKmJoy() ? c64JoymapChangedCount()
       : ouiKmMsx() ? msxKeymapChangedCount() : ouiKmA2() ? apple2KeymapChangedCount() : ouiKmZx() ? zxKeymapChangedCount() : c64KeymapChangedCount();
}
// The host keys listed: the joystick's adds SHIFT.
static int     ouiKmHostCount()
{
  return ouiKmA2J() ? apple2JoymapHostCount() : ouiKmA2() ? apple2KeymapHostCount()
       : ouiKmJoyShift() ? c64JoymapHostCount() : c64KeymapHostCount();
}
static uint8_t ouiKmHostAt(int i)
{
  return ouiKmA2J() ? apple2JoymapHostAt(i) : ouiKmA2() ? apple2KeymapHostAt(i)
       : ouiKmJoyShift() ? c64JoymapHostAt(i) : c64KeymapHostAt(i);
}
// Mode 3's list: default, NONE (, RESTORE on the C64), then the C64 keys / the NES or SMS buttons /
// the joystick's directions and FIRE.
static int     ouiKpFixed()    { return ouiKmSimple() || ouiKmZx() || ouiKmMsx() ? 2 : OUI_KP_FIXED; }   // the ZX has no RESTORE
static int     ouiKpCount()    { return ouiKmCol() ? COLK_COUNT : ouiKmNes() ? 8 : ouiKmSms() ? 6 : ouiKmAtari() ? atariKeymapActionCount()
                                     : ouiKmMsxJ() ? msxJoymapActionCount() : ouiKmA2J() ? apple2JoymapActionCount() : ouiKmZxJ() ? zxJoymapActionCount() : ouiKmJoy() ? c64JoymapActionCount()
                                     : ouiKmMsx() ? MSXK_KEYS : ouiKmA2() ? apple2KeymapKeyCount() : ouiKmZx() ? ZXK_KEYS : 64; }
static uint8_t ouiKpAt(int i)
{
  return ouiKmCol() ? colecoKeymapButtonAt(i) : ouiKmNes() ? nesKeymapButtonAt(i) : ouiKmSms() ? smsKeymapButtonAt(i) : ouiKmAtari() ? atariKeymapActionAt(i)
       : ouiKmMsxJ() ? msxJoymapActionAt(i) : ouiKmA2J() ? apple2JoymapActionAt(i) : ouiKmZxJ() ? zxJoymapActionAt(i) : ouiKmJoy() ? c64JoymapActionAt(i)
       : ouiKmMsx() ? msxKeymapMsxAt(i) : ouiKmA2() ? apple2KeymapKeyAt(i) : ouiKmZx() ? zxKeymapZxAt(i) : c64KeymapC64At(i);
}
static const char *ouiKpName(uint8_t k)
{
  return ouiKmCol() ? colecoKeymapButtonName(k) : ouiKmNes() ? nesKeymapButtonName(k) : ouiKmSms() ? smsKeymapButtonName(k) : ouiKmAtari() ? atariKeymapActionName(k)
       : ouiKmMsxJ() ? msxJoymapActionName(k) : ouiKmA2J() ? apple2JoymapActionName(k) : ouiKmZxJ() ? zxJoymapActionName(k) : ouiKmJoy() ? c64JoymapActionName(k)
       : ouiKmMsx() ? msxKeymapMsxName(k) : ouiKmA2() ? apple2KeymapKeyName(k) : ouiKmZx() ? zxKeymapZxName(k) : c64KeymapC64Name(k);
}
// A keyboard rule code presses a key (the C64's matrix, the Apple's keys: index in bits 0-5, what it
// does to SHIFT / CTRL in bits 6-7), not NONE / UNMAP / RESTORE / RESET.
static bool    ouiKmIsKey(uint8_t c) { return ouiKmMsx() ? msxkIsKey(c) : ouiKmA2() ? a2kIsKey(c) : ouiKmZx() ? zxkIsKey(c) : c64kIsMatrix(c); }
// A key code's key and its SHIFT mode, and the reverse (the MSX packs them as mode * 72 + key).
static uint8_t ouiKmIdx(uint8_t c)   { return ouiKmMsx() ? msxkIndex(c) : c64kIndex(c); }
static int     ouiKmShf(uint8_t c)   { return ouiKmMsx() ? msxkShift(c) : c64kShift(c); }
static uint8_t ouiKmMake(uint8_t k, int m) { return ouiKmMsx() ? msxkMake(k, m) : (uint8_t)((m << 6) | k); }
static int     ouiShCount()          { return ouiKmA2() || ouiKmZx() ? 4 : 3; }   // mode 4's choices (the Apple adds +CTRL, the ZX +SYMBOL)
static bool    ouiKpIs(uint8_t c, uint8_t k) { return ouiKmSimple() ? c == k : (ouiKmIsKey(c) && ouiKmIdx(c) == k); }
// What a rule picks: "C64 KEY" / "NES BUTTON" / "SMS BUTTON" / "JOYSTICK".
static const char *ouiKmWhat()
{
  return ouiKmNes() ? "NES BUTTON" : ouiKmSms() ? "SMS BUTTON" : ouiKmCol() ? "COLECO" : ouiKmAtari() ? "ATARI" : ouiKmJoy() ? "JOYSTICK" : ouiKmMsx() ? "MSX KEY" : ouiKmA2() ? "APPLE KEY" : ouiKmZx() ? "ZX KEY" : "C64 KEY";
}
// The pad / joystick rule code c is one of its buttons (not NONE / UNMAP).
static bool    ouiKmIsBtn(uint8_t c)
{
  return ouiKmNes() ? nesKIsButton(c) : ouiKmSms() ? smsKIsButton(c) : ouiKmCol() ? colKIsButton(c) : ouiKmAtari() ? atariKIsAction(c) : ouiKmZxJ() ? zxjIsAction(c) : c64jIsAction(c);
}

// CTRL / ALT arrive as modifiers and are looked up unshifted only (usbkeyboard.cpp): one row each.
// The NES and the joystick have no SHIFT rules: one row per key.
static bool ouiKmHasShifted(uint8_t hid) { return (!ouiKmSimple() || ouiKmCol()) && hid != C64K_HID_CTRL && hid != C64K_HID_ALT && hid != A2K_HID_RALT; }
// Is (hid, shifted) a row of the key list? All of them, or with ouiKmOnly just the changed ones.
static bool ouiKmListed(uint8_t hid, bool shifted)
{
  if (shifted && !ouiKmHasShifted(hid)) return false;
  if (!ouiKmOnly) return true;
  bool over = false;
  ouiKmRule(hid, shifted, &over);
  return over;
}
static int ouiKmRows()
{
  int n = 0;
  for (int i = 0; i < ouiKmHostCount(); i++)
    for (int s = 0; s < 2; s++) n += ouiKmListed(ouiKmHostAt(i), s) ? 1 : 0;
  return n;
}
// Mode 2 key row r (counted past the fixed rows) -> host key; false past the end.
static bool ouiKmAt(int r, uint8_t &hid, bool &shifted)
{
  for (int i = 0; i < ouiKmHostCount(); i++)
    for (int s = 0; s < 2; s++) {
      if (!ouiKmListed(ouiKmHostAt(i), s)) continue;
      if (r-- == 0) { hid = ouiKmHostAt(i); shifted = s; return true; }
    }
  return false;
}
// The mode 2 row of (hid, shifted); -1 when it is not listed.
static int ouiKmRowOf(uint8_t hid, bool shifted)
{
  if (!ouiKmHasShifted(hid)) shifted = false;
  int r = OUI_KM_FIXED;
  for (int i = 0; i < ouiKmHostCount(); i++)
    for (int s = 0; s < 2; s++) {
      if (!ouiKmListed(ouiKmHostAt(i), s)) continue;
      if (ouiKmHostAt(i) == hid && (bool)s == shifted) return r;
      r++;
    }
  return -1;
}
// "SHIFT+F1" etc.
static void ouiKmLabel(uint8_t hid, bool shifted, char *out, size_t n)
{
  char t[16];
  if (ouiIsApple()) apple2JoymapHostName(hid, t, sizeof(t));   // + RALT
  else c64JoymapHostName(hid, t, sizeof(t));   // the C64 page's names, + SHIFT
  snprintf(out, n, "%s%s", shifted ? "SHIFT+" : "", t);
}
#endif

static int ouiCfgCount()
{
  switch (ouiCfgMode) {
    case 1: { int it[5]; return ouiCfgItems(it); }
#if BOARD_INPUT_USB
    case 2: return OUI_KM_FIXED + ouiKmRows();
    case 3: return ouiKpFixed() + ouiKpCount();
    case 4: return ouiKmShift ? ouiShCount() - 1 : ouiShCount();   // a SHIFT+ rule has no "as typed"
    case 5: return 2;
#endif
#if A2CAL_ANALOG
    case OUI_CAL_MODE: return OUI_CAL_ROWS;
#endif
    default: return 0;
  }
}

#if A2CAL_ANALOG
// --- CALIBRATE (Apple II, cfg mode OUI_CAL_MODE): the analog stick's centre and edges ---
// Row 0 walks the wizard (apple2joycal.cpp): start -> let the stick rest, Enter / fire records the
// centre -> move it to every edge, Enter / fire saves. The rows under it show the stick live: its raw
// reading, the ranges in use (or being recorded) and what PDL(0) / PDL(1) read. While the wizard
// runs the stick doesn't move the menu (joystick.cpp / usbgamepad.cpp), and Enter on any row steps it.
static void ouiCalRow(int idx, char *a, size_t na, char *b, size_t nb, uint16_t &bc)
{
  const int step = apple2JoycalStep();
  int x, y, lo, mid, hi;
  apple2JoycalRaw(&x, &y);
  switch (idx) {
    case OUI_CAL_STEP:
      if (step == A2CAL_CENTER)     snprintf(b, nb, "< leave the stick centred, then Enter / fire >");
      else if (step == A2CAL_EDGES) snprintf(b, nb, "< push it to every edge, then Enter / fire >");
      else                          snprintf(b, nb, "< start calibrating >");
      bc = step ? OUI_REBOOT : OUI_ON;
      return;
    case OUI_CAL_RAW:
      snprintf(a, na, "STICK"); snprintf(b, nb, "X %d   Y %d", x, y); bc = OUI_TXT; return;
    case OUI_CAL_X: case OUI_CAL_Y:
      apple2JoycalRange(idx == OUI_CAL_Y, &lo, &mid, &hi);
      snprintf(a, na, idx == OUI_CAL_X ? "X RANGE" : "Y RANGE");
      snprintf(b, nb, "%d  < %d >  %d", lo, mid, hi);
      bc = (step || apple2JoycalCustom()) ? OUI_ON : OUI_LBL;
      return;
    case OUI_CAL_PDL:
      snprintf(a, na, "APPLE");
      snprintf(b, nb, "PDL(0) %d   PDL(1) %d", apple2JoycalPdl(0), apple2JoycalPdl(1));
      bc = OUI_TXT;
      return;
    case OUI_CAL_RESET:
      snprintf(b, nb, apple2JoycalCustom() ? "< forget the calibration >" : "(not calibrated: full range)");
      bc = apple2JoycalCustom() ? OUI_RED : OUI_LBL;
      return;
  }
}

static void ouiCalEnter()
{
  ouiRomMsg[0] = 0;
  const int step = apple2JoycalStep();
  if (step == A2CAL_IDLE && ouiRomSel == OUI_CAL_RESET) {
    if (!apple2JoycalCustom()) return;
    const bool ok = apple2JoycalReset();
    ouiRomSetMsg(ok ? "Calibration forgotten - full range" : "Forgotten, but the file was NOT deleted (SD)", !ok);
    return;
  }
  if (step == A2CAL_IDLE) { apple2JoycalBegin(); ouiCfgGoto(OUI_CAL_MODE, OUI_CAL_STEP); return; }
  const int r = apple2JoycalNext();
  ouiRomSel = OUI_CAL_STEP;
  if (r > 0) {
    const bool ok = apple2JoycalSave();
    ouiRomSetMsg(ok ? "Calibrated - saved" : "Calibrated, but NOT saved (SD)", !ok);
  } else if (r < 0) {
    ouiRomSetMsg("Too little movement - kept the old calibration", true);
  }
  optionsUiDirty = true;
}
#endif

static void ouiCfgRow(int idx, char *a, size_t na, char *b, size_t nb, uint16_t &bc)
{
  if (ouiCfgMode == 1) {
    int it[5], n = ouiCfgItems(it);
    if (idx >= n) return;
    switch (it[idx]) {
#if BOARD_INPUT_USB   // (KEYBOARD / JOYSTICK are only listed with a keyboard)
#if OUI_KV
      case OUI_CFG_KEYB: snprintf(a, na, "KEYBOARD"); snprintf(b, nb, "Remap keys on a picture"); bc = OUI_ON; break;
      case OUI_CFG_JOY:  snprintf(a, na, "JOYSTICK"); snprintf(b, nb, "Keys that steer and fire"); bc = OUI_ON; break;
#else
      case OUI_CFG_KEYB: snprintf(a, na, "KEYBOARD");
                         snprintf(b, nb, ouiKmNes() ? "Remap keys to NES buttons" : ouiKmSms() ? "Remap keys to SMS buttons" : ouiKmCol() ? "Remap keys to the controller" : ouiKmAtari() ? "Remap keys to the joystick"
                                                                               : ouiKmMsx() ? "Remap keys to MSX keys" : ouiKmA2() ? "Remap keys to Apple keys" : ouiKmZx() ? "Remap keys to Spectrum keys" : "Remap keys to C64 keys");
                         bc = OUI_ON; break;
      case OUI_CFG_JOY:  snprintf(a, na, "JOYSTICK"); snprintf(b, nb, "Remap keys to the joystick"); bc = OUI_ON; break;
#endif
#endif
#if A2CAL_ANALOG
      case OUI_CFG_CAL:  snprintf(a, na, "CALIBRATE"); snprintf(b, nb, apple2JoycalCustom() ? "Analog stick: calibrated" : "Analog stick: centre and edges"); bc = OUI_ON; break;
#endif
      case OUI_CFG_ROMS: snprintf(a, na, "ROMS");     snprintf(b, nb, "File each system ROM loads");     bc = OUI_ON; break;
      case OUI_CFG_HELP: snprintf(a, na, "HELP");     snprintf(b, nb, "Keys and controls");              bc = OUI_LBL; break;
    }
    return;
  }
#if A2CAL_ANALOG
  if (ouiCfgMode == OUI_CAL_MODE) { ouiCalRow(idx, a, na, b, nb, bc); return; }
#endif
#if BOARD_INPUT_USB
  if (ouiCfgMode == 2) {
    if (idx == OUI_KM_FIND) { snprintf(b, nb, ouiKmCapture ? "< press the key now >" : "< press a key to find it >"); bc = OUI_REBOOT; return; }
    if (idx == OUI_KM_ONLY) {
      if (ouiKmOnly) snprintf(b, nb, "< showing changed keys only - show all >");
      else           snprintf(b, nb, "< show only changed keys (%d) >", ouiKmChanged());
      bc = OUI_ON; return;
    }
    if (idx == OUI_KM_RESET) { snprintf(b, nb, "< reset every key to the default >"); bc = OUI_RED; return; }
    uint8_t hid; bool sh;
    if (!ouiKmAt(idx - OUI_KM_FIXED, hid, sh)) return;
    ouiKmLabel(hid, sh, a, na);
    bool over;
    const uint8_t c = ouiKmRule(hid, sh, &over);
    if (c == C64K_NONE) {                    // no rule: a SHIFT+ row falls back to the plain one
      if (sh) snprintf(b, nb, "(as without SHIFT)"); else snprintf(b, nb, "(nothing)");
      bc = OUI_LBL;
      return;
    }
    ouiKmDescribe(c, sh, b, nb);
    bc = over ? OUI_ON : OUI_TXT;
    return;
  }
  const uint8_t cur = ouiKmRule(ouiKmHid, ouiKmShift, nullptr);
  if (ouiCfgMode == 3) {
    if (idx == 0) {
      char d[24];
      ouiKmDescribe(ouiKmDefault(ouiKmHid, ouiKmShift), ouiKmShift, d, sizeof(d));
      if (ouiKmDefault(ouiKmHid, ouiKmShift) == C64K_NONE) snprintf(d, sizeof(d), ouiKmShift ? "as without SHIFT" : "nothing");
      snprintf(b, nb, "< use default: %s >", d);
      bc = OUI_REBOOT;
    } else if (idx == 1) { snprintf(b, nb, "NONE  (the key does nothing)"); bc = cur == C64K_UNMAP ? OUI_ON : OUI_TXT; }
    else if (idx < ouiKpFixed()) { snprintf(b, nb, ouiKmA2() ? "RESET" : "RESTORE"); bc = cur == C64K_RESTORE ? OUI_ON : OUI_TXT; }
    else {
      const uint8_t k = ouiKpAt(idx - ouiKpFixed());
      snprintf(b, nb, "%s", ouiKpName(k));
      bc = ouiKpIs(cur, k) ? OUI_ON : OUI_TXT;
    }
    return;
  }
  if (ouiCfgMode == 4) {
    const int m = ouiKmShift ? idx + 1 : idx;
    static const char *const kMode[4] = { "as typed  (SHIFT passes through)", "+SHIFT  (held while the key is down)",
                                          "-SHIFT  (released while the key is down)", "+CTRL  (CTRL held, SHIFT as typed)" };
    static const char *const kZxMode[4] = { "as typed  (CAPS SHIFT passes through)", "+CAPS  (CAPS SHIFT held)",
                                            "-CAPS  (CAPS SHIFT released)", "+SYMBOL  (SYMBOL SHIFT held, no CAPS)" };
    snprintf(b, nb, "%s", (ouiKmZx() ? kZxMode : kMode)[m & 3]);
    bc = (ouiKmIsKey(cur) && ouiKmIdx(cur) == ouiKmKey && ouiKmShf(cur) == m) ? OUI_ON : OUI_TXT;
    return;
  }
  if (ouiCfgMode == 5) {
    if (idx == 0) { snprintf(b, nb, "No - keep my keys"); bc = OUI_TXT; }
    else          { snprintf(b, nb, "Yes - every key back to the default"); bc = OUI_RED; }
  }
#endif
}

#if OUI_KV
// --- KEYBOARD as a picture (PicoCalc): its keyboard on top, the C64's below ---
//   mode 6  pick a PicoCalc key (arrows, or type it); the Shift keys switch to the SHIFT layer
//           (its green labels: F6..F9, Brk, Home, End, PgUp, PgDn, !@#...). The line above shows
//           what the key types, and the C64 picture lights that key (and SHIFT when it is held).
//   mode 7  pick the C64 key it presses (or DEFAULT / NONE below the picture)
//   mode 8  what it does to the C64's SHIFT
// The same rules as the list (modes 2-5, the other boards' KEYBOARD): a SHIFT-layer key edits its SHIFT+
// rule (CTRL / ALT have none), a green label = changed from the default.
// On the NES the lower picture is its controller (mode 7 picks a button, saved at once, no mode 8);
// the NES has no SHIFT, so only the SHIFT layer's own keys (F6..F9, PgUp, ...) are keys there.
// TAB swaps the direction and the pictures: target first, then the keys that press it (ouiKvRev).
#define OUI_KV_PKY   46          // PicoCalc keyboard: 8 rows from here
#define OUI_KV_CKY   186         // C64 keyboard: 5 rows from here
#define OUI_KV_BTNY  270         // DEFAULT / NONE, or the SHIFT choices
#define OUI_KV_PITCH 17
#define OUI_KV_SHIFT 0xF0        // a Shift key: switches the layer
#define OUI_KV_NOSH  0xFF        // .shid: nothing on the SHIFT layer (Shift-F5 is the snapshot page)

struct OuiKvR { int16_t x, y, w, h; };

// 8x8 arrows etc. for the labels the UI font has no character for (label "\x01".."\x09").
static const uint8_t kOuiKvGlyph[9][8] = {
  { 0x00, 0x20, 0x40, 0xFE, 0x40, 0x20, 0x00, 0x00 },   // 1 left
  { 0x00, 0x08, 0x04, 0xFE, 0x04, 0x08, 0x00, 0x00 },   // 2 right
  { 0x10, 0x38, 0x54, 0x10, 0x10, 0x10, 0x10, 0x00 },   // 3 up
  { 0x10, 0x10, 0x10, 0x10, 0x54, 0x38, 0x10, 0x00 },   // 4 down
  { 0x10, 0x38, 0x54, 0x10, 0x54, 0x38, 0x10, 0x00 },   // 5 up-down (CRSR)
  { 0x00, 0x24, 0x42, 0xFF, 0x42, 0x24, 0x00, 0x00 },   // 6 left-right (CRSR)
  { 0x1C, 0x22, 0x20, 0x78, 0x20, 0x20, 0x7E, 0x00 },   // 7 pound
  { 0x08, 0x10, 0x6C, 0x92, 0x82, 0x82, 0x44, 0x38 },   // 8 open apple (the IIe's key)
  { 0x08, 0x10, 0x6C, 0xFE, 0xFE, 0xFE, 0x7C, 0x38 },   // 9 solid apple
};

// The PicoCalc keys: x, row, width, rows; the HID code and the one Shift turns it into (0 = the
// same code with SHIFT down); labels plain / SHIFT layer. hid 0 = not mappable (Caps Lock).
struct OuiKvPk { int16_t x; uint8_t row, w, rows, hid, shid; const char *lbl, *slbl; };
#define KVR(i) (5 + (i) * 31)    // the full-width rows: 10 keys
static const OuiKvPk kOuiKvPk[] = {
  {  35, 0, 29, 1, 0x52, 0x4B, "\x03", "PgU" }, {   4, 1, 29, 1, 0x50, 0, "\x01", 0 },
  {  66, 1, 29, 1, 0x4F, 0,    "\x02", 0 },     {  35, 2, 29, 1, 0x51, 0x4E, "\x04", "PgD" },
  { 100, 0, 41, 1, 0x3A, 0x3F, "F1", "F6" }, { 143, 0, 41, 1, 0x3B, 0x40, "F2", "F7" },
  { 186, 0, 41, 1, 0x45, 0x41, "F3", "F8" }, { 229, 0, 41, 1, 0x3D, 0x42, "F4", "F9" },
  { 272, 0, 41, 1, 0x3E, OUI_KV_NOSH, "F5", "F10" },
  { 100, 1, 41, 1, 0x29, 0x48, "Esc", "Brk" }, { 143, 1, 41, 1, 0x2B, 0x4A, "Tab", "Home" },
  { 186, 1, 41, 1, 0,    0,    "Caps", 0 },    { 229, 1, 41, 1, 0x4C, 0x4D, "Del", "End" },
  { 272, 1, 41, 1, 0x2A, 0,    "Back", 0 },
  { 100, 2, 29, 1, 0x35, 0, "`", "~" },  { 131, 2, 29, 1, 0x38, 0, "/", "?" },
  { 162, 2, 29, 1, 0x31, 0, "\\", "|" }, { 193, 2, 29, 1, 0x2D, 0, "-", "_" },
  { 224, 2, 29, 1, 0x2E, 0, "=", "+" },  { 255, 2, 29, 1, 0x2F, 0, "[", "{" },
  { 286, 2, 29, 1, 0x30, 0, "]", "}" },
  { KVR(0), 3, 29, 1, 0x1E, 0, "1", "!" }, { KVR(1), 3, 29, 1, 0x1F, 0, "2", "@" },
  { KVR(2), 3, 29, 1, 0x20, 0, "3", "#" }, { KVR(3), 3, 29, 1, 0x21, 0, "4", "$" },
  { KVR(4), 3, 29, 1, 0x22, 0, "5", "%" }, { KVR(5), 3, 29, 1, 0x23, 0, "6", "^" },
  { KVR(6), 3, 29, 1, 0x24, 0, "7", "&" }, { KVR(7), 3, 29, 1, 0x25, 0, "8", "*" },
  { KVR(8), 3, 29, 1, 0x26, 0, "9", "(" }, { KVR(9), 3, 29, 1, 0x27, 0, "0", ")" },
  { KVR(0), 4, 29, 1, 0x14, 0, "Q", 0 }, { KVR(1), 4, 29, 1, 0x1A, 0, "W", 0 },
  { KVR(2), 4, 29, 1, 0x08, 0, "E", 0 }, { KVR(3), 4, 29, 1, 0x15, 0, "R", 0 },
  { KVR(4), 4, 29, 1, 0x17, 0, "T", 0 }, { KVR(5), 4, 29, 1, 0x1C, 0, "Y", 0 },
  { KVR(6), 4, 29, 1, 0x18, 0, "U", 0 }, { KVR(7), 4, 29, 1, 0x0C, 0, "I", 0 },
  { KVR(8), 4, 29, 1, 0x12, 0, "O", 0 }, { KVR(9), 4, 29, 1, 0x13, 0, "P", 0 },
  { KVR(0), 5, 29, 1, 0x04, 0, "A", 0 }, { KVR(1), 5, 29, 1, 0x16, 0, "S", 0 },
  { KVR(2), 5, 29, 1, 0x07, 0, "D", 0 }, { KVR(3), 5, 29, 1, 0x09, 0, "F", 0 },
  { KVR(4), 5, 29, 1, 0x0A, 0, "G", 0 }, { KVR(5), 5, 29, 1, 0x0B, 0, "H", 0 },
  { KVR(6), 5, 29, 1, 0x0D, 0, "J", 0 }, { KVR(7), 5, 29, 1, 0x0E, 0, "K", 0 },
  { KVR(8), 5, 29, 1, 0x0F, 0, "L", 0 }, { KVR(9), 5, 29, 2, 0x28, 0, "Ent", 0 },
  { KVR(0), 6, 29, 1, 0x1D, 0, "Z", 0 }, { KVR(1), 6, 29, 1, 0x1B, 0, "X", 0 },
  { KVR(2), 6, 29, 1, 0x06, 0, "C", 0 }, { KVR(3), 6, 29, 1, 0x19, 0, "V", 0 },
  { KVR(4), 6, 29, 1, 0x05, 0, "B", 0 }, { KVR(5), 6, 29, 1, 0x11, 0, "N", 0 },
  { KVR(6), 6, 29, 1, 0x10, 0, "M", 0 }, { KVR(7), 6, 29, 1, 0x36, 0, ",", "<" },
  { KVR(8), 6, 29, 1, 0x37, 0, ".", ">" },
  {   5, 7, 44, 1, OUI_KV_SHIFT, 0, "Shift", 0 }, { 51, 7, 29, 1, C64K_HID_CTRL, 0, "Ctrl", 0 },
  {  82, 7, 29, 1, C64K_HID_ALT, 0, "Alt", 0 },   { 113, 7, 91, 1, 0x2C, 0, "Space", 0 },
  { 206, 7, 29, 1, 0x33, 0, ";", ":" },           { 237, 7, 29, 1, 0x34, 0, "'", "\"" },
  { 268, 7, 44, 1, OUI_KV_SHIFT, 0, "Shift", 0 },
};
#undef KVR
#define OUI_KV_NPK ((int)(sizeof(kOuiKvPk) / sizeof(kOuiKvPk[0])))

// The C64 keys: x and width in half keys (a key = 17 px), row; the matrix index (C64K_RESTORE for
// RESTORE). The F keys stand in a column of their own to the right.
#define OUI_KV_FCOL 1            // .flags: the F-key column
#define OUI_KV_LOCK 2            // SHIFT LOCK: the left SHIFT's line, but not lit as "SHIFT held"
struct OuiKvCk { uint8_t hx, row, hw, code, flags; const char *lbl; };
static const OuiKvCk kOuiKvCk[] = {
  {  0, 0, 2, 57, 0, "\x01" }, {  2, 0, 2, 56, 0, "1" }, {  4, 0, 2, 59, 0, "2" }, {  6, 0, 2,  8, 0, "3" },
  {  8, 0, 2, 11, 0, "4" },    { 10, 0, 2, 16, 0, "5" }, { 12, 0, 2, 19, 0, "6" }, { 14, 0, 2, 24, 0, "7" },
  { 16, 0, 2, 27, 0, "8" },    { 18, 0, 2, 32, 0, "9" }, { 20, 0, 2, 35, 0, "0" }, { 22, 0, 2, 40, 0, "+" },
  { 24, 0, 2, 43, 0, "-" },    { 26, 0, 2, 48, 0, "\x07" }, { 28, 0, 2, 51, 0, "HM" }, { 30, 0, 2, 0, 0, "DL" },
  {  0, 1, 3, 58, 0, "CTL" },  {  3, 1, 2, 62, 0, "Q" }, {  5, 1, 2,  9, 0, "W" }, {  7, 1, 2, 14, 0, "E" },
  {  9, 1, 2, 17, 0, "R" },    { 11, 1, 2, 22, 0, "T" }, { 13, 1, 2, 25, 0, "Y" }, { 15, 1, 2, 30, 0, "U" },
  { 17, 1, 2, 33, 0, "I" },    { 19, 1, 2, 38, 0, "O" }, { 21, 1, 2, 41, 0, "P" }, { 23, 1, 2, 46, 0, "@" },
  { 25, 1, 2, 49, 0, "*" },    { 27, 1, 2, 54, 0, "\x03" }, { 29, 1, 3, C64K_RESTORE, 0, "RST" },
  {  0, 2, 2, 63, 0, "RS" },   {  2, 2, 2, 15, OUI_KV_LOCK, "SL" }, {  4, 2, 2, 10, 0, "A" }, {  6, 2, 2, 13, 0, "S" },
  {  8, 2, 2, 18, 0, "D" },    { 10, 2, 2, 21, 0, "F" }, { 12, 2, 2, 26, 0, "G" }, { 14, 2, 2, 29, 0, "H" },
  { 16, 2, 2, 34, 0, "J" },    { 18, 2, 2, 37, 0, "K" }, { 20, 2, 2, 42, 0, "L" }, { 22, 2, 2, 45, 0, ":" },
  { 24, 2, 2, 50, 0, ";" },    { 26, 2, 2, 53, 0, "=" }, { 28, 2, 4,  1, 0, "RETURN" },
  {  0, 3, 2, 61, 0, "C=" },   {  2, 3, 3, 15, 0, "SHF" }, {  5, 3, 2, 12, 0, "Z" }, {  7, 3, 2, 23, 0, "X" },
  {  9, 3, 2, 20, 0, "C" },    { 11, 3, 2, 31, 0, "V" }, { 13, 3, 2, 28, 0, "B" }, { 15, 3, 2, 39, 0, "N" },
  { 17, 3, 2, 36, 0, "M" },    { 19, 3, 2, 47, 0, "," }, { 21, 3, 2, 44, 0, "." }, { 23, 3, 2, 55, 0, "/" },
  { 25, 3, 3, 52, 0, "SHF" },  { 28, 3, 2,  7, 0, "\x05" }, { 30, 3, 2,  2, 0, "\x06" },
  {  6, 4, 18, 60, 0, "SPACE" },
  {  0, 0, 0,  4, OUI_KV_FCOL, "F1/2" }, { 0, 1, 0, 5, OUI_KV_FCOL, "F3/4" },
  {  0, 2, 0,  6, OUI_KV_FCOL, "F5/6" }, { 0, 3, 0, 3, OUI_KV_FCOL, "F7/8" },
};
#define OUI_KV_NCK ((int)(sizeof(kOuiKvCk) / sizeof(kOuiKvCk[0])))

// The Apple II+ keyboard (apple2keymap.h): as the C64's, a key = 22 px (the II+ has fewer keys a
// row); RESET where the C64 has RESTORE.
#define A2L(c) (uint8_t)(A2K_A + ((c) - 'A'))
static const OuiKvCk kOuiKvA2k[] = {
  {  2, 0, 2, A2K_1,     0, "1!" }, {  4, 0, 2, A2K_1 + 1, 0, "2\"" }, {  6, 0, 2, A2K_1 + 2, 0, "3#" },
  {  8, 0, 2, A2K_1 + 3, 0, "4$" }, { 10, 0, 2, A2K_1 + 4, 0, "5%" },  { 12, 0, 2, A2K_1 + 5, 0, "6&" },
  { 14, 0, 2, A2K_1 + 6, 0, "7'" }, { 16, 0, 2, A2K_1 + 7, 0, "8(" },  { 18, 0, 2, A2K_1 + 8, 0, "9)" },
  { 20, 0, 2, A2K_0,     0, "0" },  { 22, 0, 2, A2K_COLON, 0, ":*" },  { 24, 0, 2, A2K_MINUS, 0, "-=" },
  { 26, 0, 2, A2K_RESET, 0, "RST" },
  {  0, 1, 3, A2K_ESC, 0, "ESC" },
  {  3, 1, 2, A2L('Q'), 0, "Q" }, {  5, 1, 2, A2L('W'), 0, "W" }, {  7, 1, 2, A2L('E'), 0, "E" },
  {  9, 1, 2, A2L('R'), 0, "R" }, { 11, 1, 2, A2L('T'), 0, "T" }, { 13, 1, 2, A2L('Y'), 0, "Y" },
  { 15, 1, 2, A2L('U'), 0, "U" }, { 17, 1, 2, A2L('I'), 0, "I" }, { 19, 1, 2, A2L('O'), 0, "O" },
  { 21, 1, 2, A2L('P'), 0, "P@" }, { 23, 1, 2, A2K_REPT, 0, "RPT" }, { 25, 1, 3, A2K_RETURN, 0, "RET" },
  {  0, 2, 4, A2K_CTRL, 0, "CTRL" },
  {  4, 2, 2, A2L('A'), 0, "A" }, {  6, 2, 2, A2L('S'), 0, "S" }, {  8, 2, 2, A2L('D'), 0, "D" },
  { 10, 2, 2, A2L('F'), 0, "F" }, { 12, 2, 2, A2L('G'), 0, "G" }, { 14, 2, 2, A2L('H'), 0, "H" },
  { 16, 2, 2, A2L('J'), 0, "J" }, { 18, 2, 2, A2L('K'), 0, "K" }, { 20, 2, 2, A2L('L'), 0, "L" },
  { 22, 2, 2, A2K_SEMICOLON, 0, ";+" }, { 24, 2, 2, A2K_LEFT, 0, "\x01" }, { 26, 2, 2, A2K_RIGHT, 0, "\x02" },
  {  0, 3, 5, A2K_LSHIFT, 0, "SHIFT" },
  {  5, 3, 2, A2L('Z'), 0, "Z" }, {  7, 3, 2, A2L('X'), 0, "X" }, {  9, 3, 2, A2L('C'), 0, "C" },
  { 11, 3, 2, A2L('V'), 0, "V" }, { 13, 3, 2, A2L('B'), 0, "B" }, { 15, 3, 2, A2L('N'), 0, "N^" },
  { 17, 3, 2, A2L('M'), 0, "M" }, { 19, 3, 2, A2K_COMMA, 0, ",<" }, { 21, 3, 2, A2K_PERIOD, 0, ".>" },
  { 23, 3, 2, A2K_SLASH, 0, "/?" }, { 25, 3, 3, A2K_RSHIFT, 0, "SHF" },
  {  6, 4, 16, A2K_SPACE, 0, "SPACE" },
};
#undef A2L
#define OUI_KV_NA2K ((int)(sizeof(kOuiKvA2k) / sizeof(kOuiKvA2k[0])))

// The Apple IIe keyboard (the US layout the emulated ROM has; the tall RETURN as on the photo'd
// one): a key = 19 px, 14 keys to the top row; RESET on its own at the top right, as on the case.
#define OUI_KV_TALL 4            // .flags: two rows high (the IIe's RETURN)
#define A2L(c) (uint8_t)(A2K_A + ((c) - 'A'))
static const OuiKvCk kOuiKvA2e[] = {
  {  0, 0, 2, A2K_ESC,   0, "ESC" },
  {  2, 0, 2, A2K_1,     0, "1!" }, {  4, 0, 2, A2K_1 + 1, 0, "2@" }, {  6, 0, 2, A2K_1 + 2, 0, "3#" },
  {  8, 0, 2, A2K_1 + 3, 0, "4$" }, { 10, 0, 2, A2K_1 + 4, 0, "5%" }, { 12, 0, 2, A2K_1 + 5, 0, "6^" },
  { 14, 0, 2, A2K_1 + 6, 0, "7&" }, { 16, 0, 2, A2K_1 + 7, 0, "8*" }, { 18, 0, 2, A2K_1 + 8, 0, "9(" },
  { 20, 0, 2, A2K_0,     0, "0)" }, { 22, 0, 2, A2K_MINUS, 0, "-_" }, { 24, 0, 2, A2K_EQUAL, 0, "=+" },
  { 26, 0, 3, A2K_DELETE, 0, "DEL" },
  {  0, 1, 3, A2K_TAB, 0, "TAB" },
  {  3, 1, 2, A2L('Q'), 0, "Q" }, {  5, 1, 2, A2L('W'), 0, "W" }, {  7, 1, 2, A2L('E'), 0, "E" },
  {  9, 1, 2, A2L('R'), 0, "R" }, { 11, 1, 2, A2L('T'), 0, "T" }, { 13, 1, 2, A2L('Y'), 0, "Y" },
  { 15, 1, 2, A2L('U'), 0, "U" }, { 17, 1, 2, A2L('I'), 0, "I" }, { 19, 1, 2, A2L('O'), 0, "O" },
  { 21, 1, 2, A2L('P'), 0, "P" }, { 23, 1, 2, A2K_LBRACKET, 0, "[{" }, { 25, 1, 2, A2K_RBRACKET, 0, "]}" },
  { 27, 1, 2, A2K_RETURN, OUI_KV_TALL, "RET" },
  {  0, 2, 5, A2K_CTRL, 0, "CTRL" },
  {  5, 2, 2, A2L('A'), 0, "A" }, {  7, 2, 2, A2L('S'), 0, "S" }, {  9, 2, 2, A2L('D'), 0, "D" },
  { 11, 2, 2, A2L('F'), 0, "F" }, { 13, 2, 2, A2L('G'), 0, "G" }, { 15, 2, 2, A2L('H'), 0, "H" },
  { 17, 2, 2, A2L('J'), 0, "J" }, { 19, 2, 2, A2L('K'), 0, "K" }, { 21, 2, 2, A2L('L'), 0, "L" },
  { 23, 2, 2, A2K_SEMICOLON, 0, ";:" }, { 25, 2, 2, A2K_QUOTE, 0, "'\"" },
  {  0, 3, 5, A2K_LSHIFT, 0, "SHIFT" },
  {  5, 3, 2, A2L('Z'), 0, "Z" }, {  7, 3, 2, A2L('X'), 0, "X" }, {  9, 3, 2, A2L('C'), 0, "C" },
  { 11, 3, 2, A2L('V'), 0, "V" }, { 13, 3, 2, A2L('B'), 0, "B" }, { 15, 3, 2, A2L('N'), 0, "N" },
  { 17, 3, 2, A2L('M'), 0, "M" }, { 19, 3, 2, A2K_COMMA, 0, ",<" }, { 21, 3, 2, A2K_PERIOD, 0, ".>" },
  { 23, 3, 2, A2K_SLASH, 0, "/?" }, { 25, 3, 4, A2K_RSHIFT, 0, "SHIFT" },
  {  0, 4, 3, A2K_CAPS, 0, "CAPS" }, {  3, 4, 2, A2K_OAPPLE, 0, "\x08" }, {  5, 4, 2, A2K_GRAVE, 0, "`~" },
  {  7, 4, 10, A2K_SPACE, 0, "" }, { 17, 4, 2, A2K_BACKSLASH, 0, "\\|" }, { 19, 4, 2, A2K_SAPPLE, 0, "\x09" },
  { 21, 4, 2, A2K_LEFT, 0, "\x01" }, { 23, 4, 2, A2K_RIGHT, 0, "\x02" }, { 25, 4, 2, A2K_DOWN, 0, "\x04" },
  { 27, 4, 2, A2K_UP, 0, "\x03" },
  {  0, 0, 0, A2K_RESET, OUI_KV_FCOL, "RESET" },
};
#undef A2L
#define OUI_KV_NA2E ((int)(sizeof(kOuiKvA2e) / sizeof(kOuiKvA2e[0])))

// The ZX Spectrum 48K's rubber keys (zxkeymap.h): x and width in quarter keys (a key = 28 px), four
// rows of 21 px; the label is "key|red SYMBOL SHIFT word" (the red word on top, the key below).
static const OuiKvCk kOuiKvZk[] = {
  {  0, 0, 4, 15, 0, "1|!" },    {  4, 0, 4, 16, 0, "2|@" },    {  8, 0, 4, 17, 0, "3|#" },
  { 12, 0, 4, 18, 0, "4|$" },    { 16, 0, 4, 19, 0, "5|%" },    { 20, 0, 4, 24, 0, "6|&" },
  { 24, 0, 4, 23, 0, "7|'" },    { 28, 0, 4, 22, 0, "8|(" },    { 32, 0, 4, 21, 0, "9|)" },
  { 36, 0, 4, 20, 0, "0|_" },
  {  2, 1, 4, 10, 0, "Q|<=" },   {  6, 1, 4, 11, 0, "W|<>" },   { 10, 1, 4, 12, 0, "E|>=" },
  { 14, 1, 4, 13, 0, "R|<" },    { 18, 1, 4, 14, 0, "T|>" },    { 22, 1, 4, 29, 0, "Y|AND" },
  { 26, 1, 4, 28, 0, "U|OR" },   { 30, 1, 4, 27, 0, "I|AT" },   { 34, 1, 4, 26, 0, "O|;" },
  { 38, 1, 4, 25, 0, "P|\"" },
  {  3, 2, 4,  5, 0, "A|STOP" }, {  7, 2, 4,  6, 0, "S|NOT" },  { 11, 2, 4,  7, 0, "D|STEP" },
  { 15, 2, 4,  8, 0, "F|TO" },   { 19, 2, 4,  9, 0, "G|THEN" }, { 23, 2, 4, 34, 0, "H|^" },
  { 27, 2, 4, 33, 0, "J|-" },    { 31, 2, 4, 32, 0, "K|+" },    { 35, 2, 4, 31, 0, "L|=" },
  { 39, 2, 5, 30, 0, "ENTER|" },
  {  0, 3, 6, ZXK_CAPS_INDEX, 0, "SHIFT|CAPS" },
  {  6, 3, 4,  1, 0, "Z|:" },    { 10, 3, 4,  2, 0, "X|\x07" }, { 14, 3, 4,  3, 0, "C|?" },
  { 18, 3, 4,  4, 0, "V|/" },    { 22, 3, 4, 39, 0, "B|*" },    { 26, 3, 4, 38, 0, "N|," },
  { 30, 3, 4, 37, 0, "M|." },    { 34, 3, 4, ZXK_SYM_INDEX, 0, "SHFT|SYMB" },
  { 38, 3, 6, 35, 0, "SPACE|BREAK" },
};
#define OUI_KV_NZK ((int)(sizeof(kOuiKvZk) / sizeof(kOuiKvZk[0])))
#define OUI_KV_ZXX   6           // the ZX keys' left edge
#define OUI_KV_ZXQ   7           // a quarter key, px
#define OUI_KV_ZXR   21          // the ZX keys' row pitch
// The MSX keyboard (after the Philips VG-8020: the F1-F5 / SELECT / STOP row with HOME INS DEL at
// its right, the tall RETURN, CAPS GRPH SPACE CODE, the cursor diamond on the right): six rows, so
// a row is 14 px and a key 12 high (OUI_KV_TALL: two rows); a key = 17 px as on the C64. The code
// is the matrix index, row * 8 + bit (msxkeymap.h).
#define OUI_KV_TALL 4            // .flags: two rows high (RETURN)
#define OUI_KV_MSXPITCH 14
#define MXL(c) (uint8_t)(22 + ((c) - 'A'))
static const OuiKvCk kOuiKvMk[] = {
  {  0, 0, 4, 53, 0, "F1/6" }, {  4, 0, 4, 54, 0, "F2/7" }, {  8, 0, 4, 55, 0, "F3/8" }, { 12, 0, 4, 56, 0, "F4/9" },
  { 16, 0, 4, 57, 0, "F5/10" }, { 21, 0, 4, 62, 0, "SEL" }, { 25, 0, 4, 60, 0, "STOP" },
  { 30, 0, 2, 65, 0, "HM" },   { 32, 0, 2, 66, 0, "IN" },  { 34, 0, 2, 67, 0, "DL" },
  {  0, 1, 2, 58, 0, "ES" },   {  2, 1, 2,  1, 0, "1" },   {  4, 1, 2,  2, 0, "2" },   {  6, 1, 2,  3, 0, "3" },
  {  8, 1, 2,  4, 0, "4" },    { 10, 1, 2,  5, 0, "5" },   { 12, 1, 2,  6, 0, "6" },   { 14, 1, 2,  7, 0, "7" },
  { 16, 1, 2,  8, 0, "8" },    { 18, 1, 2,  9, 0, "9" },   { 20, 1, 2,  0, 0, "0" },   { 22, 1, 2, 10, 0, "-" },
  { 24, 1, 2, 11, 0, "=" },    { 26, 1, 2, 12, 0, "\\" },  { 28, 1, 2, 61, 0, "BS" },
  {  0, 2, 3, 59, 0, "TAB" },  {  3, 2, 2, MXL('Q'), 0, "Q" }, {  5, 2, 2, MXL('W'), 0, "W" }, {  7, 2, 2, MXL('E'), 0, "E" },
  {  9, 2, 2, MXL('R'), 0, "R" }, { 11, 2, 2, MXL('T'), 0, "T" }, { 13, 2, 2, MXL('Y'), 0, "Y" }, { 15, 2, 2, MXL('U'), 0, "U" },
  { 17, 2, 2, MXL('I'), 0, "I" }, { 19, 2, 2, MXL('O'), 0, "O" }, { 21, 2, 2, MXL('P'), 0, "P" }, { 23, 2, 2, 13, 0, "[" },
  { 25, 2, 2, 14, 0, "]" },    { 27, 2, 3, 63, OUI_KV_TALL, "RET" },
  {  0, 3, 3, 49, 0, "CTL" },  {  3, 3, 2, MXL('A'), 0, "A" }, {  5, 3, 2, MXL('S'), 0, "S" }, {  7, 3, 2, MXL('D'), 0, "D" },
  {  9, 3, 2, MXL('F'), 0, "F" }, { 11, 3, 2, MXL('G'), 0, "G" }, { 13, 3, 2, MXL('H'), 0, "H" }, { 15, 3, 2, MXL('J'), 0, "J" },
  { 17, 3, 2, MXL('K'), 0, "K" }, { 19, 3, 2, MXL('L'), 0, "L" }, { 21, 3, 2, 15, 0, ";" },   { 23, 3, 2, 16, 0, "'" },
  { 25, 3, 2, 17, 0, "`" },
  {  0, 4, 4, 48, 0, "SHIFT" }, {  4, 4, 2, MXL('Z'), 0, "Z" }, {  6, 4, 2, MXL('X'), 0, "X" }, {  8, 4, 2, MXL('C'), 0, "C" },
  { 10, 4, 2, MXL('V'), 0, "V" }, { 12, 4, 2, MXL('B'), 0, "B" }, { 14, 4, 2, MXL('N'), 0, "N" }, { 16, 4, 2, MXL('M'), 0, "M" },
  { 18, 4, 2, 18, 0, "," },    { 20, 4, 2, 19, 0, "." },   { 22, 4, 2, 20, 0, "/" },
  { 24, 4, 4, 48, 0, "SHIFT" }, { 28, 4, 2, 21, 0, "DK" },
  {  0, 5, 4, 51, 0, "CAPS" }, {  4, 5, 4, 50, 0, "GRPH" }, {  8, 5, 16, 64, 0, "SPACE" }, { 24, 5, 4, 52, 0, "CODE" },
  { 32, 3, 2, 69, 0, "\x03" }, { 30, 4, 2, 68, 0, "\x01" }, { 34, 4, 2, 71, 0, "\x02" }, { 32, 5, 2, 70, 0, "\x04" },
};
#undef MXL
#define OUI_KV_NMK ((int)(sizeof(kOuiKvMk) / sizeof(kOuiKvMk[0])))
#define OUI_KV_NCKMAXE (OUI_KV_NCK > OUI_KV_NA2K ? OUI_KV_NCK : OUI_KV_NA2K)
#define OUI_KV_NCKMAX0 (OUI_KV_NCKMAXE > OUI_KV_NA2E ? OUI_KV_NCKMAXE : OUI_KV_NA2E)
#define OUI_KV_NCKMAX (OUI_KV_NCKMAX0 > OUI_KV_NMK ? OUI_KV_NCKMAX0 : OUI_KV_NMK)
// The keyboard picture of the page: the C64's, the Apple's or the Spectrum's.
static const OuiKvCk *ouiKvCkTab() { return ouiKmMsx() ? kOuiKvMk : ouiKmA2() ? (AppleIIe ? kOuiKvA2e : kOuiKvA2k) : ouiKmZx() ? kOuiKvZk : kOuiKvCk; }
static int            ouiKvCkN()   { return ouiKmMsx() ? OUI_KV_NMK : ouiKmA2() ? (AppleIIe ? OUI_KV_NA2E : OUI_KV_NA2K) : ouiKmZx() ? OUI_KV_NZK : OUI_KV_NCK; }

// The NES controller: the D-pad, SELECT / START pills, B / A buttons; the NES button (neskeymap.h).
// Moves are a table ({L, R, U, D}; 8 / 9 = the DEFAULT / NONE buttons below): the pad is too sparse
// for the nearest-key walk.
#define OUI_KV_NESY 186          // the controller's top
enum { OUI_NK_PAD, OUI_NK_PILL, OUI_NK_ROUND, OUI_NK_KEY };   // KEY: a key with its label
struct OuiKvNk { int16_t x, y; uint8_t w, h, code, shape; const char *lbl; int8_t mv[4]; };
static const OuiKvNk kOuiKvNk[] = {
  {  51, 199, 18, 18, NESK_UP,     OUI_NK_PAD,   "\x03", { 2, 3, 0, 1 } },
  {  51, 235, 18, 18, NESK_DOWN,   OUI_NK_PAD,   "\x04", { 2, 3, 0, 8 } },
  {  33, 217, 18, 18, NESK_LEFT,   OUI_NK_PAD,   "\x01", { 2, 3, 0, 1 } },
  {  69, 217, 18, 18, NESK_RIGHT,  OUI_NK_PAD,   "\x02", { 2, 4, 0, 1 } },
  { 116, 232, 36, 12, NESK_SELECT, OUI_NK_PILL,  "",     { 3, 5, 4, 8 } },
  { 168, 232, 36, 12, NESK_START,  OUI_NK_PILL,  "",     { 4, 6, 5, 9 } },
  { 230, 214, 26, 26, NESK_B,      OUI_NK_ROUND, "",     { 5, 7, 6, 9 } },
  { 272, 214, 26, 26, NESK_A,      OUI_NK_ROUND, "",     { 6, 7, 7, 9 } },
};
#define OUI_KV_NNK ((int)(sizeof(kOuiKvNk) / sizeof(kOuiKvNk[0])))
static const int8_t kOuiKvNkBtnMv[2][4] = { { 8, 9, 1, 8 }, { 8, 9, 6, 9 } };   // DEFAULT, NONE

// The C64 joystick (a Competition Pro): the four directions around the stick, the red buttons in
// the base's top corners - FIRE on the left, FIRE2 on the right - and a small FIRE3 at the bottom
// left; the lines (c64joymap.h). Moves as for the NES ({L, R, U, D}; 7 / 8 = DEFAULT / NONE).
static const OuiKvNk kOuiKvJk[] = {
  { 152, 203, 16, 14, C64J_UP,                OUI_NK_PAD,   "\x03", { 1, 2, 0, 3 } },
  { 124, 224, 16, 14, C64J_LEFT,              OUI_NK_PAD,   "\x01", { 4, 2, 0, 3 } },
  { 180, 224, 16, 14, C64J_RIGHT,             OUI_NK_PAD,   "\x02", { 1, 5, 0, 3 } },
  { 152, 245, 16, 14, C64J_DOWN,              OUI_NK_PAD,   "\x04", { 6, 2, 0, 7 } },
  {  86, 192, 24, 24, C64J_FIRE,              OUI_NK_ROUND, "",      { 4, 0, 4, 6 } },
  { 210, 192, 24, 24, C64J_FIRE2,             OUI_NK_ROUND, "",      { 0, 5, 5, 2 } },
  {  92, 244, 16, 16, C64J_FIRE3,             OUI_NK_ROUND, "",      { 6, 3, 4, 7 } },
};
#define OUI_KV_NJK ((int)(sizeof(kOuiKvJk) / sizeof(kOuiKvJk[0])))
static const int8_t kOuiKvJkBtnMv[2][4] = { { 7, 8, 6, 7 }, { 7, 8, 3, 8 } };   // DEFAULT, NONE

// The SMS control pad (after the picture: a black pad, the red-framed CONTROL PAD panel with the
// grey square D-pad on the left, SEGA in the middle, the "1 START" and "2" buttons on the right);
// the pad button (smskeymap.h). Moves as for the NES ({L, R, U, D}; 6 / 7 = DEFAULT / NONE).
static const OuiKvNk kOuiKvSk[] = {
  {  55, 206, 18, 18, SMSK_UP,    OUI_NK_PAD,   "\x03", { 2, 3, 0, 1 } },
  {  55, 242, 18, 18, SMSK_DOWN,  OUI_NK_PAD,   "\x04", { 2, 3, 0, 6 } },
  {  37, 224, 18, 18, SMSK_LEFT,  OUI_NK_PAD,   "\x01", { 2, 3, 0, 1 } },
  {  73, 224, 18, 18, SMSK_RIGHT, OUI_NK_PAD,   "\x02", { 2, 4, 0, 1 } },
  { 212, 220, 30, 30, SMSK_1,     OUI_NK_ROUND, "",     { 3, 5, 4, 7 } },
  { 268, 220, 30, 30, SMSK_2,     OUI_NK_ROUND, "",     { 4, 5, 5, 7 } },
};
#define OUI_KV_NSK ((int)(sizeof(kOuiKvSk) / sizeof(kOuiKvSk[0])))
static const int8_t kOuiKvSkBtnMv[2][4] = { { 6, 7, 1, 6 }, { 6, 7, 4, 7 } };   // DEFAULT, NONE

// The ColecoVision hand controller, on its side (after the picture: the black body, the silver
// disc of the stick in its black collar on the left, the grey side buttons at the two ends, COLECO
// in the middle, the 12-key keypad on the right - white keys, black '*' / '#'); the controller
// button (colecokeymap.h). Moves as for the NES ({L, R, U, D}; 18 / 19 = DEFAULT / NONE).
static const OuiKvNk kOuiKvOk[] = {
  {  51, 189, 18, 18, COLK_UP,       OUI_NK_PAD, "\x03", {  2,  3,  0,  1 } },
  {  51, 245, 18, 18, COLK_DOWN,     OUI_NK_PAD, "\x04", {  2,  3,  0, 18 } },
  {  20, 217, 18, 18, COLK_LEFT,     OUI_NK_PAD, "\x01", {  4,  3,  0,  1 } },
  {  82, 217, 18, 18, COLK_RIGHT,    OUI_NK_PAD, "\x02", {  2,  6,  0,  1 } },
  {   2, 208, 12, 36, COLK_LFIRE,    OUI_NK_KEY, "L",    {  4,  2,  4, 18 } },
  { 306, 208, 12, 36, COLK_RFIRE,    OUI_NK_KEY, "R",    {  8,  5,  5, 19 } },
  { 196, 189, 30, 17, COLK_KEY0 + 1, OUI_NK_KEY, "1",    {  3,  7,  6,  9 } },
  { 232, 189, 30, 17, COLK_KEY0 + 2, OUI_NK_KEY, "2",    {  6,  8,  7, 10 } },
  { 268, 189, 30, 17, COLK_KEY0 + 3, OUI_NK_KEY, "3",    {  7,  5,  8, 11 } },
  { 196, 208, 30, 17, COLK_KEY0 + 4, OUI_NK_KEY, "4",    {  3, 10,  6, 12 } },
  { 232, 208, 30, 17, COLK_KEY0 + 5, OUI_NK_KEY, "5",    {  9, 11,  7, 13 } },
  { 268, 208, 30, 17, COLK_KEY0 + 6, OUI_NK_KEY, "6",    { 10,  5,  8, 14 } },
  { 196, 227, 30, 17, COLK_KEY0 + 7, OUI_NK_KEY, "7",    {  3, 13,  9, 15 } },
  { 232, 227, 30, 17, COLK_KEY0 + 8, OUI_NK_KEY, "8",    { 12, 14, 10, 16 } },
  { 268, 227, 30, 17, COLK_KEY0 + 9, OUI_NK_KEY, "9",    { 13,  5, 11, 17 } },
  { 196, 246, 30, 17, COLK_STAR,     OUI_NK_KEY, "*",    {  3, 16, 12, 18 } },
  { 232, 246, 30, 17, COLK_KEY0 + 0, OUI_NK_KEY, "0",    { 15, 17, 13, 19 } },
  { 268, 246, 30, 17, COLK_HASH,     OUI_NK_KEY, "#",    { 16,  5, 14, 19 } },
};
#define OUI_KV_NOK ((int)(sizeof(kOuiKvOk) / sizeof(kOuiKvOk[0])))
static const int8_t kOuiKvOkBtnMv[2][4] = { { 18, 19, 1, 18 }, { 18, 19, 16, 19 } };   // DEFAULT, NONE

// The Atari 2600 (after the picture: the CX40 joystick - a square black base, the red FIRE button
// in its top-left corner, the orange ring around the stick - and the console's GAME SELECT / GAME
// RESET levers on the right); the action (atarikeymap.h). Moves as for the NES ({L, R, U, D};
// 7 / 8 = DEFAULT / NONE).
static const OuiKvNk kOuiKvAk[] = {
  { 142, 192, 16, 14, ATARIK_UP,                  OUI_NK_PAD,   "\x03", { 4, 2, 0, 3 } },
  { 115, 219, 16, 14, ATARIK_LEFT,                OUI_NK_PAD,   "\x01", { 4, 2, 0, 3 } },
  { 169, 219, 16, 14, ATARIK_RIGHT,               OUI_NK_PAD,   "\x02", { 1, 5, 0, 3 } },
  { 142, 246, 16, 14, ATARIK_DOWN,                OUI_NK_PAD,   "\x04", { 1, 2, 0, 7 } },
  { 112, 188, 14, 14, ATARIK_FIRE,                OUI_NK_ROUND, "",       { 4, 0, 4, 1 } },
  { 236, 202, 14, 30, ATARIK_SELECT,              OUI_NK_PILL,  "",       { 2, 6, 5, 8 } },
  { 280, 202, 14, 30, ATARIK_RESET,               OUI_NK_PILL,  "",       { 5, 6, 6, 8 } },
};
#define OUI_KV_NAK ((int)(sizeof(kOuiKvAk) / sizeof(kOuiKvAk[0])))
static const int8_t kOuiKvAkBtnMv[2][4] = { { 7, 8, 3, 7 }, { 7, 8, 5, 8 } };   // DEFAULT, NONE

// The Apple joystick (after the picture: a beige square box, the orange button 0 on its left side
// and button 1 on its back edge, the stick in a recessed square well at the front right); the
// action (apple2joymap.h). Moves as for the NES ({L, R, U, D}; 6 / 7 = DEFAULT / NONE).
static const OuiKvNk kOuiKvA2j[] = {
  { 184, 199, 16, 14, A2J_UP,               OUI_NK_PAD,   "\x03", { 5, 2, 0, 3 } },
  { 158, 221, 16, 14, A2J_LEFT,             OUI_NK_PAD,   "\x01", { 4, 2, 0, 3 } },
  { 210, 221, 16, 14, A2J_RIGHT,            OUI_NK_PAD,   "\x02", { 1, 2, 0, 3 } },
  { 184, 243, 16, 14, A2J_DOWN,             OUI_NK_PAD,   "\x04", { 1, 2, 0, 6 } },
  {  86, 214, 22, 22, A2J_BTN0,             OUI_NK_ROUND, "",       { 4, 1, 5, 6 } },
  { 110, 192, 28, 12, A2J_BTN1,             OUI_NK_PILL,  "",       { 5, 0, 5, 4 } },
};
#define OUI_KV_NA2J ((int)(sizeof(kOuiKvA2j) / sizeof(kOuiKvA2j[0])))
static const int8_t kOuiKvA2jBtnMv[2][4] = { { 6, 7, 4, 6 }, { 6, 7, 3, 7 } };   // DEFAULT, NONE

// The MSX joystick (after the picture: a black square base, the black stick with its orange top,
// the orange trigger A in the base's front-left corner; trigger B, which most MSX sticks lack,
// small behind it); the lines (msxjoymap.h). Moves as for the NES ({L, R, U, D}; 6 / 7 = DEFAULT
// / NONE).
static const OuiKvNk kOuiKvMj[] = {
  { 152, 192, 16, 14, C64J_UP,                OUI_NK_PAD,   "\x03", { 5, 2, 0, 3 } },
  { 125, 219, 16, 14, C64J_LEFT,              OUI_NK_PAD,   "\x01", { 4, 2, 0, 3 } },
  { 179, 219, 16, 14, C64J_RIGHT,             OUI_NK_PAD,   "\x02", { 1, 2, 0, 3 } },
  { 152, 246, 16, 14, C64J_DOWN,              OUI_NK_PAD,   "\x04", { 4, 2, 0, 6 } },
  { 112, 244, 20, 20, MSXJ_TRIG_A,            OUI_NK_ROUND, "",       { 4, 3, 5, 6 } },
  { 114, 190, 14, 14, MSXJ_TRIG_B,            OUI_NK_ROUND, "",       { 5, 0, 5, 4 } },
};
#define OUI_KV_NMJ ((int)(sizeof(kOuiKvMj) / sizeof(kOuiKvMj[0])))
static const int8_t kOuiKvMjBtnMv[2][4] = { { 6, 7, 4, 6 }, { 6, 7, 3, 7 } };   // DEFAULT, NONE

// The ZX Spectrum's Kempston joystick, drawn as a Cheetah 125+ from above: the black base with a red
// fire button at each side, the grip in the middle with the red trigger on top (all three FIRE),
// the four directions round the grip; the lines (zxjoymap.h). Moves as for the NES ({L, R, U, D};
// 7 / 8 = DEFAULT / NONE).
static const OuiKvNk kOuiKvZj[] = {
  { 162, 197, 16, 14, ZXJ_UP,               OUI_NK_PAD,   "\x03", {  1,  2,  0,  4 } },
  { 134, 219, 16, 14, ZXJ_LEFT,             OUI_NK_PAD,   "\x01", {  5,  4,  0,  3 } },
  { 190, 219, 16, 14, ZXJ_RIGHT,            OUI_NK_PAD,   "\x02", {  4,  6,  0,  3 } },
  { 162, 241, 16, 14, ZXJ_DOWN,             OUI_NK_PAD,   "\x04", {  1,  2,  4,  7 } },
  { 163, 219, 14, 14, ZXJ_FIRE,             OUI_NK_ROUND, "",       {  1,  2,  0,  3 } },
  {  96, 215, 22, 22, ZXJ_FIRE,             OUI_NK_ROUND, "",       {  5,  1,  0,  7 } },
  { 222, 215, 22, 22, ZXJ_FIRE,             OUI_NK_ROUND, "",       {  2,  6,  0,  8 } },
};
#define OUI_KV_NZJ ((int)(sizeof(kOuiKvZj) / sizeof(kOuiKvZj[0])))
static const int8_t kOuiKvZjBtnMv[2][4] = { { 7, 8, 5, 7 }, { 7, 8, 6, 8 } };   // DEFAULT, NONE

// TAB swaps the direction (ouiKvRev) and the pictures with it: the target picture (C64 keys / NES
// controller / joystick) on top, the PicoCalc keyboard below. Then mode 6 picks the target, mode 7
// the PicoCalc key to give it (Enter on a key that already has it takes it away), mode 8 the SHIFT.
#define OUI_KV_PKY2  132         // the PicoCalc keyboard's top when swapped
static int     ouiKvPkSel = 0;          // the PicoCalc key under the cursor (mode 6; swapped: mode 7)
static bool    ouiKvLayer = false;      // the SHIFT layer is showing
static int     ouiKvCkSel = 0;          // the target under the cursor (mode 7, or OUI_KV_NCK / +1 = DEFAULT / NONE;
                                        // swapped: mode 6, and the target being given in modes 7-8)
static int     ouiKvShSel = 0;          // mode 8: the SHIFT button
static int     ouiKvNavX = -1;          // x kept across Up / Down, so a column walk does not drift
static bool    ouiKvRev = false;        // swapped: target first, then the keys
// SPACE marks PicoCalc keys (per layer). Forward: the target picked goes to every marked key.
// Swapped: the keys that press the target start marked; Esc gives / takes it as marked
// (ouiKvMk0 = the marks as they started).
static bool    ouiKvMk[2][OUI_KV_NPK], ouiKvMk0[2][OUI_KV_NPK];
static uint32_t ouiKvSigPk[OUI_KV_NPK], ouiKvSigCk[OUI_KV_NCKMAX], ouiKvSigInfo, ouiKvSigBtn, ouiKvSigBody, ouiKvSigDir;

static void ouiKvInvalidate()
{
  memset(ouiKvSigPk, 0, sizeof(ouiKvSigPk)); memset(ouiKvSigCk, 0, sizeof(ouiKvSigCk));
  ouiKvSigInfo = ouiKvSigBtn = ouiKvSigBody = ouiKvSigDir = OUI_SIG_NONE;
}

// Is the cursor on the PicoCalc keyboard (else on the target picture)?
static bool ouiKvOnHost() { return ouiKvRev ? ouiCfgMode == 7 : ouiCfgMode == 6; }
static bool ouiKvOnTgt()  { return ouiKvRev ? ouiCfgMode == 6 : ouiCfgMode == 7; }
static int  ouiKvPkY()    { return ouiKvRev ? OUI_KV_PKY2 : OUI_KV_PKY; }
static int  ouiKvTy()     { return ouiKvRev ? OUI_KV_PKY - OUI_KV_CKY : 0; }   // the target picture's shift

// The lower picture's entries: the C64 keys, or the NES buttons / the joystick (a table each).
static const OuiKvNk *ouiKvTab() { return ouiKmMsxJ() ? kOuiKvMj : ouiKmA2J() ? kOuiKvA2j : ouiKmZxJ() ? kOuiKvZj : ouiKmJoy() ? kOuiKvJk : ouiKmSms() ? kOuiKvSk : ouiKmCol() ? kOuiKvOk : ouiKmAtari() ? kOuiKvAk : kOuiKvNk; }
static int ouiKvNck()
{
  return ouiKmNes() ? OUI_KV_NNK : ouiKmSms() ? OUI_KV_NSK : ouiKmCol() ? OUI_KV_NOK : ouiKmAtari() ? OUI_KV_NAK : ouiKmMsxJ() ? OUI_KV_NMJ : ouiKmA2J() ? OUI_KV_NA2J : ouiKmZxJ() ? OUI_KV_NZJ : ouiKmJoy() ? OUI_KV_NJK : ouiKvCkN();
}

static void ouiKvPkRect(int i, OuiKvR &r)
{
  const OuiKvPk &k = kOuiKvPk[i];
  r.x = k.x; r.y = ouiKvPkY() + k.row * OUI_KV_PITCH; r.w = k.w; r.h = k.rows * OUI_KV_PITCH - 2;
}
static int ouiKvBtnCount() { return ouiCfgMode == 8 ? (ouiKmShift ? ouiShCount() - 1 : ouiShCount()) : 2; }
static void ouiKvBtnRect(int b, OuiKvR &r)
{
  const int n = ouiKvBtnCount(), w = (312 - (n - 1) * 4) / n;
  r.x = 4 + b * (w + 4); r.y = OUI_KV_BTNY; r.w = w; r.h = 15;
}
static void ouiKvCkRect(int i, OuiKvR &r)   // i >= ouiKvNck(): the buttons below
{
  if (i >= ouiKvNck()) { ouiKvBtnRect(i - ouiKvNck(), r); return; }
  if (ouiKmSimple()) { const OuiKvNk &n = ouiKvTab()[i]; r.x = n.x; r.y = n.y + ouiKvTy(); r.w = n.w; r.h = n.h; return; }
  const OuiKvCk &k = ouiKvCkTab()[i];
  if (ouiKmMsx()) {   // six rows of 14 px, a key = 17 px
    r.y = OUI_KV_CKY + ouiKvTy() + k.row * OUI_KV_MSXPITCH; r.h = k.flags & OUI_KV_TALL ? 2 * OUI_KV_MSXPITCH - 2 : 12;
    r.x = 4 + k.hx * 17 / 2; r.w = k.hw * 17 / 2 - 2;
    return;
  }
  if (ouiKmZx()) {
    r.x = OUI_KV_ZXX + k.hx * OUI_KV_ZXQ; r.w = k.hw * OUI_KV_ZXQ - 2;
    r.y = OUI_KV_CKY + ouiKvTy() + k.row * OUI_KV_ZXR; r.h = OUI_KV_ZXR - 2;
    return;
  }
  r.y = OUI_KV_CKY + ouiKvTy() + k.row * OUI_KV_PITCH; r.h = k.row == 4 ? 12 : 15;
  if (k.flags & OUI_KV_TALL) r.h += OUI_KV_PITCH;
  if (k.flags & OUI_KV_FCOL) { r.x = 282; r.w = 34; return; }
  const int kw = ouiKmA2() ? (AppleIIe ? 19 : 22) : 17;   // a key's width
  r.x = 4 + k.hx * kw / 2; r.w = k.hw * kw / 2 - 2;
}
static bool ouiKvPkOk(int i) { return kOuiKvPk[i].hid != 0; }
static bool ouiKvAnyOk(int) { return true; }

// The rule PicoCalc key i edits on `layer`; false for a key with none (Shift, Caps, Shift-F5).
static bool ouiKvTarget(int i, bool layer, uint8_t &hid, bool &sh)
{
  const OuiKvPk &k = kOuiKvPk[i];
  if (k.hid == OUI_KV_SHIFT && ouiKmJoyShift()) { hid = C64K_HID_SHIFT; sh = false; return true; }
  if (!k.hid || k.hid == OUI_KV_SHIFT || (layer && k.shid == OUI_KV_NOSH)) return false;
  if (layer && !k.shid && ouiKmSimple() && !ouiKmCol()) return false;   // the NES / joystick have no SHIFT+ rules
  hid = layer && k.shid ? k.shid : k.hid;
  sh  = layer && ouiKmHasShifted(hid);
  return true;
}

// The arrow keys over a keyboard picture: Left / Right stay in the row, Up / Down go to the
// nearest row that way, under the column the walk started in.
static int ouiKvMove(int n, void (*rf)(int, OuiKvR &), bool (*ok)(int), int cur, int dx, int dy)
{
  OuiKvR c; rf(cur, c);
  const int ccx = c.x + c.w / 2, ccy = c.y + c.h / 2;
  const int cx = (dy && ouiKvNavX >= 0) ? ouiKvNavX : ccx;
  int best = -1; long bs = 0;
  for (int i = 0; i < n; i++) {
    if (i == cur || !ok(i)) continue;
    OuiKvR k; rf(i, k);
    const int kx = k.x + k.w / 2;
    long s;
    if (dx) {
      if (k.y >= c.y + c.h || k.y + k.h <= c.y) continue;      // another row
      const int d = (kx - ccx) * dx;
      if (d <= 0) continue;
      s = (long)d * 64 + abs(k.y + k.h / 2 - ccy);
    } else {
      const int gap = dy > 0 ? k.y - (c.y + c.h) : c.y - (k.y + k.h);
      if (gap < 0) continue;
      const int ox = cx < k.x ? k.x - cx : cx >= k.x + k.w ? cx - (k.x + k.w - 1) : 0;
      s = (long)gap * 65536L + ox * 64L + abs(kx - cx);
    }
    if (best < 0 || s < bs) { best = i; bs = s; }
  }
  if (best < 0) return cur;
  ouiKvNavX = dy ? cx : -1;
  return best;
}

static void ouiKvKey(const OuiKvR &r, const char *lbl, uint16_t bg, uint16_t fg)
{
  tft.fillRoundRect(r.x, r.y, r.w, r.h, 3, bg);
  if ((uint8_t)lbl[0] >= 1 && (uint8_t)lbl[0] <= 9 && !lbl[1]) {
    tft.drawGlyph8(r.x + (r.w - 8) / 2, r.y + (r.h - 8) / 2, 8, 8, kOuiKvGlyph[lbl[0] - 1], fg, bg, true);
    return;
  }
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(lbl, r.x + r.w / 2, r.y + r.h / 2, 1);
}

// The NES controller's body: drawn once; the buttons go on top (ouiKvNesBtn).
static void ouiKvNesBody()
{
  const uint16_t grey = tft.color565(196, 196, 196), dark = tft.color565(30, 30, 30);
  const uint16_t stripe = tft.color565(150, 150, 150), red = tft.color565(210, 40, 45);
  const int o = ouiKvTy(), y = OUI_KV_NESY + o;
  tft.fillRect(0, y - 2, 320, OUI_KV_BTNY - OUI_KV_NESY, OUI_BG);
  tft.fillRoundRect(6, y, 308, 80, 6, grey);
  tft.fillRoundRect(14, y + 8, 292, 66, 4, dark);
  // the D-pad's rim and centre
  const uint16_t rim = tft.color565(70, 70, 70);
  tft.fillRect(49, 197 + o, 22, 58, rim); tft.fillRect(31, 215 + o, 58, 22, rim);
  tft.fillRect(51, 217 + o, 18, 18, tft.color565(24, 24, 24));
  // the middle: stripes, the red SELECT / START, the light box the pills sit in
  tft.fillRoundRect(108, 198 + o, 104, 5, 2, stripe);
  tft.fillRoundRect(108, 207 + o, 104, 5, 2, stripe);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(red, dark);
  tft.drawString("SELECT", 134, 221 + o, 1);
  tft.drawString("START", 186, 221 + o, 1);
  tft.fillRoundRect(108, 228 + o, 104, 20, 4, grey);
  tft.fillRoundRect(108, 252 + o, 104, 4, 2, stripe);
  // the grey squares behind B / A, their red letters below
  tft.fillRoundRect(226, 210 + o, 34, 34, 4, grey);
  tft.fillRoundRect(268, 210 + o, 34, 34, 4, grey);
  tft.drawString("B", 243, 252 + o, 1);
  tft.drawString("A", 285, 252 + o, 1);
}

// The SMS control pad's body: drawn once; the D-pad and the two buttons go on top (ouiKvNesBtn).
static void ouiKvSmsBody()
{
  const uint16_t body = tft.color565(22, 22, 24), red = tft.color565(200, 40, 45);
  const uint16_t grey = tft.color565(96, 96, 100), dark = tft.color565(14, 14, 16);
  const uint16_t white = tft.color565(220, 220, 220);
  const int o = ouiKvTy(), y = OUI_KV_NESY + o;
  tft.fillRect(0, y - 2, 320, OUI_KV_BTNY - OUI_KV_NESY, OUI_BG);
  tft.fillRoundRect(6, y, 308, 80, 8, body);
  // the CONTROL PAD panel: a red frame, its name, the grey square the D-pad sits in
  tft.drawRoundRect(14, 191 + o, 100, 72, 4, red);
  tft.drawRoundRect(15, 192 + o, 98, 70, 4, red);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(white, body);
  tft.drawString("CONTROL PAD", 64, 198 + o, 1);
  tft.fillRoundRect(34, 203 + o, 60, 59, 4, grey);
  tft.fillRect(55, 224 + o, 18, 18, tft.color565(60, 60, 64));
  // the middle: the ribbed strip and SEGA
  const uint16_t rib = tft.color565(48, 48, 52);
  for (int r = 200; r < 260; r += 3) tft.fillRect(122, r + o, 76, 1, rib);
  tft.fillRect(130, 224 + o, 60, 18, body);
  tft.setTextColor(tft.color565(200, 200, 205), body);
  tft.drawString("SEGA", 160, 233 + o, 2);
  // the two buttons: a red label strip above a grey square, the button's dark collar
  static const char *const kLbl[2] = { "1 START", "2" };
  for (int b = 0; b < 2; b++) {
    const int cx = 227 + b * 56, px = cx - 24;
    tft.fillRect(px, 194 + o, 48, 11, red);
    tft.setTextColor(white, red);
    tft.drawString(kLbl[b], cx, 200 + o, 1);
    tft.fillRoundRect(px, 208 + o, 48, 54, 4, grey);
    tft.fillRoundRect(cx - 17, 218 + o, 34, 34, 17, dark);
  }
}

// The ColecoVision controller's body: drawn once; the stick's directions, the side buttons and the
// keypad go on top (ouiKvNesBtn).
static void ouiKvColecoBody()
{
  const uint16_t body = tft.color565(18, 18, 20), collar = tft.color565(6, 6, 6);
  const uint16_t silver = tft.color565(186, 186, 192), seam = tft.color565(120, 120, 124);
  const int o = ouiKvTy(), y = OUI_KV_NESY + o;
  tft.fillRect(0, y - 2, 320, OUI_KV_BTNY - OUI_KV_NESY, OUI_BG);
  tft.fillRoundRect(10, y, 300, 80, 8, body);
  // the grey clips on the edges, where the stick's housing meets the keypad's
  tft.fillRect(108, y, 10, 6, seam); tft.fillRect(108, y + 74, 10, 6, seam);
  // the stick: the black collar, the silver disc, its shine
  tft.fillRoundRect(38, 204 + o, 44, 44, 22, collar);
  tft.fillRoundRect(46, 212 + o, 28, 28, 14, silver);
  tft.fillRoundRect(53, 217 + o, 9, 6, 3, tft.color565(232, 232, 236));
  // COLECO, embossed
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(tft.color565(70, 70, 76), body);
  tft.drawString("COLECO", 153, 226 + o, 2);
  // the keypad's recess
  tft.fillRoundRect(191, 186 + o, 112, 80, 4, collar);
}

// The Atari's body: the CX40 joystick (the cable out to its plug, the square base, the red FIRE
// button's collar, the orange dashed ring, the stick) and the console's lever panel. Drawn once;
// the directions, FIRE and the levers go on top (ouiKvNesBtn).
static void ouiKvAtariBody()
{
  const uint16_t base = tft.color565(20, 20, 20), rim = tft.color565(70, 70, 72);
  const uint16_t edge = tft.color565(8, 8, 8), orange = tft.color565(230, 130, 40);
  const uint16_t cable = tft.color565(40, 40, 42), wood = tft.color565(92, 56, 28);
  const uint16_t grain = tft.color565(72, 42, 20), slot = tft.color565(12, 12, 12);
  const int o = ouiKvTy(), y = OUI_KV_NESY + o, cx = 150, cy = 226 + o;
  tft.fillRect(0, y - 2, 320, OUI_KV_BTNY - OUI_KV_NESY, OUI_BG);
  // the cable from the base's left side, the plug at its end
  tft.fillRect(40, 224 + o, 70, 4, cable);
  tft.fillRoundRect(18, 217 + o, 24, 18, 3, cable);
  tft.fillRect(10, 220 + o, 8, 12, tft.color565(150, 150, 155));
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(orange, OUI_BG);
  tft.drawString("ATARI", 50, 204 + o, 1);
  tft.setTextColor(OUI_LBL, OUI_BG);
  tft.drawString("CX40", 50, 248 + o, 1);
  // the square base, its rim, the FIRE button's collar
  tft.fillRoundRect(110, y, 80, 80, 6, base);
  tft.drawRoundRect(110, y, 80, 80, 6, rim);
  tft.fillRoundRect(110, y + 1, 18, 18, 9, edge);
  // the orange ring: short dashes all round, under the directions
  for (int a = 0; a < 360; a += 12) {
    const float r = a * 3.14159265f / 180.0f;
    tft.fillRect(cx + (int)lroundf(30 * cosf(r)) - 1, cy + (int)lroundf(30 * sinf(r)) - 1, 3, 3, orange);
  }
  // the stick: the boot, the shaft, a highlight
  tft.fillRoundRect(cx - 12, cy - 12, 24, 24, 12, edge);
  tft.fillRoundRect(cx - 9, cy - 9, 18, 18, 9, tft.color565(30, 30, 30));
  tft.fillRoundRect(cx - 5, cy - 5, 4, 4, 2, tft.color565(110, 110, 110));
  // the console's panel: wood grain, the black slots the levers stand in, their names
  tft.fillRoundRect(214, y + 2, 102, 76, 6, wood);
  for (int r = y + 6; r < y + 76; r += 5) tft.fillRect(218, r, 94, 1, grain);
  for (int b = 0; b < 2; b++) {
    const int lx = 243 + b * 44;
    tft.fillRoundRect(lx - 12, 196 + o, 24, 42, 4, slot);
    tft.setTextColor(tft.color565(220, 220, 220), wood);
    tft.drawString(b ? "RESET" : "SELECT", lx, 250 + o, 1);
  }
}

// The joystick's body (after the picture: a black base, the raised plate, the ball-top stick, the
// cable out to the DB9 plug): drawn once; the directions and fire buttons go on top (ouiKvNesBtn).
static void ouiKvJoyBody()
{
  const uint16_t base = tft.color565(26, 26, 26), rim = tft.color565(72, 72, 72);
  const uint16_t plate = tft.color565(44, 44, 44), edge = tft.color565(16, 16, 16);
  const uint16_t cable = tft.color565(62, 62, 66), metal = tft.color565(160, 160, 165);
  const int o = ouiKvTy(), y = OUI_KV_NESY + o;
  tft.fillRect(0, y - 2, 320, OUI_KV_BTNY - OUI_KV_NESY, OUI_BG);
  // the cable, from the base's right side up into the plug
  tft.fillRect(240, 243 + o, 22, 4, cable);
  tft.fillRect(258, 233 + o, 4, 14, cable);
  tft.fillRect(258, 233 + o, 14, 4, cable);
  // the DB9 plug: the black hood, the metal shell, the trapezoid face with its 5 + 4 pins
  tft.fillRoundRect(270, 226 + o, 12, 18, 2, cable);
  tft.fillRect(282, 224 + o, 4, 22, metal);
  tft.fillRoundRect(286, 227 + o, 22, 16, 3, metal);
  tft.fillRoundRect(288, 229 + o, 18, 12, 2, edge);
  for (int p = 0; p < 5; p++) tft.fillRect(290 + p * 3, 231 + o, 2, 2, metal);
  for (int p = 0; p < 4; p++) tft.fillRect(291 + p * 3, 237 + o, 2, 2, metal);
  char port[12];
  snprintf(port, sizeof(port), "PORT %d", joyPort == 1 ? 1 : 2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_LBL, OUI_BG);
  tft.drawString(port, 290, 214 + o, 1);
  // the base and its rim, the plate with a shadow edge
  tft.fillRoundRect(80, y, 160, 80, 12, base);
  tft.drawRoundRect(80, y, 160, 80, 12, rim);
  tft.fillRoundRect(117, y + 12, 86, 66, 6, edge);
  tft.fillRoundRect(116, y + 11, 86, 66, 6, plate);
  // the red buttons' collars, and which button each is
  tft.fillRoundRect(83, 189 + o, 30, 30, 15, edge);
  tft.fillRoundRect(207, 189 + o, 30, 30, 15, edge);
  tft.fillRoundRect(90, 242 + o, 20, 20, 10, edge);
  tft.setTextColor(OUI_LBL, OUI_BG);
  tft.drawString("1", 71, 204 + o, 1);
  tft.drawString("2", 249, 204 + o, 1);
  tft.drawString("3", 71, 252 + o, 1);
  // the stick: the boot, the shaft's collar, the ball with a highlight
  tft.fillRoundRect(146, 217 + o, 28, 28, 14, edge);
  tft.fillRoundRect(149, 220 + o, 22, 22, 11, tft.color565(10, 10, 10));
  tft.fillRoundRect(152, 223 + o, 16, 16, 8, tft.color565(34, 34, 34));
  tft.fillRoundRect(155, 225 + o, 5, 4, 2, tft.color565(120, 120, 120));
}

// The Apple joystick's body (after the picture: the beige box with its shadow, the collars of the
// orange buttons, the dark square well the stick stands in, the cable out of the back right to its
// plug): drawn once; the directions and buttons go on top (ouiKvNesBtn).
static void ouiKvA2JoyBody()
{
  const uint16_t beige = tft.color565(214, 200, 168), shade = tft.color565(160, 146, 116);
  const uint16_t well = tft.color565(58, 52, 44), ink = tft.color565(112, 96, 70);
  const uint16_t cable = tft.color565(150, 140, 120), black = tft.color565(24, 24, 24);
  const int o = ouiKvTy(), y = OUI_KV_NESY + o;
  tft.fillRect(0, y - 2, 320, OUI_KV_BTNY - OUI_KV_NESY, OUI_BG);
  // the cable out of the back right, the plug with its pins
  tft.fillRect(246, 196 + o, 30, 4, cable);
  tft.fillRoundRect(276, 190 + o, 30, 16, 2, black);
  for (int p = 0; p < 8; p++) tft.fillRect(279 + p * 3, 206 + o, 1, 3, tft.color565(190, 190, 195));
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_LBL, OUI_BG);
  tft.drawString("GAME I/O", 290, 218 + o, 1);
  // the box, its shadow
  tft.fillRoundRect(78, y + 4, 172, 78, 8, shade);
  tft.fillRoundRect(74, y + 2, 172, 76, 8, beige);
  // the buttons' collars, which button each is
  tft.fillRoundRect(84, 212 + o, 26, 26, 13, shade);
  tft.fillRoundRect(108, 190 + o, 32, 16, 8, shade);
  tft.setTextColor(ink, beige);
  tft.drawString("0", 97, 246 + o, 1);
  tft.drawString("1", 148, 198 + o, 1);
  tft.drawGlyph8(88, 250 + o, 8, 8, kOuiKvGlyph[8], ink, beige, true);   // the embossed apple
  // the well: a shaded rim, the dark floor; the stick in the middle
  tft.fillRoundRect(150, 193 + o, 84, 68, 4, shade);
  tft.fillRoundRect(152, 195 + o, 80, 64, 3, well);
  tft.fillRoundRect(184, 220 + o, 16, 16, 8, black);
  tft.fillRoundRect(188, 223 + o, 4, 4, 2, tft.color565(130, 130, 130));
}

// The Spectrum's body behind its keys: the black case, the rainbow flash at the right of the top
// row, "sinclair" under it. Drawn once; the keys go on top (ouiKvZxKey).
static void ouiKvZxBody()
{
  const int o = ouiKvTy(), y = OUI_KV_CKY + o;
  const uint16_t black = tft.color565(16, 16, 18);
  tft.fillRect(0, y - 2, 320, OUI_KV_BTNY - OUI_KV_CKY, OUI_BG);
  tft.fillRoundRect(2, y - 2, 316, 4 * OUI_KV_ZXR + 2, 4, black);
  static const uint8_t kFlash[4][3] = { { 220, 40, 40 }, { 240, 200, 40 }, { 60, 170, 70 }, { 40, 140, 210 } };
  for (int b = 0; b < 4; b++)                       // the four stripes, slanting up to the right
    for (int r = 0; r < 14; r++)
      tft.fillRect(292 + b * 5 + r / 2, y + 16 - r, 5, 1, tft.color565(kFlash[b][0], kFlash[b][1], kFlash[b][2]));
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(tft.color565(200, 200, 205), black);
  tft.drawString("ZX", 316, y + OUI_KV_ZXR + 9, 1);
}

// The Apple IIe's case behind its keys: beige, the rainbow "//e" badge under RESET. Drawn once.
static void ouiKvA2eBody()
{
  const int o = ouiKvTy(), y = OUI_KV_CKY + o;
  const uint16_t beige = tft.color565(214, 200, 168);
  tft.fillRect(0, y - 2, 320, OUI_KV_BTNY - OUI_KV_CKY, OUI_BG);
  tft.fillRoundRect(1, y - 3, 318, 4 * OUI_KV_PITCH + 12 + 6, 4, beige);
  static const uint8_t kBow[6][3] = { { 97, 187, 70 }, { 253, 184, 39 }, { 245, 130, 31 },
                                      { 224, 58, 62 }, { 150, 61, 151 }, { 0, 157, 220 } };
  const int by = y + OUI_KV_PITCH + 4;               // the badge: the apple's six stripes, "//e"
  for (int b = 0; b < 6; b++) tft.fillRect(287, by + b * 2, 8, 2, tft.color565(kBow[b][0], kBow[b][1], kBow[b][2]));
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(tft.color565(90, 80, 64), beige);
  tft.drawString("//e", 297, by + 6, 1);
  tft.drawString("apple", 285, by + 22, 1);
}

// A Spectrum key: the red SYMBOL SHIFT word on top, the key itself below (white; both red on
// SYMBOL SHIFT, as printed).
static void ouiKvZxKey(const OuiKvR &r, const char *lbl, uint16_t bg, uint16_t fg)
{
  char top[8], bot[8];
  const char *bar = strchr(lbl, '|');
  snprintf(bot, sizeof(bot), "%.*s", (int)(bar - lbl), lbl);
  snprintf(top, sizeof(top), "%s", bar + 1);
  tft.fillRoundRect(r.x, r.y, r.w, r.h, 3, bg);
  const uint16_t red = tft.color565(255, 96, 96);
  const bool sym = !strcmp(top, "SYMB");
  tft.setTextDatum(MC_DATUM);
  if (!top[0]) { tft.setTextColor(fg, bg); tft.drawString(bot, r.x + r.w / 2, r.y + r.h / 2, 1); return; }
  tft.setTextColor(strcmp(top, "CAPS") ? red : fg, bg);   // CAPS SHIFT is printed white
  if (top[0] == 7 && !top[1]) tft.drawGlyph8(r.x + (r.w - 8) / 2, r.y + 1, 8, 8, kOuiKvGlyph[6], red, bg, true);   // pound
  else tft.drawString(top, r.x + r.w / 2, r.y + 5, 1);
  tft.setTextColor(sym ? red : fg, bg);
  tft.drawString(bot, r.x + r.w / 2, r.y + 14, 1);
}

// The Cheetah 125+'s body: the cable out to the Kempston interface, the black base and its rim,
// the red buttons' collars, the grip round the trigger. Drawn once; the directions and the three
// FIRE buttons go on top (ouiKvNesBtn).
static void ouiKvZxJoyBody()
{
  const uint16_t base = tft.color565(24, 24, 26), rim = tft.color565(74, 74, 78);
  const uint16_t edge = tft.color565(10, 10, 10), grip = tft.color565(40, 40, 44);
  const uint16_t cable = tft.color565(56, 56, 60), metal = tft.color565(160, 160, 165);
  const int o = ouiKvTy(), y = OUI_KV_NESY + o;
  tft.fillRect(0, y - 2, 320, OUI_KV_BTNY - OUI_KV_NESY, OUI_BG);
  // the cable from the base's back right, round to the Kempston interface
  tft.fillRect(254, 200 + o, 20, 4, cable);
  tft.fillRect(270, 200 + o, 4, 30, cable);
  tft.fillRoundRect(262, 230 + o, 52, 22, 3, tft.color565(30, 30, 32));
  tft.fillRect(266, 252 + o, 44, 3, metal);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_LBL, tft.color565(30, 30, 32));
  tft.drawString("KEMPSTON", 288, 241 + o, 1);
  tft.setTextColor(OUI_LBL, OUI_BG);
  tft.drawString("CHEETAH", 40, 204 + o, 1);
  tft.drawString("125+", 40, 216 + o, 1);
  // the base, its rim, the red buttons' collars
  tft.fillRoundRect(84, y + 2, 172, 76, 12, base);
  tft.drawRoundRect(84, y + 2, 172, 76, 12, rim);
  tft.fillRoundRect(92, 211 + o, 30, 30, 15, edge);
  tft.fillRoundRect(218, 211 + o, 30, 30, 15, edge);
  // the grip, seen from above, the trigger's collar on top
  tft.fillRoundRect(154, 211 + o, 32, 30, 12, grip);
  tft.fillRoundRect(160, 216 + o, 20, 20, 10, edge);
  tft.setTextColor(OUI_LBL, base);
  tft.drawString("FIRE", 107, 250 + o, 1);
  tft.drawString("FIRE", 233, 250 + o, 1);
}

// The MSX joystick's body (after the picture: the cable out of the base's back left, the black
// square base with its rim, the triggers' collars, the black stick with its orange top): drawn
// once; the directions and triggers go on top (ouiKvNesBtn).
static void ouiKvMsxJoyBody()
{
  const uint16_t base = tft.color565(24, 24, 24), rim = tft.color565(74, 74, 76);
  const uint16_t edge = tft.color565(8, 8, 8), orange = tft.color565(240, 120, 30);
  const uint16_t cable = tft.color565(40, 40, 42);
  const int o = ouiKvTy(), y = OUI_KV_NESY + o, cx = 160, cy = 226 + o;
  tft.fillRect(0, y - 2, 320, OUI_KV_BTNY - OUI_KV_NESY, OUI_BG);
  // the cable out of the base's left side, the DB9 plug at its end
  tft.fillRect(44, 196 + o, 62, 4, cable);
  tft.fillRoundRect(22, 189 + o, 24, 18, 3, cable);
  tft.fillRect(14, 192 + o, 8, 12, tft.color565(150, 150, 155));
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_LBL, OUI_BG);
  tft.drawString("A", 98, 254 + o, 1);
  tft.drawString("B", 98, 210 + o, 1);
  tft.setTextColor(orange, OUI_BG);
  tft.drawString("MSX", 262, 214 + o, 2);
  tft.setTextColor(OUI_LBL, OUI_BG);
  tft.drawString("JOYSTICK", 262, 236 + o, 1);
  // the square base and its rim, the triggers' collars
  tft.fillRoundRect(106, y, 100, 80, 6, base);
  tft.drawRoundRect(106, y, 100, 80, 6, rim);
  tft.fillRoundRect(109, 241 + o, 26, 26, 13, edge);
  tft.fillRoundRect(112, 188 + o, 18, 18, 9, edge);
  // the stick: the boot, the black shaft, the orange top with a highlight
  tft.fillRoundRect(cx - 13, cy - 13, 26, 26, 13, edge);
  tft.fillRoundRect(cx - 10, cy - 10, 20, 20, 10, tft.color565(34, 34, 34));
  tft.fillRoundRect(cx - 6, cy - 6, 12, 12, 6, orange);
  tft.fillRoundRect(cx - 4, cy - 4, 4, 3, 1, tft.color565(255, 190, 130));
}

static void ouiKvNesBtn(int i, uint16_t bg, uint16_t fg)
{
  const OuiKvNk &n = ouiKvTab()[i];
  const int y = n.y + ouiKvTy();
  if (n.shape == OUI_NK_PAD) {
    tft.fillRect(n.x, y, n.w, n.h, bg);
    tft.drawGlyph8(n.x + (n.w - 8) / 2, y + (n.h - 8) / 2, 8, 8, kOuiKvGlyph[n.lbl[0] - 1], fg, bg, true);
  } else if (n.shape == OUI_NK_KEY) {
    tft.fillRoundRect(n.x, y, n.w, n.h, 3, bg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(fg, bg);
    tft.drawString(n.lbl, n.x + n.w / 2, y + n.h / 2 + 1, n.w > 16 ? 2 : 1);
  } else {
    tft.fillRoundRect(n.x, y, n.w, n.h, n.h / 2, bg);
  }
}

// The rule being shown: mode 6 the key under the cursor, modes 7-8 the one being changed.
static bool ouiKvShown(uint8_t &hid, bool &sh)
{
  if (ouiCfgMode == 6) return ouiKvTarget(ouiKvPkSel, ouiKvLayer, hid, sh);
  hid = ouiKmHid; sh = ouiKmShift;
  return true;
}

// Does rule code eff press target ti (an entry of the target picture)?
static bool ouiKvHits(uint8_t eff, int ti)
{
  if (ti < 0 || ti >= ouiKvNck()) return false;
  if (ouiKmSimple()) return ouiKmIsBtn(eff) && eff == ouiKvTab()[ti].code;
  const uint8_t k = ouiKvCkTab()[ti].code;
  return k == C64K_RESTORE ? eff == C64K_RESTORE : (ouiKmIsKey(eff) && ouiKmIdx(eff) == k);
}

// The marks (SPACE): how many rules are marked (diff: whose mark changed since it started). Two
// picture keys can edit the same rule (a key with no SHIFT+ rule shows on both layers): one count.
static int ouiKvMkScan(bool diff)
{
  uint16_t seen[2 * OUI_KV_NPK];
  int n = 0;
  for (int l = 0; l < 2; l++)
    for (int i = 0; i < OUI_KV_NPK; i++) {
      if (diff ? ouiKvMk[l][i] == ouiKvMk0[l][i] : !ouiKvMk[l][i]) continue;
      uint8_t h; bool s;
      if (!ouiKvTarget(i, l, h, s)) continue;
      const uint16_t k = (uint16_t)(h << 1 | s);
      int j = 0;
      while (j < n && seen[j] != k) j++;
      if (j == n) seen[n++] = k;
    }
  return n;
}
// Mark / unmark rule (h, s) on every picture key that edits it (start: as it started, too).
static void ouiKvMkSet(uint8_t h, bool s, bool on, bool start)
{
  for (int l = 0; l < 2; l++)
    for (int i = 0; i < OUI_KV_NPK; i++) {
      uint8_t hh; bool ss;
      if (!ouiKvTarget(i, l, hh, ss) || hh != h || ss != s) continue;
      ouiKvMk[l][i] = on;
      if (start) ouiKvMk0[l][i] = on;
    }
}
static void ouiKvMkClear() { memset(ouiKvMk, 0, sizeof(ouiKvMk)); memset(ouiKvMk0, 0, sizeof(ouiKvMk0)); }
// Swapped: the keys that press target ouiKvCkSel start marked.
static void ouiKvMkHits()
{
  for (int l = 0; l < 2; l++)
    for (int i = 0; i < OUI_KV_NPK; i++) {
      uint8_t h; bool s;
      ouiKvMk[l][i] = ouiKvMk0[l][i] = ouiKvTarget(i, l, h, s) && ouiKvHits(ouiKmLookup(h, s), ouiKvCkSel);
    }
}
// The marked rule shown while the target is picked: the one under the cursor, else the first.
static bool ouiKvMkFirst(uint8_t &h, bool &s)
{
  if (ouiKvMk[ouiKvLayer][ouiKvPkSel] && ouiKvTarget(ouiKvPkSel, ouiKvLayer, h, s)) return true;
  for (int l = 0; l < 2; l++)
    for (int i = 0; i < OUI_KV_NPK; i++)
      if (ouiKvMk[l][i] && ouiKvTarget(i, l, h, s)) return true;
  return false;
}
// The SHIFT step's first choice for rule (h, s): its current SHIFT handling, else the usual one
// (a SHIFT+ rule: released).
static int ouiKvShPre(uint8_t h, bool s)
{
  const uint8_t c = ouiKmRule(h, s, nullptr);
  int sm = ouiKmIsKey(c) ? ouiKmShf(c) : (s ? C64K_SHIFT_OFF : C64K_SHIFT_KEEP);
  if (s && sm == C64K_SHIFT_KEEP) sm = C64K_SHIFT_ON;
  return sm;
}
// Swapped, Esc: the rule giving (h, s) target ck (the SHIFT step taken as preselected), or taking
// it away (back to the default, or NONE when the default is this target too).
static uint8_t ouiKvGiveCode(uint8_t h, bool s, int ck)
{
  if (ouiKmSimple()) return ouiKvTab()[ck].code;
  const uint8_t code = ouiKvCkTab()[ck].code;
  if (code == C64K_RESTORE || (ouiKmA2() && a2kIsPlainOnly(code))) return code;
  return ouiKmMake(code, ouiKvShPre(h, s));
}
static uint8_t ouiKvTakeCode(uint8_t h, bool s, int ck)
{
  uint8_t df = ouiKmDefault(h, s);
  if (df == C64K_NONE && s) df = ouiKmLookup(h, false);   // a SHIFT+ rule: as without SHIFT
  return ouiKvHits(df, ck) ? C64K_UNMAP : C64K_NONE;
}

// Target ti's name, for the info line.
static void ouiKvTgtName(int ti, char *o, size_t n)
{
  if (ouiKmSimple()) { ouiKmDescribe(ouiKvTab()[ti].code, false, o, n); return; }
  const uint8_t k = ouiKvCkTab()[ti].code;
  snprintf(o, n, "%s", k == C64K_RESTORE ? (ouiKmA2() ? "RESET" : "RESTORE") : ouiKpName(k));
}

// The swapped info line: target ti and the keys that press it now.
static void ouiKvTgtKeys(int ti, char *o, size_t n)
{
  char t[24], k[24];
  ouiKvTgtName(ti, t, sizeof(t));
  int len = snprintf(o, n, "%s  <-  ", t), cnt = 0;
  for (int i = 0; i < ouiKmHostCount(); i++)
    for (int s = 0; s < 2; s++) {
      const uint8_t h = ouiKmHostAt(i);
      if (s && !ouiKmHasShifted(h)) continue;
      if (!ouiKvHits(ouiKmLookup(h, s), ti)) continue;
      ouiKmLabel(h, s, k, sizeof(k));
      if (len < (int)n) len += snprintf(o + len, n - len, "%s%s", cnt++ ? ", " : "", k);
    }
  if (!cnt && len < (int)n) snprintf(o + len, n - len, "no key");
}

static void ouiDrawKv()
{
  const int m = ouiCfgMode;
  const bool nes = ouiKmNes(), sms = ouiKmSms(), col = ouiKmCol(), ata = ouiKmAtari(), joy = ouiKmJoy(), simple = nes || sms || col || ata || joy, rev = ouiKvRev;
  const bool a2 = ouiKmA2(), a2j = ouiKmA2J();   // the Apple's keyboard / joystick
  const bool zx = ouiKmZx(), zxj = ouiKmZxJ();   // the Spectrum's keyboard / Kempston joystick
  const bool msx = ouiKmMsx(), msxj = ouiKmMsxJ();   // the MSX's keyboard / joystick
  const char *tgt = a2 ? "APPLE" : msx ? "MSX" : zx ? "ZX" : "C64";
  const char *pad = sms ? "SMS" : col ? "COLECO" : ata ? "ATARI" : "NES";   // the pad the keys press (NES / SMS pages)
  // TAB moved the pictures: start over on a clean page.
  if (ouiChanged(ouiKvSigDir, rev ? 2 : 1)) {
    tft.fillRect(0, OUI_KV_PKY - 2, 320, OUI_KV_BTNY - OUI_KV_PKY, OUI_BG);
    memset(ouiKvSigPk, 0, sizeof(ouiKvSigPk)); memset(ouiKvSigCk, 0, sizeof(ouiKvSigCk));
    ouiKvSigBtn = ouiKvSigBody = OUI_SIG_NONE;
  }
  if (rev) {
    const char *w = joy ? "JOYSTICK" : "KEYBOARD";
    char t[40];
    if (m == 6) snprintf(t, sizeof(t), "%s BY %s", w, joy ? "LINE" : col ? "COLECO BUTTON" : nes || sms ? (sms ? "SMS BUTTON" : "NES BUTTON") : ata ? "ACTION" : msx ? "MSX KEY" : a2 ? "APPLE KEY" : zx ? "ZX KEY" : "C64 KEY");
    else        snprintf(t, sizeof(t), "%s > %s", w, m == 7 ? "KEY" : msx ? "MSX SHIFT" : a2 ? "APPLE SHIFT" : zx ? "ZX SHIFT" : "C64 SHIFT");
    ouiDrawListTitle(t);
  }
  else if (joy) ouiDrawListTitle(m == 6 ? "JOYSTICK" : "JOYSTICK > DIRECTION");
  else ouiDrawListTitle(m == 6 ? "KEYBOARD" : m == 7 ? (nes ? "KEYBOARD > NES BUTTON" : sms ? "KEYBOARD > SMS BUTTON" : col ? "KEYBOARD > COLECO" : ata ? "KEYBOARD > ATARI" : msx ? "KEYBOARD > MSX KEY" : a2 ? "KEYBOARD > APPLE KEY" : zx ? "KEYBOARD > ZX KEY" : "KEYBOARD > C64 KEY")
                                       : msx ? "KEYBOARD > MSX SHIFT" : a2 ? "KEYBOARD > APPLE SHIFT" : zx ? "KEYBOARD > ZX SHIFT" : "KEYBOARD > C64 SHIFT");
  uint8_t hid = 0; bool sh = false;
  // Swapped, mode 7 shows the key under the cursor; otherwise the forward rule (ouiKvShown).
  const bool shown = rev ? (m == 6 ? false : m == 7 ? ouiKvTarget(ouiKvPkSel, ouiKvLayer, hid, sh)
                                                    : (hid = ouiKmHid, sh = ouiKmShift, true))
                         : ouiKvShown(hid, sh);
  // What the C64 picture lights: the key the rule presses now (mode 8: the one being set up).
  uint8_t eff = shown ? ouiKmLookup(hid, sh) : C64K_NONE;
  if (m == 8) eff = ouiKmMake(ouiKmKey, ouiKmShift ? ouiKvShSel + 1 : ouiKvShSel);
  const bool effMx = ouiKmIsKey(eff);
  // The Apple's +CTRL leaves SHIFT as typed, and lights CTRL.
  const bool effCtrl = a2 && effMx && a2kMode(eff) == A2K_CTRL_ON;
  const bool effShift = effMx && (ouiKmShf(eff) == C64K_SHIFT_ON || ((ouiKmShf(eff) == C64K_SHIFT_KEEP || effCtrl) && sh));
  const bool effSym = zx && effMx && zxkMode(eff) == ZXK_SYM;   // the ZX's +SYMBOL lights SYMBOL SHIFT
  const int nMk = ouiKvMkScan(false), nDf = rev ? ouiKvMkScan(true) : 0;   // keys marked / marks changed

  // The line under the title: what the key types / what is being asked.
  char info[100], k[24], d[28];
  if (rev && m == 6) ouiKvTgtKeys(ouiKvCkSel, info, sizeof(info));
  else if (!shown) {
    const OuiKvPk &p = kOuiKvPk[ouiKvPkSel];
    if (p.hid == OUI_KV_SHIFT && zxj) snprintf(info, sizeof(info), "Shift: CAPS SHIFT, not a joystick key");
    else if (p.hid == OUI_KV_SHIFT) snprintf(info, sizeof(info), "Shift: Enter shows the %s keys", ouiKvLayer ? "plain" : "SHIFT+");
    else if (p.hid && (nes || sms || ata) && ouiKvLayer && !p.shid) snprintf(info, sizeof(info), "No SHIFT on the %s: the plain key's button", pad);
    else if (p.hid && joy && ouiKvLayer && !p.shid) snprintf(info, sizeof(info), "No SHIFT+ joystick rules: the plain key's");
    else if (p.hid) snprintf(info, sizeof(info), "Shift+F5 is the snapshot page: not a key");
    else snprintf(info, sizeof(info), "Caps Lock stays on the keyboard");
  } else {
    ouiKmLabel(hid, sh, k, sizeof(k));
    if (!rev && m != 6 && nMk > 1) snprintf(k, sizeof(k), "%d keys", nMk);   // the marked keys
    if (eff == C64K_NONE) snprintf(d, sizeof(d), "nothing");
    else ouiKmDescribe(eff, sh, d, sizeof(d));
    if (rev && m == 7) {
      char t[24];
      ouiKvTgtName(ouiKvCkSel, t, sizeof(t));
      if (ouiKvHits(eff, ouiKvCkSel)) snprintf(info, sizeof(info), "%s presses %s: Enter takes it away", k, t);
      else snprintf(info, sizeof(info), "%s -> %s?  (now %s)", k, t, d);
    }
    else if (m == 6) snprintf(info, sizeof(info), "%s  ->  %s: %s", k, nes || sms || col || ata ? pad : joy ? "JOY" : tgt, d);
    else if (m == 7 && joy) snprintf(info, sizeof(info), "%s pulls which joystick line?  (now %s)", k, d);
    else if (m == 7) snprintf(info, sizeof(info), "%s presses which %s?  (now %s)", k, nes ? "NES button" : sms ? "SMS button" : col ? "Coleco button" : ata ? "Atari action" : msx ? "MSX key" : a2 ? "Apple key" : zx ? "Spectrum key" : "C64 key", d);
    else if (a2)     snprintf(info, sizeof(info), "%s = %s, and the Apple's SHIFT / CTRL:", k, ouiKpName(ouiKmKey));
    else if (zx)     snprintf(info, sizeof(info), "%s = %s, and CAPS / SYMBOL SHIFT:", k, ouiKpName(ouiKmKey));
    else if (msx)    snprintf(info, sizeof(info), "%s = %s, and the MSX's SHIFT is:", k, ouiKpName(ouiKmKey));
    else             snprintf(info, sizeof(info), "%s = %s, and the C64's SHIFT is:", k, c64KeymapC64Name(ouiKmKey));
  }
  bool over = false;
  if (shown) ouiKmRule(hid, sh, &over);
  const uint16_t ic = rev && m == 6 ? OUI_TXT : !shown ? OUI_LBL : over ? OUI_ON : OUI_TXT;
  char is[120];
  snprintf(is, sizeof(is), "%s|%u", info, (unsigned)ic);
  if (ouiChanged(ouiKvSigInfo, ouiHash(is))) {
    tft.fillRect(0, OUI_TITLE_H, 320, OUI_KV_PKY - 2 - OUI_TITLE_H, OUI_BG);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(ic, OUI_BG);
    tft.drawString(ouiRomFit(info, 308, 1).c_str(), 6, OUI_TITLE_H + 9, 1);
  }

  // PicoCalc keys: cursor blue, the key being changed orange, the Shift keys green while their
  // layer shows; a green label = that key's rule is changed. Swapped: the keys that press the
  // target green.
  for (int i = 0; i < OUI_KV_NPK; i++) {
    const OuiKvPk &p = kOuiKvPk[i];
    uint8_t h = 0; bool s = false;
    const bool t = ouiKvTarget(i, ouiKvLayer, h, s);
    bool o = false;
    if (t) ouiKmRule(h, s, &o);
    uint16_t bg = OUI_CARD, fg = o ? OUI_ON : OUI_TXT;
    if (!t && p.hid != OUI_KV_SHIFT) { bg = OUI_CARD2; fg = OUI_LBL; }
    if (p.hid == OUI_KV_SHIFT && ouiKvLayer) bg = OUI_MOUNT;
    // Marked (SPACE): forward orange; swapped green, the keys that will press the target.
    const bool mk = t && ouiKvMk[ouiKvLayer][i];
    if (rev && t && (m == 6 ? ouiKvHits(ouiKmLookup(h, s), ouiKvCkSel) : mk)) bg = OUI_MOUNT;
    if (ouiKvOnHost() && i == ouiKvPkSel) { bg = OUI_SEL; fg = mk ? (rev ? OUI_ON : OUI_REBOOT) : OUI_TXT; }   // (the line above is green then)
    else if (!rev && mk) { bg = OUI_REBOOT; fg = OUI_TXT; }
    else if ((rev ? m == 8 : m != 6 && !nMk) && t && h == ouiKmHid && s == ouiKmShift) { bg = OUI_REBOOT; fg = OUI_TXT; }
    const char *l = ouiKvLayer && p.slbl ? p.slbl : p.lbl;
    uint32_t sig = ((uint32_t)bg << 16 | fg) ^ (uint32_t)(uintptr_t)l;
    if (!sig) sig = 1;
    if (!ouiChanged(ouiKvSigPk[i], sig)) continue;
    OuiKvR r; ouiKvPkRect(i, r);
    ouiKvKey(r, l, bg, fg);
  }

  // NES buttons / joystick lines: the one the rule presses green, cursor blue (mode 7). Swapped:
  // cursor blue (mode 6), the one being given orange.
  if (simple) {
    if (ouiChanged(ouiKvSigBody, (msxj ? 14 : a2j ? 11 : zxj ? 12 : joy ? 2 + joyPort : sms ? 8 : ata ? 9 : col ? 10 : 1) | (rev ? 0x10 : 0))) {
      if (msxj) ouiKvMsxJoyBody(); else if (a2j) ouiKvA2JoyBody(); else if (zxj) ouiKvZxJoyBody(); else if (joy) ouiKvJoyBody(); else if (sms) ouiKvSmsBody(); else if (col) ouiKvColecoBody(); else if (ata) ouiKvAtariBody(); else ouiKvNesBody();
      memset(ouiKvSigCk, 0, sizeof(ouiKvSigCk));
    }
    const OuiKvNk *tab = ouiKvTab();
    for (int i = 0; i < ouiKvNck(); i++) {
      const OuiKvNk &n = tab[i];
      const bool hit = !rev && ouiKmIsBtn(eff) && eff == n.code;
      const int pg = joy ? 32 : sms ? 78 : ata ? 20 : col ? 40 : 24;   // the D-pad's grey
      uint16_t bg = n.shape == OUI_NK_PAD ? tft.color565(pg, pg, pg + (sms ? 4 : 0))
                  : n.shape == OUI_NK_PILL ? (ata ? tft.color565(150, 150, 155) : tft.color565(60, 60, 60))
                  : sms ? tft.color565(34, 34, 38) : tft.color565(210, 40, 45);
      uint16_t fg = ata ? tft.color565(230, 130, 40) : tft.color565(170, 170, 170);   // Atari: the ring's orange
      if (a2j) {   // the Apple's: grey-brown pads in the dark well, orange buttons
        bg = n.shape == OUI_NK_PAD ? tft.color565(100, 92, 80) : tft.color565(236, 122, 34);
        fg = tft.color565(236, 226, 204);
      }
      if (msxj) {   // the MSX's: dark grey pads on the black base, orange triggers
        bg = n.shape == OUI_NK_PAD ? tft.color565(52, 52, 54) : tft.color565(240, 120, 30);
        fg = tft.color565(240, 150, 70);
      }
      if (n.shape == OUI_NK_KEY) {   // Coleco: white keys, black '*' / '#', the grey side buttons
        const bool blk = n.code == COLK_STAR || n.code == COLK_HASH, side = n.code == COLK_LFIRE || n.code == COLK_RFIRE;
        bg = side ? tft.color565(140, 140, 145) : blk ? tft.color565(28, 28, 30) : tft.color565(226, 226, 222);
        fg = blk ? tft.color565(236, 236, 236) : tft.color565(20, 20, 20);
      }
      if (hit) { bg = OUI_MOUNT; fg = OUI_TXT; }
      if (ouiKvOnTgt() && i == ouiKvCkSel) { bg = OUI_SEL; fg = hit ? OUI_ON : OUI_TXT; }
      else if (rev && i == ouiKvCkSel) { bg = OUI_REBOOT; fg = OUI_TXT; }
      uint32_t sig = ((uint32_t)bg << 16 | fg) | 1;
      if (!ouiChanged(ouiKvSigCk[i], sig)) continue;
      ouiKvNesBtn(i, bg, fg);
    }
  }

  // C64 keys: the one the rule presses green (SHIFT too when it is held), cursor blue (mode 7).
  // Swapped: cursor blue (mode 6), the one being given orange.
  if (zx && ouiChanged(ouiKvSigBody, 13 | (rev ? 0x10 : 0))) {
    ouiKvZxBody();
    memset(ouiKvSigCk, 0, sizeof(ouiKvSigCk));
  }
  const bool a2e = a2 && AppleIIe;
  if (a2e && ouiChanged(ouiKvSigBody, 14 | (rev ? 0x10 : 0))) {
    ouiKvA2eBody();
    memset(ouiKvSigCk, 0, sizeof(ouiKvSigCk));
  }
  for (int i = 0; i < ouiKvCkN() && !simple; i++) {
    const OuiKvCk &c = ouiKvCkTab()[i];
    const bool hit = !rev && (c.code == C64K_RESTORE ? eff == C64K_RESTORE : (effMx && ouiKmIdx(eff) == c.code));
    const bool shl = !rev && ((effShift && c.code == (a2 ? A2K_LSHIFT : zx ? msx ? MSXK_SHIFT_INDEX : ZXK_CAPS_INDEX : C64K_LSHIFT_INDEX) && !(c.flags & OUI_KV_LOCK))
                              || (effCtrl && c.code == A2K_CTRL) || (effSym && c.code == ZXK_SYM_INDEX));
    uint16_t bg = zx ? tft.color565(118, 120, 130) : a2e ? tft.color565(72, 58, 46) : OUI_CARD, fg = OUI_TXT;   // the ZX: grey rubber; the IIe: dark brown
    if (hit) bg = m == 8 ? OUI_REBOOT : OUI_MOUNT;
    else if (shl) bg = OUI_MOUNT;
    if (ouiKvOnTgt() && i == ouiKvCkSel) { bg = OUI_SEL; if (hit) fg = OUI_ON; }
    else if (rev && i == ouiKvCkSel) bg = OUI_REBOOT;
    uint32_t sig = ((uint32_t)bg << 16 | fg) | 1;
    if (!ouiChanged(ouiKvSigCk[i], sig)) continue;
    OuiKvR r; ouiKvCkRect(i, r);
    if (zx) ouiKvZxKey(r, c.lbl, bg, fg); else ouiKvKey(r, c.lbl, bg, fg);
  }

  // The strip at the bottom: a legend, or the choices that are not keys.
  int bn = 0, bsel = -1;
  const char *bl[4] = { "", "", "", "" };
  uint16_t bc[4] = { OUI_TXT, OUI_TXT, OUI_TXT, OUI_TXT };
  char dl[40];
  if (m == 7 && !rev) {
    const uint8_t df = ouiKmDefault(ouiKmHid, ouiKmShift);
    if (df == C64K_NONE) snprintf(d, sizeof(d), ouiKmShift ? "as no SHIFT" : "nothing");
    else ouiKmDescribe(df, ouiKmShift, d, sizeof(d));
    snprintf(dl, sizeof(dl), "DEFAULT: %s", d);
    bool ov = false;
    const uint8_t cur = ouiKmRule(ouiKmHid, ouiKmShift, &ov);
    bn = 2; bl[0] = dl; bl[1] = "NONE (does nothing)";
    if (!ov) bc[0] = OUI_ON;
    if (cur == C64K_UNMAP) bc[1] = OUI_ON;
    bsel = ouiKvCkSel - ouiKvNck();
  } else if (m == 8) {
    static const char *const kSh[4] = { "AS TYPED", "+SHIFT", "-SHIFT", "+CTRL" };   // +CTRL: the Apple
    bn = ouiKvBtnCount();
    const uint8_t cur = ouiKmRule(ouiKmHid, ouiKmShift, nullptr);
    for (int b = 0; b < bn; b++) {
      const int sm = ouiKmShift ? b + 1 : b;
      static const char *const kZxSh[4] = { "AS TYPED", "+CAPS", "-CAPS", "+SYMBOL" };
      bl[b] = (zx ? kZxSh : kSh)[sm];
      if (ouiKmIsKey(cur) && ouiKmIdx(cur) == ouiKmKey && ouiKmShf(cur) == sm) bc[b] = OUI_ON;
    }
    bsel = ouiKvShSel;
  }
  char bs[160];
  snprintf(bs, sizeof(bs), "%d|%d|%d|%s|%s|%s|%s|%u|%u|%u|%u|%d|%d", m, bn, bsel, bl[0], bl[1], bl[2], bl[3],
           (unsigned)bc[0], (unsigned)bc[1], (unsigned)bc[2], (unsigned)bc[3], (int)ouiKvLayer, (int)rev);
  if (ouiChanged(ouiKvSigBtn, ouiHash(bs))) {
    tft.fillRect(0, OUI_KV_BTNY - 2, 320, OUI_RS_FOOT - OUI_KV_BTNY + 2, OUI_BG);
    if (!bn) {
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(OUI_LBL, OUI_BG);
      tft.drawString(rev ? (ouiKvLayer ? "SHIFT+ keys   green = presses it   label = changed"
                                       : "green = presses it    green label = changed")
                   : a2j ? (ouiKvLayer ? "SHIFT+ keys    green = changed    lit = joystick"
                                       : "green = changed    lit = what it moves / presses")
                   : zxj ? (ouiKvLayer ? "SHIFT+ keys    green = changed    lit = Kempston"
                                       : "green = changed    lit = the Kempston line it pulls")
                   : joy ? (ouiKvLayer ? "SHIFT+ keys    green = changed    lit = joystick"
                                       : "green = changed    lit = the line it pulls")
                   : nes ? (ouiKvLayer ? "SHIFT+ keys    green = changed    lit = NES button"
                                       : "green = changed    lit = the NES button it presses")
                   : sms ? (ouiKvLayer ? "SHIFT+ keys    green = changed    lit = SMS button"
                                       : "green = changed    lit = the SMS button it presses")
                   : col ? (ouiKvLayer ? "SHIFT+ keys    green = changed    lit = Coleco button"
                                       : "green = changed    lit = the Coleco button it presses")
                   : ata ? (ouiKvLayer ? "SHIFT+ keys    green = changed    lit = Atari action"
                                       : "green = changed    lit = what it moves / presses")
                   : zx  ? (ouiKvLayer ? "SHIFT+ keys    green = changed    lit = ZX key"
                                       : "green = changed    lit = the ZX key it presses")
                   : msx ? (ouiKvLayer ? "SHIFT+ keys    green = changed    lit = MSX key"
                                       : "green = changed    lit = the MSX key it presses")
                   : a2  ? (ouiKvLayer ? "SHIFT+ keys    green = changed    lit = Apple key"
                                       : "green = changed    lit = the Apple key it presses")
                         : (ouiKvLayer ? "SHIFT+ keys    green = changed    lit = C64 key"
                                       : "green = changed    lit = the C64 key it presses"), 160, OUI_KV_BTNY + 7, 1);
    }
    for (int b = 0; b < bn; b++) {
      OuiKvR r; ouiKvBtnRect(b, r);
      const uint16_t bg = b == bsel ? OUI_SEL : OUI_CARD;
      ouiKvKey(r, ouiRomFit(bl[b], r.w - 6, 1).c_str(), bg, b == bsel ? OUI_TXT : bc[b]);
    }
  }

  char ft[64];
  if (rev && m == 7 && nDf) snprintf(ft, sizeof(ft), "Space: mark  Enter: give/take  Esc: save %d", nDf);
  else if (!rev && m == 6 && nMk) snprintf(ft, sizeof(ft), "%d marked  Space: mark  Enter: change  Esc: clear", nMk);
  else if (!rev && m == 7 && nMk) snprintf(ft, sizeof(ft), "Arrows: pick  Enter: give the %d  Esc: done", nMk);
  else snprintf(ft, sizeof(ft), "%s", rev ? (m == 6 ? "Arrows: pick  Enter: choose  Tab: swap  Esc: back"
                                           : m == 7 ? (joy ? "Space: mark  Enter: give / take  Esc: back"
                                                           : "Space: mark  Enter: give/take  Shift: layer")
                                                    : "Left/Right: SHIFT   Enter: save   Esc: back")
                                     : m == 6 ? (joy && !zxj ? "Space: mark  Enter: change  Tab: swap  Esc: back"
                                                             : "Space: mark  Enter: change  Tab: swap  Shift: layer")
                                     : m == 7 ? "Arrows: pick  Enter: choose  Tab: swap  Esc: back"
                                              : "Left/Right: SHIFT   Enter: save   Esc: back");
  ouiDrawListFoot(ft);
}

static void ouiKmApply(uint8_t code);
static int  ouiKvCkOf(uint8_t c);

// "N keys saved" after the marked keys changed (fit: the map took them all).
static void ouiKvMkSaved(int n, bool fit)
{
  if (!fit) { ouiRomSetMsg("Too many changed keys (64 at most)", true); return; }
  const bool ok = ouiKmSave();
  char m[48];
  if (ok) snprintf(m, sizeof(m), "%d key%s saved", n, n == 1 ? "" : "s");
  else    snprintf(m, sizeof(m), "%d key%s changed, but NOT saved (SD)", n, n == 1 ? "" : "s");
  ouiRomSetMsg(m, !ok);
}

// Swapped, Esc: the keys newly marked get the target, the ones unmarked lose it.
static void ouiKvMkCommit()
{
  const int n = ouiKvMkScan(true);
  if (!n) return;
  bool fit = true;
  for (int l = 0; l < 2; l++)
    for (int i = 0; i < OUI_KV_NPK; i++) {
      uint8_t h; bool s;
      if (ouiKvMk[l][i] == ouiKvMk0[l][i] || !ouiKvTarget(i, l, h, s)) continue;
      const uint8_t c = ouiKvMk[l][i] ? ouiKvGiveCode(h, s, ouiKvCkSel) : ouiKvTakeCode(h, s, ouiKvCkSel);
      if (!ouiKmSet(h, s, c)) fit = false;
    }
  memcpy(ouiKvMk0, ouiKvMk, sizeof(ouiKvMk0));
  ouiKvMkSaved(n, fit);
}

// Forward, keys marked: rule code goes to every one (a SHIFT+ key takes AS TYPED as +SHIFT, as
// its own SHIFT step would), and the page stays on the target picture until Esc.
static bool ouiKvMkApply(uint8_t code)
{
  const int n = ouiKvRev ? 0 : ouiKvMkScan(false);
  if (!n) return false;
  bool fit = true;
  for (int l = 0; l < 2; l++)
    for (int i = 0; i < OUI_KV_NPK; i++) {
      uint8_t h; bool s;
      if (!ouiKvMk[l][i] || !ouiKvTarget(i, l, h, s)) continue;
      uint8_t c = code;
      if (s && ouiKmIsKey(c) && ouiKmShf(c) == C64K_SHIFT_KEEP) c = ouiKmMake(ouiKmIdx(c), C64K_SHIFT_ON);
      if (!ouiKmSet(h, s, c)) fit = false;
    }
  if (ouiCfgMode == 8) ouiKvCkSel = ouiKvCkOf(code);   // back on the key given
  ouiCfgMode = 7; optionsUiDirty = true;
  ouiKvMkSaved(n, fit);
  return true;
}

static void ouiKvOpen()
{
  ouiKvLayer = false; ouiKvRev = false; ouiKvNavX = -1; ouiRomMsg[0] = 0;
  ouiKvMkClear();
  ouiCfgGoto(6, 0);
}

// The C64 picture's entry for rule code c, else the DEFAULT / NONE button.
static int ouiKvCkOf(uint8_t c)
{
  if (c == C64K_UNMAP) return ouiKvNck() + 1;
  if (ouiKmSimple()) {
    for (int i = 0; i < ouiKvNck(); i++) if (ouiKvTab()[i].code == c) return i;
    return ouiKvNck();
  }
  const OuiKvCk *t = ouiKvCkTab();
  for (int i = 0; i < ouiKvCkN(); i++) {
    const uint8_t k = t[i].code;
    if (t[i].flags & OUI_KV_LOCK) continue;
    if (c == C64K_RESTORE ? k == C64K_RESTORE : (ouiKmIsKey(c) && k == ouiKmIdx(c))) return i;
  }
  return ouiKvCkN();
}

// Give the key being changed (ouiKmHid / ouiKmShift) target ck: saved at once, or for a C64 key
// on to its SHIFT (mode 8).
static void ouiKvGive(int ck)
{
  if (ouiKmSimple()) { ouiKmApply(ouiKvTab()[ck].code); return; }  // no SHIFT step
  const uint8_t code = ouiKvCkTab()[ck].code;
  if (code == C64K_RESTORE) { ouiKmApply(C64K_RESTORE); return; }
  if (ouiKmA2() && a2kIsPlainOnly(code)) { ouiKmApply(code); return; }   // CAPS LOCK / the apple keys: no SHIFT step
  ouiKmKey = code;
  const int sm = ouiKvShPre(ouiKmHid, ouiKmShift);
  ouiKvShSel = ouiKmShift ? sm - 1 : sm;
  ouiCfgMode = 8; optionsUiDirty = true;
}

// Enter on PicoCalc key ouiKvPkSel: the Shift keys switch the layer; false for a key with no rule.
static bool ouiKvTakeHost()
{
  const OuiKvPk &p = kOuiKvPk[ouiKvPkSel];
  if (p.hid == OUI_KV_SHIFT && !ouiKmJoyShift()) { ouiKvLayer = !ouiKvLayer; optionsUiDirty = true; return false; }
  uint8_t h; bool s;
  if (!ouiKvTarget(ouiKvPkSel, ouiKvLayer, h, s)) return false;   // the info line says why
  ouiKmHid = h; ouiKmShift = s; ouiRomMsg[0] = 0;
  return true;
}

static void ouiKvEnter()
{
  ouiKvNavX = -1;
  if (ouiCfgMode == 8) { ouiKmApply(ouiKmMake(ouiKmKey, ouiKmShift ? ouiKvShSel + 1 : ouiKvShSel)); return; }
  if (ouiKvRev) {
    // Swapped: mode 6 the target is picked, mode 7 the key gets it - or, if it has it, loses it
    // (back to its default, or NONE when the default is this target too).
    // The keys that press it start marked (SPACE changes them, Esc saves).
    if (ouiCfgMode == 6) { ouiRomMsg[0] = 0; ouiKvMkHits(); ouiCfgMode = 7; optionsUiDirty = true; return; }
    if (!ouiKvTakeHost()) return;
    if (ouiKvHits(ouiKmLookup(ouiKmHid, ouiKmShift), ouiKvCkSel)) {
      ouiKmApply(ouiKvTakeCode(ouiKmHid, ouiKmShift, ouiKvCkSel));
      return;
    }
    ouiKvGive(ouiKvCkSel);
    return;
  }
  if (ouiCfgMode == 6) {
    // Keys marked: they all get the target (shown: the marked one under the cursor, else the
    // first). Enter on Shift still switches the layer.
    const bool lay = kOuiKvPk[ouiKvPkSel].hid == OUI_KV_SHIFT && !ouiKmJoyShift();
    if (!lay && ouiKvMkFirst(ouiKmHid, ouiKmShift)) ouiRomMsg[0] = 0;
    else if (!ouiKvTakeHost()) return;
    ouiKvCkSel = ouiKvCkOf(ouiKmLookup(ouiKmHid, ouiKmShift));
    ouiCfgMode = 7; optionsUiDirty = true;
    return;
  }
  if (ouiKvCkSel == ouiKvNck())     { ouiKmApply(C64K_NONE); return; }    // back to the default
  if (ouiKvCkSel == ouiKvNck() + 1) { ouiKmApply(C64K_UNMAP); return; }
  ouiKvGive(ouiKvCkSel);
}

// TAB: swap the direction and the pictures, keeping the cursor on what it was about - a key goes
// to the target it presses, a target to the first key that presses it.
static void ouiKvSwap()
{
  const int m = ouiCfgMode;
  if (!ouiKvRev) {
    int ck = -1;
    if (m != 6) ck = ouiKvCkSel;
    else {
      uint8_t h; bool s;
      if (ouiKvTarget(ouiKvPkSel, ouiKvLayer, h, s)) ck = ouiKvCkOf(ouiKmLookup(h, s));
    }
    ouiKvCkSel = ck >= 0 && ck < ouiKvNck() ? ck : 0;
  } else if (m == 6) {
    bool found = false;
    for (int l = 0; l < 2 && !found; l++) {
      const bool layer = l ? !ouiKvLayer : ouiKvLayer;
      for (int i = 0; i < OUI_KV_NPK && !found; i++) {
        uint8_t h; bool s;
        if (!ouiKvTarget(i, layer, h, s) || !ouiKvHits(ouiKmLookup(h, s), ouiKvCkSel)) continue;
        ouiKvPkSel = i; ouiKvLayer = layer; found = true;
      }
    }
  }
  ouiKvRev = !ouiKvRev;
  ouiKvNavX = -1; ouiRomMsg[0] = 0;
  ouiKvMkClear();
  ouiCfgMode = 6; optionsUiDirty = true;
}

static void ouiKvArrow(int dx, int dy)
{
  if (ouiKvOnHost()) ouiKvPkSel = ouiKvMove(OUI_KV_NPK, ouiKvPkRect, ouiKvPkOk, ouiKvPkSel, dx, dy);
  else if (ouiKvOnTgt() && ouiKmSimple()) {
    const int8_t (*bmv)[4] = ouiKmMsxJ() ? kOuiKvMjBtnMv : ouiKmA2J() ? kOuiKvA2jBtnMv : ouiKmZxJ() ? kOuiKvZjBtnMv : ouiKmJoy() ? kOuiKvJkBtnMv : ouiKmSms() ? kOuiKvSkBtnMv : ouiKmCol() ? kOuiKvOkBtnMv : ouiKmAtari() ? kOuiKvAkBtnMv : kOuiKvNkBtnMv;
    const int8_t *mv = ouiKvCkSel < ouiKvNck() ? ouiKvTab()[ouiKvCkSel].mv : bmv[ouiKvCkSel - ouiKvNck()];
    const int to = mv[dx < 0 ? 0 : dx > 0 ? 1 : dy < 0 ? 2 : 3];
    if (!ouiKvRev || to < ouiKvNck()) ouiKvCkSel = to;   // swapped: no DEFAULT / NONE buttons
  }
  else if (ouiKvOnTgt()) ouiKvCkSel = ouiKvMove(ouiKvRev ? ouiKvCkN() : ouiKvCkN() + 2, ouiKvCkRect, ouiKvAnyOk, ouiKvCkSel, dx, dy);
  else if (dx) {
    const int n = ouiKvBtnCount();
    ouiKvShSel = (ouiKvShSel + dx + n) % n;
  }
  ouiRomMsg[0] = 0;
  optionsUiDirty = true;
}

// Esc: one step back (SHIFT -> the second picture -> the first). False in mode 6 (the caller goes
// back to the list).
static bool ouiKvBack()
{
  ouiKvNavX = -1;
  if (ouiCfgMode == 8) {
    if (!ouiKvRev) ouiKvCkSel = ouiKvCkOf(ouiKmKey);
    ouiCfgMode = 7; optionsUiDirty = true; return true;
  }
  if (ouiCfgMode == 7) {
    ouiRomMsg[0] = 0;
    if (ouiKvRev) ouiKvMkCommit();   // swapped: the marks changed are saved
    ouiKvMkClear();
    ouiCfgMode = 6; optionsUiDirty = true; return true;
  }
  if (!ouiKvRev && ouiKvMkScan(false)) { ouiKvMkClear(); ouiRomMsg[0] = 0; optionsUiDirty = true; return true; }   // forward: unmark all
  return false;
}

// A key typed on the PicoCalc picture moves the cursor to it (and to its layer). The keys that
// drive the page (arrows, Enter, Esc, F10) are left to it.
static bool ouiKvCapture(uint8_t hid, bool shift)
{
  if (hid == 0x2B) { ouiKvSwap(); return true; }   // TAB: swap the direction
  if (hid == 0x2C) {   // SPACE: mark / unmark the key under the cursor
    uint8_t h; bool s;
    if (ouiKvOnHost() && ouiKvTarget(ouiKvPkSel, ouiKvLayer, h, s)) {
      ouiKvMkSet(h, s, !ouiKvMk[ouiKvLayer][ouiKvPkSel], false);
      ouiRomMsg[0] = 0; optionsUiDirty = true;
    }
    return true;
  }
  if (!ouiKvOnHost()) return false;
  // SHIFT pressed on its own (C64K_HID_SHIFT) flips the PicoCalc picture to its SHIFT+ layer; on
  // the joystick page, where SHIFT is a key to map, it goes to the Shift key.
  if (hid == C64K_HID_SHIFT && ouiKmJoyShift()) {
    for (int i = 0; i < OUI_KV_NPK; i++) if (kOuiKvPk[i].hid == OUI_KV_SHIFT) { ouiKvPkSel = i; break; }
    ouiKvLayer = false; ouiKvNavX = -1; ouiRomMsg[0] = 0;
    optionsUiDirty = true;
    return true;
  }
  if (hid == C64K_HID_SHIFT) {
    ouiKvLayer = !ouiKvLayer; ouiKvNavX = -1; ouiRomMsg[0] = 0;
    optionsUiDirty = true;
    return true;
  }
  switch (hid) {
    case 0x4F: case 0x50: case 0x51: case 0x52: case 0x28: case 0x58: case 0x29: case 0x43: return false;
  }
  int at = -1; bool layer = shift && (!ouiKmSimple() || ouiKmCol());    // NES / joystick: a shifted key is the plain one
  for (int i = 0; i < OUI_KV_NPK && at < 0; i++)            // the SHIFT layer's own codes first
    if (kOuiKvPk[i].shid == hid) { at = i; layer = true; }
  for (int i = 0; i < OUI_KV_NPK && at < 0; i++)
    if (kOuiKvPk[i].hid == hid) at = i;
  if (at < 0) return false;
  if (hid == C64K_HID_CTRL || hid == C64K_HID_ALT) layer = ouiKvLayer;
  ouiKvPkSel = at; ouiKvLayer = layer; ouiKvNavX = -1; ouiRomMsg[0] = 0;
  optionsUiDirty = true;
  return true;
}
#else
static void ouiKvInvalidate() {}
#endif

static void ouiDrawCfg()
{
  char hdr[96];
  const char *title = "CONFIG", *hint = "Enter/tap: open    Esc/X: back";
  int lblW = 110;
#if OUI_KV
  if (ouiCfgKv()) { ouiDrawKv(); return; }
#endif
  snprintf(hdr, sizeof(hdr), ouiIsNES() ? "NES setup" : ouiIsSms() ? "SMS setup" : ouiIsColeco() ? "Coleco setup" : ouiIsAtari() ? "Atari setup" : ouiIsMsx() ? "MSX setup" : ouiIsApple() ? "Apple setup" : ouiIsZx() ? "ZX setup" : ouiIsPcxt() ? "PC-XT setup" : "C64 setup");
#if BOARD_INPUT_USB
  char k[24];
  ouiKmLabel(ouiKmHid, ouiKmShift, k, sizeof(k));
  switch (ouiCfgMode) {
    case 2:
      title = "KEYBOARD"; lblW = 150;
      hint = ouiKmCapture ? "Press the key to find (any key)" : "Enter/tap: change    Esc/X: back";
      snprintf(hdr, sizeof(hdr), "KEYBOARD KEY\t%s  (green=changed)", ouiKmWhat());
      if (ouiKmJoy()) title = "JOYSTICK";
      break;
    case 3:
      title = ouiKmWhat(); hint = "Enter/2nd tap: choose    Esc/X: back";
      snprintf(hdr, sizeof(hdr), "%s presses:", k);
      break;
    case 4:
      title = ouiKmMsx() ? "MSX SHIFT" : ouiKmA2() ? "APPLE SHIFT" : ouiKmZx() ? "ZX SHIFT" : "C64 SHIFT"; hint = "Enter/2nd tap: choose    Esc/X: back";
      snprintf(hdr, sizeof(hdr), ouiKmMsx() ? "%s = %s, and the MSX's SHIFT is:" : ouiKmA2() ? "%s = %s, and the Apple's SHIFT / CTRL:" : ouiKmZx() ? "%s = %s, and CAPS / SYMBOL SHIFT:" : "%s = %s, and the C64's SHIFT is:",
               k, ouiKpName(ouiKmKey));
      break;
    case 5:
      title = ouiKmJoy() ? "RESET JOYSTICK" : "RESET KEYBOARD"; hint = "Enter/2nd tap: choose    Esc/X: back";
      snprintf(hdr, sizeof(hdr), "Forget the %d changed key(s)", ouiKmChanged());
      break;
  }
#endif
#if A2CAL_ANALOG
  if (ouiCfgMode == OUI_CAL_MODE) {
    title = "CALIBRATE"; lblW = 110;
    hint = apple2JoycalBusy() ? "Enter/fire: next step    Esc/X: cancel" : "Enter/tap: choose    Esc/X: back";
    snprintf(hdr, sizeof(hdr), "Analog joystick: centre, then every edge");
  }
#endif
  ouiDrawList(title, hdr, hint, ouiCfgRow, lblW);
}

static void ouiCfgGoto(uint8_t mode, int sel)
{
  ouiCfgMode = mode; ouiRomSel = sel; ouiRomFirst = 0; ouiRomTapped = -1;
  ouiRomKeepVisible();
  ouiRomRedraw();
}

static void ouiOpenCfg()
{
  ouiRomMsg[0] = 0;
#if BOARD_INPUT_USB
  ouiKmCapture = false;
#endif
  ouiCfgGoto(1, 0);
}

static void ouiCfgClose()
{
#if A2CAL_ANALOG
  apple2JoycalCancel();
#endif
  ouiCfgMode = 0;
#if BOARD_INPUT_USB
  ouiKmCapture = false;
#endif
}

#if BOARD_INPUT_USB
// Set the rule being edited (C64K_NONE = back to the default), save, and go back to the key list.
static void ouiKmApply(uint8_t code)
{
#if OUI_KV
  if (ouiCfgKv() && ouiKvMkApply(code)) return;   // forward, keys marked: all of them
#endif
  const bool fit = ouiKmSet(ouiKmHid, ouiKmShift, code);
  if (ouiCfgKv()) {   // back on the picture; swapped, on the keys (until Esc), the key's mark as it is now
    ouiCfgMode = 6;
#if OUI_KV
    if (ouiKvRev) { ouiCfgMode = 7; ouiKvMkSet(ouiKmHid, ouiKmShift, ouiKvHits(ouiKmLookup(ouiKmHid, ouiKmShift), ouiKvCkSel), true); }
#endif
    optionsUiDirty = true;
  }
  else {
    // Back on the key's row; with "changed only" a key put back to its default leaves the list.
    int r = ouiKmRowOf(ouiKmHid, ouiKmShift);
    if (r < 0) r = ouiKmRow < OUI_KM_FIXED + ouiKmRows() ? ouiKmRow : OUI_KM_FIXED + ouiKmRows() - 1;
    ouiCfgGoto(2, r); ouiCfgCenter();
  }
  if (!fit) { ouiRomSetMsg("Too many changed keys (64 at most)", true); return; }
  const bool ok = ouiKmSave();
  char k[24], m[48];
  ouiKmLabel(ouiKmHid, ouiKmShift, k, sizeof(k));
  if (ok) snprintf(m, sizeof(m), "%s saved", k);
  else    snprintf(m, sizeof(m), "%s changed, but NOT saved (SD)", k);
  ouiRomSetMsg(m, !ok);
}
#endif

static void ouiCfgEnter()
{
  switch (ouiCfgMode) {
    case 1: {
      int it[5], n = ouiCfgItems(it);
      if (ouiRomSel >= n) return;
      ouiRomMsg[0] = 0;
#if BOARD_INPUT_USB
      ouiKmJoyPage = it[ouiRomSel] == OUI_CFG_JOY;   // which map the KEYBOARD pages edit
#endif
#if A2CAL_ANALOG
      if (it[ouiRomSel] == OUI_CFG_CAL) { apple2JoycalCancel(); ouiCfgGoto(OUI_CAL_MODE, OUI_CAL_STEP); return; }
#endif
      if (it[ouiRomSel] == OUI_CFG_ROMS) ouiOpenRoms();
      else if (it[ouiRomSel] == OUI_CFG_HELP) ouiOpenHelp();
#if OUI_KV
      else if (it[ouiRomSel] == OUI_CFG_KEYB || it[ouiRomSel] == OUI_CFG_JOY) ouiKvOpen();
#endif
#if BOARD_INPUT_USB
      else { ouiKmCapture = false; ouiCfgGoto(2, 0); }
#endif
      return;
    }
#if OUI_KV
    case 6: case 7: case 8: ouiKvEnter(); return;
#endif
#if A2CAL_ANALOG
    case OUI_CAL_MODE: ouiCalEnter(); return;
#endif
#if BOARD_INPUT_USB
    case 2: {
      if (ouiRomSel == OUI_KM_FIND) { ouiKmCapture = true; ouiRomMsg[0] = 0; optionsUiDirty = true; return; }
      ouiKmCapture = false;
      if (ouiRomSel == OUI_KM_ONLY) { ouiKmOnly = !ouiKmOnly; ouiRomMsg[0] = 0; ouiCfgGoto(2, OUI_KM_ONLY); return; }
      if (ouiRomSel == OUI_KM_RESET) { ouiRomMsg[0] = 0; ouiCfgGoto(5, 0); return; }   // "No" preselected
      if (!ouiKmAt(ouiRomSel - OUI_KM_FIXED, ouiKmHid, ouiKmShift)) return;
      ouiKmRow = ouiRomSel; ouiRomMsg[0] = 0;
      const uint8_t c = ouiKmRule(ouiKmHid, ouiKmShift, nullptr);
      int sel = 0;
      if (c == C64K_UNMAP) sel = 1;
      else if (c == C64K_RESTORE) sel = 2;
      else for (int i = 0; i < ouiKpCount(); i++) if (ouiKpIs(c, ouiKpAt(i))) sel = ouiKpFixed() + i;
      ouiCfgGoto(3, sel); ouiCfgCenter();
      return;
    }
    case 3: {
      if (ouiRomSel == 0) { ouiKmApply(C64K_NONE); return; }
      if (ouiRomSel == 1) { ouiKmApply(C64K_UNMAP); return; }
      if (ouiKmSimple()) { ouiKmApply(ouiKpAt(ouiRomSel - ouiKpFixed())); return; }   // no SHIFT step
      if (ouiRomSel == 2 && ouiKpFixed() == 3) { ouiKmApply(C64K_RESTORE); return; }
      ouiKmKey = ouiKpAt(ouiRomSel - ouiKpFixed());
      if (ouiKmA2() && a2kIsPlainOnly(ouiKmKey)) { ouiKmApply(ouiKmKey); return; }   // held: no SHIFT step
      const uint8_t c = ouiKmRule(ouiKmHid, ouiKmShift, nullptr);
      // Preselect the key's current SHIFT handling, else the usual one (a SHIFT+ rule: released).
      int m = ouiKmIsKey(c) ? ouiKmShf(c) : (ouiKmShift ? C64K_SHIFT_OFF : C64K_SHIFT_KEEP);
      if (ouiKmShift && m == C64K_SHIFT_KEEP) m = C64K_SHIFT_ON;
      ouiCfgGoto(4, ouiKmShift ? m - 1 : m);
      return;
    }
    case 4: {
      const int m = ouiKmShift ? ouiRomSel + 1 : ouiRomSel;
      ouiKmApply(ouiKmMake(ouiKmKey, m));
      return;
    }
    case 5: {
      if (ouiRomSel == 0) { ouiCfgGoto(2, OUI_KM_RESET); return; }
      ouiKmResetAll();
      const bool ok = ouiKmSave();
      ouiCfgGoto(2, OUI_KM_RESET);
      ouiRomSetMsg(ok ? "Every key back to the default - saved" : "Reset, but NOT saved (SD)", !ok);
      return;
    }
#endif
  }
}

// Esc / X: one level back; from the list, back to the settings page.
static void ouiCfgBack()
{
  switch (ouiCfgMode) {
#if BOARD_INPUT_USB
    case 2:
      if (ouiKmCapture) { ouiKmCapture = false; optionsUiDirty = true; return; }
      ouiRomMsg[0] = 0; ouiCfgGoto(1, ouiCfgRowOf(ouiKmItem()));
      return;
    case 3: ouiRomMsg[0] = 0; ouiCfgGoto(2, ouiKmRow); ouiCfgCenter(); return;
    case 4:
      for (int i = 0; i < ouiKpCount(); i++) if (ouiKpAt(i) == ouiKmKey) { ouiCfgGoto(3, ouiKpFixed() + i); break; }
      ouiCfgCenter();
      return;
    case 5: ouiCfgGoto(2, OUI_KM_RESET); return;
#endif
#if OUI_KV
    case 6: case 7: case 8:
      if (ouiKvBack()) return;
      ouiRomMsg[0] = 0; ouiCfgGoto(1, ouiCfgRowOf(ouiKmItem()));
      return;
#endif
#if A2CAL_ANALOG
    case OUI_CAL_MODE:
      ouiRomMsg[0] = 0;
      if (apple2JoycalBusy()) {
        apple2JoycalCancel(); ouiRomSel = OUI_CAL_STEP;
        ouiRomSetMsg("Cancelled - kept the old calibration", false);
        return;
      }
      ouiCfgGoto(1, ouiCfgRowOf(OUI_CFG_CAL));
      return;
#endif
    default: ouiCfgClose(); ouiRomMsg[0] = 0; ouiRomRedraw(); return;
  }
}

static void ouiCfgTap(int16_t x, int16_t y)
{
  if (y < OUI_TITLE_H && x >= 320 - OUI_TITLE_H) { ouiCfgBack(); return; }   // X = back
  if (ouiCfgKv()) return;                 // the keyboard picture: keys only
  const int listH = OUI_RS_ROWS * OUI_FB_ROWH;
  if (y < OUI_RS_LIST || y >= OUI_RS_LIST + listH) return;
  if (x >= 302) { ouiRomScroll(y < OUI_RS_LIST + listH / 2 ? -1 : 1); return; }
  int idx = ouiRomFirst + (y - OUI_RS_LIST) / OUI_FB_ROWH;
  if (idx >= ouiRomCount()) return;
  // Lists open on the first tap; a choice that saves is highlighted first and taken by a second.
  bool now = ouiCfgMode == 1 || (ouiCfgMode == 5 && idx == 0)
#if A2CAL_ANALOG
             || (ouiCfgMode == OUI_CAL_MODE && (idx == OUI_CAL_STEP || apple2JoycalBusy()))
#endif
#if BOARD_INPUT_USB
             || (ouiCfgMode == 2 && idx != OUI_KM_RESET)
             || (ouiCfgMode == 3 && idx >= ouiKpFixed() && !ouiKmSimple())
#endif
             ;
  ouiRomSel = idx;
  if (now || ouiRomTapped == idx) { ouiRomTapped = -1; ouiCfgEnter(); return; }
  ouiRomTapped = idx;
  optionsUiDirty = true;
}

// A key typed while the menu is open (usbkeyboard.cpp), before it navigates: on the KEYBOARD page
// with "press a key to find it" armed it is taken here and selects that key's row (its SHIFT+ row
// when SHIFT is down). CTRL / ALT come as C64K_HID_CTRL / C64K_HID_ALT. True = consumed.
bool optionsUiKeyCapture(uint8_t hid, bool shift)
{
#if BOARD_INPUT_USB
#if OUI_KV
  if (ouiCfgKv() && !ouiRomMode && !ouiHelpOpen) return ouiKvCapture(hid, shift);
#endif
  if (hid == C64K_HID_SHIFT && !ouiKmJoyShift()) return false;   // a bare SHIFT: the picture's, or a joystick key
  if (ouiCfgMode != 2 || !ouiKmCapture || ouiRomMode || ouiHelpOpen) return false;
  ouiKmCapture = false;
  int r = ouiKmRowOf(hid, shift);
  if (r < 0 && ouiKmOnly) { ouiKmOnly = false; r = ouiKmRowOf(hid, shift); }   // unchanged: show all keys
  if (r < 0) {
    char m[48];
    snprintf(m, sizeof(m), "That key (0x%02X) cannot be mapped", hid);
    ouiRomSetMsg(m, true);
    optionsUiDirty = true;
    return true;
  }
  ouiRomSel = r; ouiRomTapped = -1; ouiRomMsg[0] = 0;
  ouiCfgCenter();
  return true;
#else
  (void)hid; (void)shift;
  return false;
#endif
}

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

// "/emu8/c64/snapshot/0007-GIANA.snp" -> num = "0007", name = "GIANA"
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
  ouiHelpOpen = false; ouiRomClose(); ouiCfgClose(); ouiEditing = false;
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
  ouiRomClose(); ouiCfgClose();
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
#if A2CAL_ANALOG
  // CALIBRATE shows the stick live: repaint when it moves (throttled; only changed rows redraw).
  if (ouiCfgMode == OUI_CAL_MODE && !ouiRomMode && !ouiHelpOpen && apple2JoycalChanged()) optionsUiDirty = true;
#endif
  if (!optionsUiDirty) return;
  if (optionsUiFirstDraw) { tft.fillScreen(OUI_BG); ouiInvalidate(); optionsUiFirstDraw = false; }
  if (ouiGenMode)  { ouiDrawGeneral(); optionsUiDirty = false; return; }
  if (ouiSnapMode) { optionsUiDirty = false; ouiDrawSnap(); return; }   // (a thumbnail retry re-arms it)
  if (ouiHelpOpen) { ouiDrawHelp(); optionsUiDirty = false; return; }
  if (ouiRomMode)  { ouiDrawRoms(); optionsUiDirty = false; return; }
  if (ouiCfgMode)  { ouiDrawCfg(); optionsUiDirty = false; return; }
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
static void ouiToggleScreenFill() { screenFill = (uint8_t)((screenFill + 1) % SCREEN_MODES); optionsUiDirty = true; }
#endif

// Any grid slot (0..7): platform toggles, SCREEN and HELP / CONFIG.
static void ouiToggle(int idx)
{
  if (idx == OUI_FOC_HELP) { if (ouiHasCfg()) ouiOpenCfg(); else ouiOpenHelp(); return; }   // grid slot 7 = HELP / CONFIG
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
    case 3:   // ARROWS: JOY -> KBD -> BOTH
      appleArrows = (uint8_t)((appleArrows + 1) % APPLE_ARROWS_MODES);
      joystick = (appleArrows != APPLE_ARROWS_KBD);
      break;
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
#if OUI_KV
  if (ouiCfgKv()) { ouiKvArrow(dir, 0); return; }                         // keyboard picture
#endif
  if (ouiCfgMode)  { if (dir < 0) ouiCfgBack(); else ouiCfgEnter(); return; }   // C64 CONFIG page
  for (int i = 0; i < OUI_FOC_COUNT; i++) {
    optionsUiFocus = (optionsUiFocus + dir + OUI_FOC_COUNT) % OUI_FOC_COUNT;
    if ((optionsUiFocus != OUI_FOC_HELP || ouiHasCfg()) && ouiFocusable(optionsUiFocus)) break;   // C64 / NES: CONFIG
  }
  optionsUiDirty = true;
}

void optionsUiAdjust(int dir)         // dir: -1 = up, +1 = down
{
  if (ouiHelpOpen) { ouiCloseHelp(); return; }
  if (ouiSnapMode) { ouiSnapMove(dir < 0 ? -1 : 1); return; }
#if OUI_KV
  if (!ouiRomMode && ouiCfgKv()) { ouiKvArrow(0, dir < 0 ? -1 : 1); return; }
#endif
  if (ouiRomMode || ouiGenMode || ouiCfgMode) { ouiRomMove(dir < 0 ? -1 : 1); return; }
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
  if (ouiCfgMode)  { ouiCfgEnter(); return; }
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
#if OUI_KV
  if (!ouiRomMode && ouiCfgKv()) { ouiKvArrow(dx, dy); return; }   // the keyboard picture
#endif
  if (ouiRomMode || ouiGenMode || ouiCfgMode) {   // list pages: Up/Down move, Left/Right page
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
  if (ouiCfgMode)  { ouiCfgEnter(); return; }
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
  if (ouiHelpOpen || ouiSnapMode || ouiRomMode || ouiGenMode || ouiCfgMode || optionsUiFocus != OUI_FOC_FILES) return;
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
  if (ouiGenMode || ouiHelpOpen || ouiRomMode || ouiCfgMode) return;
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
  if (ouiCfgMode)  { ouiCfgBack(); return true; }
  if (ouiEditing)  { ouiEditing = false; optionsUiDirty = true; return true; }
  return false;
}

// HELP overlay open/close. Opening/closing forces a full repaint (the pages don't overlap).
static void ouiOpenHelp()  { ouiHelpOpen = true;  optionsUiFirstDraw = true; optionsUiDirty = true; }
static void ouiCloseHelp()   // back to the settings page, or to CONFIG when it was opened from there
{
  ouiHelpOpen = false; optionsUiFirstDraw = true; optionsUiDirty = true;
  if (ouiCfgMode) ouiRomRedraw();
}

static void ouiHandleTap(int16_t x, int16_t y)
{
  if (ouiGenMode)  { ouiGenTap(x, y); return; }   // general settings (over the system menu)
  if (ouiSnapMode) { ouiSnapTap(x, y); return; }  // snapshot page
  // HELP overlay is modal: any tap returns to the settings page.
  if (ouiHelpOpen) { ouiCloseHelp(); return; }
  if (ouiRomMode)  { ouiRomTap(x, y); return; }   // ROMS page is modal too
  if (ouiCfgMode)  { ouiCfgTap(x, y); return; }   // C64 CONFIG page

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
  ouiCfgClose();                 // ...nor CONFIG
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
