#include "../../emu.h"
#include "filebrowser.h"

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
// for), then its MOUNT / REBOOT buttons, then the option grid and VOLUME. The keyboard arrows
// (ouiNextFocus, further down) move by these positions, so Up/Down walk the page top to bottom.
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
static uint8_t ouiRomMode        = 0;       // ROMS page: 0 closed, 1 ROM list, 2 SD browser for one ROM

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
// MSX with a .dsk highlighted: the action row becomes MOUNT/UNMOUNT | MOUNT&RUN | REBOOT
// (a .rom keeps LOAD & RUN | REBOOT).
static bool ouiMsxDsk()
{
  if (!ouiIsMsx()) return false;
  std::vector<std::string> &fl = ouiFiles();
  if (shownFile >= fl.size() || ouiIsDir(fl[shownFile])) return false;
  const std::string &n = fl[shownFile];
  return n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".dsk") == 0;
}
static bool ouiMsxDskCur() { return ouiMsxDsk() && msxDiskMounted(ouiFiles()[shownFile].c_str()); }
// MSX with the LOADED cartridge highlighted: the row becomes UNMOUNT | LOAD & RUN | REBOOT.
static bool ouiMsxCartCur()
{
  if (!ouiIsMsx()) return false;
  std::vector<std::string> &fl = ouiFiles();
  return shownFile < fl.size() && !ouiIsDir(fl[shownFile]) && msxCartLoaded(fl[shownFile].c_str());
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
  tft.drawString(value, x + 8, y + OUI_TG_H - 5, 2);
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

// S3 panel + desktop only: a SCREEN toggle (drawn after the platform toggles clear the rest).
// FILL = video scaled to fill the panel (keep 4:3); ORIG = centered 320x240 with a border.
static void ouiDrawScreenToggle()
{
#if BOARD_HAS_SCREENFILL
  ouiDrawToggle(ouiScreenSlot(), "SCREEN", screenFill ? "FILL" : "ORIG", OUI_TXT);
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
    ouiDrawToggle(4, "DISK ROM", msxDiskRom ? "ON" : "AUTO",     OUI_TXT);   // AUTO = only with a .dsk mounted
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
      ouiClearToggles(3);   // Atari: no speed control
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
  std::vector<std::string> &files = ouiFiles();
  std::string sel = ouiSel();

  // header: the highlighted entry's problem, when it has one, in place of the list title
  const char *problem = (shownFile < files.size()) ? ouiEntryProblem(shownFile) : nullptr;
  char hdr[64];
  if (problem) snprintf(hdr, sizeof(hdr), "! %s", problem);
  else sprintf(hdr, "%s  (%d)", ouiIsC64() ? "PRG/D64/CRT" : ouiIsNES() ? "NES ROMS"
                         : ouiIsAtari() ? "A26/BIN ROMS"
                         : ouiIsMsx() ? "MSX ROM/DSK"
                         : ouiIsSms() ? "SMS ROMS"
                         : ouiIsColeco() ? "CARTRIDGES"
                         : ouiIsZx() ? "SNA/Z80/TAP"
                         : ouiIsPcxt() ? "PC DISK IMG"
                         : (HdDisk ? "HD IMAGES" : "DISK IMAGES"),
          (int)files.size());
  if (ouiChanged(ouiSigHdr, ouiHash(hdr))) {
    tft.fillRect(0, OUI_FB_TOP, 320, OUI_FB_HDR_H, OUI_BG);
    tft.setTextDatum(BL_DATUM);
    tft.setTextColor(problem ? tft.color565(255, 170, 60) : OUI_LBL, OUI_BG);
    tft.drawString(hdr, 7, OUI_FB_TOP + OUI_FB_HDR_H - 2, 1);
  }

  // The focus ring is drawn over the rows, so turning it off (or recolouring it) means repainting
  // every row underneath; while it stays on, only a repainted row needs it put back.
  const uint8_t ring = (uint8_t)ouiFocusState(OUI_FOC_FILES);
  if (ring != ouiRingDrawn) { ouiInvalidateRows(); ouiRingDrawn = ring; }
  bool rowsDrawn = false;

  int listH = OUI_FB_ROWS * OUI_FB_ROWH;
  if (files.empty()) {
    if (ouiSigRow[0] != OUI_SIG_NOFILES) {
      for (int r = 0; r < OUI_FB_ROWS; r++) ouiSigRow[r] = OUI_SIG_NOFILES;
      tft.fillRect(0, OUI_FB_LIST, 300, listH, OUI_CARD2);
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
        if (ouiChanged(ouiSigRow[r], OUI_SIG_EMPTY)) { tft.fillRect(0, ry, 300, OUI_FB_ROWH, OUI_CARD2); rowsDrawn = true; }
        continue;
      }

      bool selected = (idx == shownFile);
      bool mntA = ouiIsPcxt() && ouiPcA().length() && (files[idx] == std::string(ouiPcA().c_str()));
      bool mntC = ouiIsPcxt() && ouiPcC().length() && (files[idx] == std::string(ouiPcC().c_str()));
      bool mounted  = (files[idx] == sel) || mntA || mntC;
      const bool bad = ouiEntryProblem(idx) != nullptr;
      const char flags[] = { (char)('0' + selected), (char)('0' + mounted), (char)('0' + mntA), (char)('0' + mntC), (char)('0' + bad), 0 };
      if (!ouiChanged(ouiSigRow[r], ouiHash(flags, ouiHash(files[idx].c_str())))) continue;
      rowsDrawn = true;
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

  if (ring && rowsDrawn) ouiFocusRing(0, OUI_FB_LIST, 300, listH, 2);
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
                       : ouiMsxDsk()       ? (ouiMsxDskCur() ? "unmount" : "mount")
                       : ouiMsxCartCur()   ? "unmount"
                       : (currentPlatform == PLATFORM_APPLE2) ? "mount + reboot" : NULL;

  char sig[48];
  snprintf(sig, sizeof(sig), "H|%d|%s|%s", (int)ouiEditing, enterAct, ctrlAct ? ctrlAct : "");
  if (!ouiChanged(ouiSigAct, ouiHash(sig))) return;

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
#endif
  bool canMount = !ouiFiles().empty();
  uint16_t mc = canMount ? OUI_MOUNT : OUI_CARD2;
  uint16_t mt = canMount ? OUI_TXT : OUI_LBL;
  bool curA = false, curC = false;
  if (ouiIsPcxt()) {
    std::vector<std::string> &fl = ouiFiles();
    curA = shownFile < fl.size() && ouiPcA().length() && (fl[shownFile] == std::string(ouiPcA().c_str()));
    curC = shownFile < fl.size() && ouiPcC().length() && (fl[shownFile] == std::string(ouiPcC().c_str()));
  }
  const int f = optionsUiFocus;
  const bool actFocus = f == OUI_FOC_MOUNT || f == OUI_FOC_MNTREBOOT || f == OUI_FOC_REBOOT;
  const bool msxDsk = ouiMsxDsk(), msxCur = msxDsk ? ouiMsxDskCur() : ouiMsxCartCur();
  char sig[48];
  snprintf(sig, sizeof(sig), "B|%d|%d|%d|%d|%d|%d|%d", (int)currentPlatform, (int)canMount, (int)curA, (int)curC,
           (int)msxDsk, (int)msxCur, actFocus ? f : -1);
  if (!ouiChanged(ouiSigAct, ouiHash(sig))) return;
#if defined(BOARD_PICOCALC)
  tft.fillRect(0, OUI_ACT_TOP, 320, OUI_ACT_H, OUI_BG);   // wipe the hint panel from the gaps
#endif

  if (ouiIsPcxt()) {                       // PC-XT: MOUNT/EJECT A: | MOUNT/EJECT C: | REBOOT
    ouiActBtn(4,   102, curA ? "EJECT A:" : "MOUNT A:", curA ? OUI_RED : mc, curA ? OUI_TXT : mt, OUI_FOC_MOUNT);
    ouiActBtn(109, 102, curC ? "EJECT C:" : "MOUNT C:", curC ? OUI_RED : mc, curC ? OUI_TXT : mt, OUI_FOC_MNTREBOOT);
    ouiActBtn(214, 102, "REBOOT",   OUI_REBOOT, OUI_TXT, OUI_FOC_REBOOT);
    return;
  }
  if (msxDsk) {                            // MSX .dsk: MOUNT/UNMOUNT (live) | MOUNT&RUN (boot it) | REBOOT
    ouiActBtn(4,   102, msxCur ? "UNMOUNT" : "MOUNT", msxCur ? OUI_RED : mc, msxCur ? OUI_TXT : mt, OUI_FOC_MOUNT);
    ouiActBtn(109, 102, "MOUNT&RUN", mc,         mt,      OUI_FOC_MNTREBOOT);
    ouiActBtn(214, 102, "REBOOT",    OUI_REBOOT, OUI_TXT, OUI_FOC_REBOOT);
    return;
  }
  if (msxCur) {                            // MSX, loaded cart highlighted: UNMOUNT (+reset) | LOAD & RUN | REBOOT
    ouiActBtn(4,   102, "UNMOUNT",    OUI_RED,    OUI_TXT, OUI_FOC_MOUNT);
    ouiActBtn(109, 102, "LOAD & RUN", mc,         mt,      OUI_FOC_MNTREBOOT);
    ouiActBtn(214, 102, "REBOOT",     OUI_REBOOT, OUI_TXT, OUI_FOC_REBOOT);
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
  const char *title = ouiIsC64() ? "COMMODORE 64"
               : ouiIsNES() ? "NINTENDO  NES"
               : ouiIsAtari() ? "ATARI 2600"
               : ouiIsMsx() ? "MSX1"
               : ouiIsSms() ? "MASTER SYSTEM"
               : ouiIsColeco() ? "COLECOVISION"
               : ouiIsZx() ? "ZX SPECTRUM 48K"
               : ouiIsPcxt() ? "PC-XT"
               : AppleIIe ? "APPLE IIe" : "APPLE II+";
  // Every CPU speed readout lives here, right-aligned before the close button (Atari has none).
  char mhz[20] = "";
  if (currentPlatform == PLATFORM_APPLE2)       // live, measured in cpuLoop
    snprintf(mhz, sizeof(mhz), "6502  %.2f MHz", appleMeasuredMhz);
  else if (ouiIsC64())                          // live, measured in cpuLoop
    snprintf(mhz, sizeof(mhz), "6510  %.2f MHz", c64MeasuredMhz);
  else if (ouiIsNES())                          // derived from fps x cycles-per-frame
    snprintf(mhz, sizeof(mhz), "2A03  %.2f MHz", nesMeasuredMhz);
  else if (ouiIsMsx() || ouiIsSms() || ouiIsColeco() || ouiIsZx())   // uncapped Z80, boot benchmark
    snprintf(mhz, sizeof(mhz), "Z80  %.2f MHz", ouiIsMsx() ? msxMeasuredMhz : ouiIsSms() ? smsMeasuredMhz
                                              : ouiIsZx() ? zxMeasuredMhz : colecoMeasuredMhz);
  else if (ouiIsPcxt())                         // 8086-equivalent, boot benchmark
    snprintf(mhz, sizeof(mhz), "8086  %.2f MHz", pcMeasuredMhz);
  if (!ouiChanged(ouiSigTitle, ouiHash(mhz, ouiHash(title)))) return;

  tft.fillRect(0, 0, 320, OUI_TITLE_H, OUI_TITLE);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(OUI_TXT, OUI_TITLE);
  tft.drawString(title, 10, OUI_TITLE_H / 2, 2);
  int cw = OUI_TITLE_H, cx = 320 - cw;
  if (mhz[0]) {
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

// --- ROMS page: pick the SD-card file each system ROM loads from (romsel.cpp) ---
// Mode 1 lists the ROMs the running machine needs with the file each one will load; Enter (or a
// tap) opens an SD browser for that ROM (mode 2), whose first row puts the ROM back on its default
// name. A picked file is size-checked against what the loader accepts and saved to EEPROM at once;
// the loaders only read it at boot, so it takes effect on the next reboot.
#define OUI_RS_HDR    (OUI_TITLE_H + 2)
#define OUI_RS_LIST   (OUI_TITLE_H + 20)
#define OUI_RS_FOOT   (OUI_SCR_H - 32)
#define OUI_RS_ROWS   ((OUI_RS_FOOT - 2 - OUI_RS_LIST) / OUI_FB_ROWH)
#if defined(BOARD_PICOCALC)
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
static std::vector<std::string> ouiRomFiles;
static FileBrowser ouiRomBrowser = { "ROMS", &ouiRomFiles, nullptr, nullptr, 200, "/" };

// The slots shown in mode 1, in table order.
static int ouiRomSlots(int *out)
{
  int n = 0;
  for (int i = 0; i < ROMSEL_COUNT; i++) if (romSlotVisible(i)) out[n++] = i;
  return n;
}

static int ouiRomCount()
{
  if (ouiRomMode == 2) return 1 + (int)ouiRomFiles.size();   // row 0 = "use default"
  int s[ROMSEL_COUNT];
  return ouiRomSlots(s);
}

static int ouiRomTextW(const char *s, int font)
{
#if defined(BOARD_PICOCALC)
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
  std::vector<std::string>().swap(ouiRomFiles);
}

static void ouiRomKeepVisible()
{
  if (ouiRomSel < ouiRomFirst) ouiRomFirst = ouiRomSel;
  else if (ouiRomSel >= ouiRomFirst + OUI_RS_ROWS) ouiRomFirst = ouiRomSel - OUI_RS_ROWS + 1;
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
  int maxFirst = ouiRomCount() - OUI_RS_ROWS;
  if (maxFirst < 0) maxFirst = 0;
  ouiRomFirst += d * (OUI_RS_ROWS - 1);
  if (ouiRomFirst > maxFirst) ouiRomFirst = maxFirst;
  if (ouiRomFirst < 0) ouiRomFirst = 0;
  if (ouiRomSel < ouiRomFirst) ouiRomSel = ouiRomFirst;
  if (ouiRomSel >= ouiRomFirst + OUI_RS_ROWS) ouiRomSel = ouiRomFirst + OUI_RS_ROWS - 1;
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
  std::vector<std::string>().swap(ouiRomFiles);
  tft.fillScreen(OUI_BG); ouiInvalidate(); ouiRomRedraw(); optionsUiFirstDraw = false;
  ouiDrawRoms();                             // the scan's progress bar then sits on this page
  fbScan(ouiRomBrowser);
  for (int i = 0; i < (int)ouiRomFiles.size(); i++)   // preselect the current pick if it is here
    if (ouiRomFiles[i] == std::string(cur.c_str())) { ouiRomSel = i + 1; break; }
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
    std::vector<std::string>().swap(ouiRomFiles);
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

static void ouiRomPick(const char *path)   // path == nullptr: back to the default name
{
  const RomSlotInfo &si = romSlotInfo(ouiRomSlot);
  char m[48];
  if (path) {
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
  int i = ouiRomSel - 1;
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
  bool dirRow = ouiRomMode == 2 && idx > 0 && ouiIsDir(ouiRomFiles[idx - 1]);
  ouiRomSel = idx;
  if (ouiRomMode == 1 || dirRow || ouiRomTapped == idx) { ouiRomTapped = -1; ouiRomEnter(); return; }
  ouiRomTapped = idx;
  optionsUiDirty = true;
}

static void ouiDrawRoms()
{
  if (ouiChanged(ouiSigTitle, ouiHash(ouiRomMode == 1 ? "ROMS1" : "ROMS2"))) {
    tft.fillRect(0, 0, 320, OUI_TITLE_H, OUI_TITLE);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(OUI_TXT, OUI_TITLE);
    tft.drawString(ouiRomMode == 1 ? "SYSTEM ROMS" : "PICK ROM FILE", 10, OUI_TITLE_H / 2, 2);
    int cw = OUI_TITLE_H, cx = 320 - cw;
    tft.fillRect(cx, 0, cw, OUI_TITLE_H, OUI_RED);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(OUI_TXT, OUI_RED);
    tft.drawString("X", cx + cw / 2, OUI_TITLE_H / 2, 2);
  }

  char hdr[96];
  if (ouiRomMode == 1) snprintf(hdr, sizeof(hdr), "File each ROM loads at boot");
  else snprintf(hdr, sizeof(hdr), "%s: %s", romSlotInfo(ouiRomSlot).label, ouiRomBrowser.dir.c_str());
  if (ouiChanged(ouiSigHdr, ouiHash(hdr))) {
    tft.fillRect(0, OUI_RS_HDR, 320, OUI_RS_LIST - OUI_RS_HDR, OUI_BG);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(OUI_LBL, OUI_BG);
    tft.drawString(ouiRomFit(hdr, 306, 1).c_str(), 7, OUI_RS_HDR + (OUI_RS_LIST - OUI_RS_HDR) / 2 - 1, 1);
  }

  int slots[ROMSEL_COUNT], ns = ouiRomSlots(slots);
  const int count = ouiRomCount();
  for (int r = 0; r < OUI_RS_ROWS; r++) {
    const int idx = ouiRomFirst + r, ry = OUI_RS_LIST + r * OUI_FB_ROWH;
    char a[24] = "", b[80] = "";
    uint16_t bc = OUI_TXT;
    if (idx < count) {
      if (ouiRomMode == 1) {
        if (idx < ns) {
          const char *o = romOverride(slots[idx]);
          snprintf(a, sizeof(a), "%s", romSlotInfo(slots[idx]).label);
          if (o) { snprintf(b, sizeof(b), "%s", o); bc = OUI_ON; }
          else   { snprintf(b, sizeof(b), "%s (default)", romSlotInfo(slots[idx]).defPath); bc = OUI_LBL; }
        }
      } else if (idx == 0) {
        const char *d = romSlotInfo(ouiRomSlot).defPath, *base = strrchr(d, '/');
        snprintf(b, sizeof(b), "< use default: %s >", base ? base + 1 : d);
        bc = OUI_REBOOT;
      } else {
        const std::string &e = ouiRomFiles[idx - 1];
        snprintf(b, sizeof(b), "%s", ouiDisplayName(e).c_str());
        bc = ouiIsDir(e) ? tft.color565(90, 200, 255)
           : (e == std::string(romPath(ouiRomSlot))) ? OUI_ON : OUI_TXT;
      }
    }
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
      bx = OUI_RS_LBLW;
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
  const char *hint = ouiRomMode == 1 ? "Enter/tap: choose file    Esc/X: back"
                                     : "Enter/2nd tap: use file    Esc/X: back";
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

void optionsUiRender()
{
#if defined(BOARD_PICOCALC)
  // Normally already on from optionsUiOpen(); if anything switched it off, repaint all of it.
  if (tft.setFullPanel(true)) { optionsUiFirstDraw = true; optionsUiDirty = true; }
#endif
  if (!optionsUiDirty) return;
  if (optionsUiFirstDraw) { tft.fillScreen(OUI_BG); ouiInvalidate(); optionsUiFirstDraw = false; }
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
static void ouiToggleScreenFill() { screenFill = !screenFill; optionsUiDirty = true; }
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
      case 4: msxDiskRom = !msxDiskRom; msxApplyDiskRom(); break;   // AUTO <-> ON (resets the MSX)
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
      case 3: if (!ouiIsNES()) return;        // NES: SPEED NORMAL <-> FAST
              nesFast = !nesFast; break;
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
  // Nothing listed (a failed scan): Enter still goes up a level. At the root that is a no-op.
  if (files.empty()) { ouiBrowse(".."); return; }
  if (shownFile >= files.size()) return;
  // Every browser can be sitting inside a subdirectory now, so a highlighted ".." or "name/"
  // row means navigate, never mount. Checked once here instead of in each per-core branch.
  if (ouiIsDir(files[shownFile])) { ouiBrowse(files[shownFile]); return; }
  if (ouiIsC64()) {                       // C64: load the highlighted image (.prg/.d64/.crt) + run
    if (shownFile >= files.size()) return;
    if (ouiEntryProblem(shownFile)) { optionsUiDirty = true; return; }   // greyed: reason is in the header
    if (!c64LoadAndRun(files[shownFile].c_str())) {
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

// MSX .dsk: MOUNT swaps the disk into the running machine, or UNMOUNTs it if it is the mounted one
// (stays in settings so the button flips); MOUNT & RUN boots it. Both close settings on success.
static void ouiMsxMount(bool run)
{
  std::vector<std::string> &files = ouiFiles();
  if (files.empty() || shownFile >= files.size()) return;
  if (ouiIsDir(files[shownFile])) { ouiBrowse(files[shownFile]); return; }  // dir row -> navigate
  if (!run && ouiMsxDskCur()) { msxUnmountDisk(); optionsUiDirty = true; return; }
  if (ouiMsxCartCur()) {                   // loaded cart: UNMOUNT (resets) -> close, back to BASIC
    msxUnloadCart(); showHideOptionsWindow(); return;
  }
  if (msxMountDisk(files[shownFile].c_str(), run)) showHideOptionsWindow();
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
    if (ouiIsAtari()) return f <= 2;
    if (ouiIsNES())   return f <= 3 || (BOARD_DISPLAY_GFX && f == 4);             // 4 = SKIP (S3)
    return f <= 4;                                                                // C64, Apple II
  }
  if (f == OUI_FOC_MNTREBOOT) return ouiIsPcxt() || currentPlatform == PLATFORM_APPLE2 || ouiMsxDsk() || ouiMsxCartCur();
  return true;
}

// ---- Joystick navigation (called from joystick.ino, core 0) ----
// Left/right move the focus; up/down act on the focused control; fire activates it.
void optionsUiNav(int dir)            // dir: -1 = left, +1 = right
{
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
  if (ouiRomMode)  { ouiRomMove(dir < 0 ? -1 : 1); return; }
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
  if (ouiRomMode)  { ouiRomEnter(); return; }
  int f = optionsUiFocus;
  if (f >= 0 && f < OUI_TG_COUNT)  ouiToggle(f);   // incl. SCREEN, ROMS, HELP
  else if (f == OUI_FOC_FILES)     ouiMount();
  else if (f == OUI_FOC_MOUNT)     { if (ouiIsPcxt()) ouiPcMountA(); else if (ouiMsxDsk() || ouiMsxCartCur()) ouiMsxMount(false); else ouiMount(); }
  else if (f == OUI_FOC_MNTREBOOT) { if (ouiIsPcxt()) ouiPcMountC(); else if (ouiMsxDsk()) ouiMsxMount(true);
                                     else if (ouiMsxCartCur()) ouiMount(); else ouiMountReboot(); }
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

// Whether the action row is LOAD & RUN | REBOOT (two buttons) rather than three. Mirrors ouiDrawActions.
static bool ouiActTwoButtons()
{
  if (ouiIsPcxt() || ouiMsxDsk() || ouiMsxCartCur()) return false;
  return ouiIsC64() || ouiIsNES() || ouiIsAtari() || ouiIsMsx() || ouiIsSms() || ouiIsColeco() || ouiIsZx();
}

// Screen rectangle of control f as drawn now, or false if it is not on the page.
static bool ouiFocusRect(int f, int &x, int &y, int &w, int &h)
{
  if (f >= 0 && f < OUI_TG_COUNT) {
    x = (f % 4) * OUI_TG_W; y = OUI_TG_TOP + (f / 4) * OUI_TG_H; w = OUI_TG_W; h = OUI_TG_H;
    return true;
  }
  if (f == OUI_FOC_VOL)   { x = 0; y = OUI_VOL_TOP; w = 320; h = OUI_VOL_H; return true; }
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
  if (ouiHelpOpen) { ouiCloseHelp(); return; }
  if (ouiRomMode) {                       // ROMS page: Up/Down move, Left/Right page
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

void optionsUiKeyEnter(bool ctrl)
{
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
    else if (ouiMsxDsk() || ouiMsxCartCur()) ouiMsxMount(false);   // MSX: .dsk MOUNT/UNMOUNT, loaded cart UNMOUNT
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
  if (ouiRomMode)  { ouiRomBack(); return true; }
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
  if (ouiRomMode)  { ouiRomTap(x, y); return; }   // ROMS page is modal too

  // close button
  if (y < OUI_TITLE_H && x >= 320 - OUI_TITLE_H) { showHideOptionsWindow(); return; }

  // toggle grid
  if (y >= OUI_TG_TOP && y < OUI_TG_TOP + 2 * OUI_TG_H) {
    int col = x / OUI_TG_W, row = (y - OUI_TG_TOP) / OUI_TG_H;
    int idx = row * 4 + col;
    if (idx >= 0 && idx < OUI_TG_COUNT) ouiToggle(idx);   // incl. SCREEN, ROMS, HELP
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
    } else if (ouiMsxDsk()) {                 // MOUNT/UNMOUNT (4..106) | MOUNT&RUN (109..211) | REBOOT (214..316)
      if (x >= 4 && x < 106)        ouiMsxMount(false);
      else if (x >= 109 && x < 211) ouiMsxMount(true);
      else if (x >= 214 && x < 316) ouiReboot();
    } else if (ouiMsxCartCur()) {             // UNMOUNT (4..106) | LOAD & RUN (109..211) | REBOOT (214..316)
      if (x >= 4 && x < 106)        ouiMsxMount(false);
      else if (x >= 109 && x < 211) ouiMount();
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
  ouiRomClose();                 // ...nor the ROMS page
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
