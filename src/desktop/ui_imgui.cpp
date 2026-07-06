// ui_imgui.cpp — desktop native UI shell (see ui_imgui.h). Dear ImGui (docking) over SDL2 +
// SDL_Renderer. Presents the emulator framebuffer as an aspect-fit, dockable image and hosts the
// menu bar / settings / debug panels. Desktop-only (BOARD_DESKTOP); never compiled for the device.
#if defined(BOARD_DESKTOP)

#include "ui_imgui.h"
#include "debug_bridge.h"            // uniform debug facade over the cores (regs / memory / exec control)
#include "imgui.h"
#include "imgui_internal.h"          // DockBuilder* for the one-time default layout
#include "imgui_memory_editor.h"     // vendored hex viewer/editor (imgui_club)
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include <SDL.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cctype>

// The active emulator-video content rect inside the 320x240 framebuffer (cores set it each frame via
// displaySetVideoRect/Fill). "Crop borders" samples just this sub-rect. Defined in display_sdl.cpp.
void desktopGetVideoRect(int *l, int *t, int *w, int *h);

extern bool running;     // globals.cpp — cleared on Quit (kept light; we don't pull in all of emu.h here)

// Settings globals (emu.h) the native Settings window flips live; saveConfig persists them.
extern bool sound, videoColor, smoothUpscale, screenFill, Fast1MhzSpeed;
void        saveConfig();
const char *desktopSdRoot();              // sd_host.cpp — host dir backing the emulated SD (file browser)
void        desktopGetCurrentWindowSize(int *w, int *h);   // display_sdl.cpp — live SDL window size
int         desktopAudioRate();                            // audio_sdl.cpp — output sample rate
int         desktopAudioSnapshot(float *out, int n);       // audio_sdl.cpp — most-recent N output samples
const char *desktopBaseDir();                              // hal.cpp — directory of the .exe (trailing slash)

static int g_cfgWinW = 0, g_cfgWinH = 0;  // window size loaded from emu8.cfg (0 = none stored)

// Persistence files anchored next to the .exe (NOT cwd) so a saved session is found regardless of
// where the app is launched from. Resolved once, before the window is created.
static std::string g_cfgPath, g_iniPath;
static void resolvePersistPaths() {
  if (!g_cfgPath.empty()) return;
  std::string base = desktopBaseDir();
  g_cfgPath = base + "emu8.cfg";
  g_iniPath = base + "imgui.ini";
}

// --- shared SDL handles (owned by display_sdl.cpp) ---
static SDL_Window   *g_win = nullptr;
static SDL_Renderer *g_ren = nullptr;

// --- view options (wired to the View menu) ---
static bool g_filterLinear = true;    // emulator image scaling: linear (smooth) vs nearest (crisp pixels)
static bool g_cropToVideo  = false;   // show only the active video rect (drop the cores' black borders)
static bool g_showDemo     = false;   // ImGui demo window (dev aid)
static bool g_buildLayout  = false;   // build the default dock layout once (only when no imgui.ini yet)

// --- debug panel visibility (Debug menu) ---
static bool g_showCtrl   = true;      // execution control (pause/step/reset)
static bool g_showCpu    = true;      // CPU register/flag state
static bool g_showMem    = true;      // memory hex viewer/editor
static bool g_showDisasm = true;      // disassembly + breakpoints
static bool g_showHeat   = false;     // memory-access heat map
static bool g_showDisk   = false;     // disk-read heat map (Apple II)
static bool g_showSpectrum = false;   // audio spectrum analyzer
static bool g_showIo     = true;      // I/O / soft-switch state
static bool g_disasmFollow = true;    // disassembly: keep PC in view (persisted)
static bool g_showBp     = false;     // breakpoints + watchpoints manager
static bool g_showLoad     = false;   // SD file browser (load disk/cart)
static bool g_showSettings = false;   // native settings window
static bool g_showVic    = false;     // VIC-II register inspector / control (C64)
static bool g_showSid    = false;     // SID register inspector / control (C64)
static bool g_showPpu    = false;     // PPU/VRAM inspector (NES): pattern tables, nametables, palette, OAM
static bool g_showVdp    = false;     // VDP/VRAM inspector (MSX): palette, table bases, pattern sheet, sprites, raw VRAM
static bool g_heatVic    = true;      // heat map: overlay the VIC's DMA reads (orange)
static bool g_heatRegions = true;     // heat map: overlay the named memory-region boundaries + labels

// heat-map render texture (256x256, one pixel per CPU byte; row = 256-byte page)
static SDL_Texture *g_heatTex = nullptr;
// NES PPU inspector textures: two 128x128 pattern tables + the 512x480 nametable view
static SDL_Texture *g_ppuPatTex[2] = {nullptr, nullptr};
static SDL_Texture *g_ntTex = nullptr;
// MSX VDP inspector texture: the 128x128 pattern-generator tile sheet
static SDL_Texture *g_vdpPatTex = nullptr;
static bool         g_heatFade = false;
static float        g_heatZoom = 1.0f;   // mouse-wheel zoom of the memory map
static bool         g_diskFade = false;  // disk heat map: fade old accesses

// Hex viewer over the debug facade: ReadFn/WriteFn route to the side-effect-free peek/poke so the
// viewer never trips emulator soft-switches. mem_data is unused (the callbacks ignore it).
static MemoryEditor g_memEdit;
static ImU8  memRead(const ImU8 *, size_t off, void *)        { return dbgPeek((uint32_t)off); }
static void  memWrite(ImU8 *, size_t off, ImU8 d, void *)     { dbgPoke((uint32_t)off, d); }
// Second hex editor bound to the MSX VDP's separate 16K VRAM (ports $98/$99), shown in the VDP panel.
static MemoryEditor g_vramEdit;
static ImU8  vramRead(const ImU8 *, size_t off, void *)       { return dbgMsxPeekVram((uint32_t)off); }
static void  vramWrite(ImU8 *, size_t off, ImU8 d, void *)    { dbgMsxPokeVram((uint32_t)off, d); }

// --- last emulator-image placement, for window-pixel -> framebuffer mouse mapping ---
static SDL_Rect g_srcRect = {0, 0, 0, 0};   // sub-rect of the framebuffer shown (fb coords)
static SDL_Rect g_dstRect = {0, 0, 0, 0};   // where it landed on screen (window pixels)

// --- audio spectrum analyzer --------------------------------------------------------------------
// In-place iterative radix-2 FFT (n a power of two).
static void fftRadix2(float *re, float *im, int n)
{
  for (int i = 1, j = 0; i < n; i++) {              // bit-reversal permutation
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * (float)M_PI / (float)len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = a + len / 2;
        float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr;        im[a] += ti;
        float ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
      }
    }
  }
}

static float g_specBars[96] = {0};   // smoothed display bars (persist across frames for a smooth decay)

static void buildSpectrumPanel()
{
  if (!g_showSpectrum) return;
  if (ImGui::Begin("Audio spectrum", &g_showSpectrum)) {
    const int N = 1024, BARS = 96;
    static float re[N], im[N], mag[N / 2];
    desktopAudioSnapshot(re, N);
    for (int i = 0; i < N; i++) { float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (N - 1))); re[i] *= w; im[i] = 0; }
    fftRadix2(re, im, N);
    int rate = desktopAudioRate(); if (rate <= 0) rate = 44100;
    float mx = 0.02f;                                // floor so silence stays flat
    for (int i = 0; i < N / 2; i++) { mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]); if (mag[i] > mx) mx = mag[i]; }

    ImGui::TextDisabled("%d Hz   FFT %d   log-frequency 40 Hz..%g kHz", rate, N, rate * 0.0005);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.y < 60) avail.y = 60;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), IM_COL32(16, 16, 22, 255));
    float fmin = 40.0f, fmax = (float)rate * 0.5f, bw = avail.x / BARS;
    for (int b = 0; b < BARS; b++) {
      float f0 = fmin * powf(fmax / fmin, (float)b / BARS);
      float f1 = fmin * powf(fmax / fmin, (float)(b + 1) / BARS);
      int i0 = (int)(f0 * N / rate), i1 = (int)(f1 * N / rate);
      if (i0 < 1) i0 = 1; if (i1 <= i0) i1 = i0 + 1; if (i1 > N / 2) i1 = N / 2;
      float peak = 0; for (int i = i0; i < i1; i++) if (mag[i] > peak) peak = mag[i];
      float v = sqrtf(peak / mx);                    // 0..1
      g_specBars[b] = (v > g_specBars[b]) ? v : g_specBars[b] * 0.80f + v * 0.20f;   // fast attack, smooth decay
      float h = g_specBars[b] * avail.y;
      ImU32 col = IM_COL32((int)(70 + 185 * g_specBars[b]), (int)(225 - 150 * g_specBars[b]), 70, 255);
      ImVec2 a(p0.x + b * bw, p0.y + avail.y - h);
      dl->AddRectFilled(a, ImVec2(a.x + bw - 1.0f, p0.y + avail.y), col);
    }
    ImGui::Dummy(avail);
  }
  ImGui::End();
}

// --- session persistence (emu8.cfg = window size + view prefs + which panels are open) ---------
void desktopUiLoadConfig()
{
  resolvePersistPaths();
  FILE *f = fopen(g_cfgPath.c_str(), "r");
  if (!f) return;
  char line[160];
  while (fgets(line, sizeof(line), f)) {
    char key[64]; int v;
    if (sscanf(line, "%63[^=]=%d", key, &v) != 2) continue;
    #define CFG(name, var) else if (strcmp(key, name) == 0) var = (v != 0)
    if (strcmp(key, "winW") == 0) g_cfgWinW = v;
    else if (strcmp(key, "winH") == 0) g_cfgWinH = v;
    else if (strcmp(key, "memCols") == 0) g_memEdit.Cols = (v >= 4 && v <= 64) ? v : 16;
    CFG("filterLinear", g_filterLinear); CFG("cropToVideo", g_cropToVideo);
    CFG("showCtrl", g_showCtrl);   CFG("showCpu", g_showCpu);   CFG("showIo", g_showIo);
    CFG("showDisasm", g_showDisasm); CFG("showBp", g_showBp);   CFG("showMem", g_showMem);
    CFG("showHeat", g_showHeat);   CFG("showDisk", g_showDisk);
    CFG("showSettings", g_showSettings); CFG("showSpectrum", g_showSpectrum);
    CFG("showVic", g_showVic);     CFG("showSid", g_showSid);     CFG("showPpu", g_showPpu);
    CFG("showVdp", g_showVdp);
    CFG("heatVic", g_heatVic);
    CFG("heatRegions", g_heatRegions);
    CFG("heatFade", g_heatFade);   CFG("disasmFollow", g_disasmFollow); CFG("diskFade", g_diskFade);
    CFG("memAscii", g_memEdit.OptShowAscii);   CFG("memOpts", g_memEdit.OptShowOptions);
    CFG("memGrey", g_memEdit.OptGreyOutZeroes); CFG("memPreview", g_memEdit.OptShowDataPreview);
    else if (strcmp(key, "heatRecord") == 0) { if (v) dbgHeatEnable(true); }      // restore Record state
    else if (strcmp(key, "diskRecord") == 0) { if (v) dbgDiskHeatEnable(true); }
    else if (strcmp(key, "heatZoom") == 0) g_heatZoom = (v >= 100 && v <= 4800) ? (float)v / 100.0f : 1.0f;
    else if (strcmp(key, "clockMilliMhz") == 0) dbgSetClockMhz((float)v / 1000.0f); // restore clock speed
    #undef CFG
  }
  fclose(f);
}
void desktopUiGetWindowSize(int *w, int *h) { if (w) *w = g_cfgWinW; if (h) *h = g_cfgWinH; }

// Write JUST emu8.cfg (window size + view prefs + open panels + per-panel options). Cheap; called
// both on a debounced auto-save (so options persist WITHOUT a clean quit, like ImGui's own ini) and
// from desktopUiSaveState() on exit.
static void desktopWriteCfg()
{
  int w = 0, h = 0; desktopGetCurrentWindowSize(&w, &h);
  resolvePersistPaths();
  FILE *f = fopen(g_cfgPath.c_str(), "w");
  if (!f) return;
  if (w > 0 && h > 0) fprintf(f, "winW=%d\nwinH=%d\n", w, h);
  fprintf(f, "filterLinear=%d\ncropToVideo=%d\n", g_filterLinear, g_cropToVideo);
  fprintf(f, "showCtrl=%d\nshowCpu=%d\nshowIo=%d\nshowDisasm=%d\nshowBp=%d\nshowMem=%d\nshowHeat=%d\nshowDisk=%d\n",
          g_showCtrl, g_showCpu, g_showIo, g_showDisasm, g_showBp, g_showMem, g_showHeat, g_showDisk);
  fprintf(f, "showSettings=%d\nshowSpectrum=%d\nheatFade=%d\ndisasmFollow=%d\n",
          g_showSettings, g_showSpectrum, g_heatFade, g_disasmFollow);
  fprintf(f, "showVic=%d\nshowSid=%d\nshowPpu=%d\nshowVdp=%d\nheatVic=%d\nheatRegions=%d\n",
          g_showVic, g_showSid, g_showPpu, g_showVdp, g_heatVic, g_heatRegions);
  fprintf(f, "memCols=%d\nmemAscii=%d\nmemOpts=%d\nmemGrey=%d\nmemPreview=%d\n",
          g_memEdit.Cols, g_memEdit.OptShowAscii, g_memEdit.OptShowOptions,
          g_memEdit.OptGreyOutZeroes, g_memEdit.OptShowDataPreview);
  fprintf(f, "heatRecord=%d\ndiskRecord=%d\ndiskFade=%d\n", dbgHeatEnabled(), dbgDiskHeatEnabled(), g_diskFade);
  fprintf(f, "heatZoom=%d\n", (int)(g_heatZoom * 100.0f + 0.5f));
  fprintf(f, "clockMilliMhz=%d\n", (int)(dbgGetClockMhz() * 1000.0f + 0.5f));
  fclose(f);
}

// Re-write emu8.cfg whenever any persisted value changed, debounced ~0.7 s (so a resize-drag or a
// burst of toggles writes once after it settles). Runs every frame; cheap (a snprintf + strcmp).
static void desktopUiAutoSaveCfg()
{
  int w = 0, h = 0; desktopGetCurrentWindowSize(&w, &h);
  char sig[256];
  snprintf(sig, sizeof(sig), "%d,%d,%d,%d|%d%d%d%d%d%d%d%d%d%d|%d%d|%d,%d%d%d%d|%d%d",
           w, h, g_filterLinear, g_cropToVideo,
           g_showCtrl, g_showCpu, g_showIo, g_showDisasm, g_showBp, g_showMem, g_showHeat, g_showDisk,
           g_showSettings, g_showSpectrum, g_heatFade, g_disasmFollow,
           g_memEdit.Cols, (int)g_memEdit.OptShowAscii, (int)g_memEdit.OptShowOptions,
           (int)g_memEdit.OptGreyOutZeroes, (int)g_memEdit.OptShowDataPreview,
           (int)dbgHeatEnabled(), (int)dbgDiskHeatEnabled());
  char sig2[64]; snprintf(sig2, sizeof(sig2), "|c%d|%d|%d%d%d%d%d%d|z%d", (int)(dbgGetClockMhz() * 1000.0f),
                          (int)g_diskFade, (int)g_showVic, (int)g_showSid, (int)g_showPpu, (int)g_showVdp,
                          (int)g_heatVic, (int)g_heatRegions, (int)(g_heatZoom * 100.0f));
  strncat(sig, sig2, sizeof(sig) - strlen(sig) - 1);
  static char last[256] = {0};
  static int  countdown = -1;
  if (strcmp(sig, last) != 0) { strncpy(last, sig, sizeof(last) - 1); countdown = 45; }   // change -> arm
  if (countdown > 0 && --countdown == 0) desktopWriteCfg();                                // settled -> write
}

void desktopUiSaveState()
{
  desktopWriteCfg();
  if (ImGui::GetCurrentContext()) ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);  // dock layout
  saveConfig();   // persist emulator settings + last-loaded disk to eeprom.bin
}

void desktopUiInit(SDL_Window *win, SDL_Renderer *ren)
{
  g_win = win;
  g_ren = ren;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;       // dockable debug panels
  // NOTE: do NOT enable NavEnableKeyboard — it makes ImGui hold WantCaptureKeyboard whenever a panel
  // is focused, which would steal the keyboard from the emulator. The emulator needs every key; we
  // only divert the keyboard to ImGui while an actual text field is being edited (WantTextInput).
  io.FontGlobalScale = 1.25f;                             // comfortable, crisp at desktop sizes

  resolvePersistPaths();
  io.IniFilename = g_iniPath.c_str();                     // dock layout next to the .exe (g_iniPath outlives io)

  // Build the default dock layout only on a fresh install (no imgui.ini to restore). After that the
  // user's saved layout wins.
  if (FILE *f = fopen(g_iniPath.c_str(), "rb")) { fclose(f); g_buildLayout = false; }
  else                                          { g_buildLayout = true; }

  ImGui::StyleColorsDark();
  ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
  ImGui_ImplSDLRenderer2_Init(ren);

  g_memEdit.ReadFn  = memRead;         // hex viewer reads/writes through the side-effect-free facade
  g_memEdit.WriteFn = memWrite;

  if (getenv("EMU_DBG_PAUSE")) dbgSetPaused(true);   // boot paused (handy for offline debug captures)
  if (getenv("EMU_DBG_HEAT")) { g_showHeat = true; g_showMem = false; dbgHeatEnable(true); }  // heat capture
  if (getenv("EMU_DBG_DIS"))  { g_showMem = false; g_showHeat = false; }  // disasm capture (sole bottom tab)
  if (getenv("EMU_DBG_LOAD"))     g_showLoad = true;        // open the file browser (offline capture)
  if (getenv("EMU_DBG_SETTINGS")) g_showSettings = true;    // open settings (offline capture)
  if (getenv("EMU_DBG_VIC"))      g_showVic = true;         // open the VIC-II panel (C64, offline capture)
  if (getenv("EMU_DBG_SID"))      g_showSid = true;         // open the SID panel (C64, offline capture)
  if (getenv("EMU_DBG_PPU"))      g_showPpu = true;         // open the PPU/VRAM panel (NES, offline capture)
  if (getenv("EMU_DBG_VDP"))      g_showVdp = true;         // open the VDP/VRAM panel (MSX, offline capture)
  if (getenv("EMU_DBG_IO"))       g_showCpu = false;        // I/O panel solo in its tab (offline capture)
  if (getenv("EMU_DBG_DISK")) { g_showDisk = true; g_showMem = g_showHeat = g_showDisasm = false; dbgDiskHeatEnable(true); }
  if (getenv("EMU_DBG_SPECTRUM")) { g_showSpectrum = true; g_showMem = g_showHeat = g_showDisasm = g_showDisk = false; }
  if (const char *c = getenv("EMU_DBG_CLOCK")) { dbgSetThrottle(true); dbgSetClockMhz((float)atof(c)); }  // clock self-test
  if (const char *w = getenv("EMU_DBG_WATCH")) {           // set a R+W watchpoint at boot (self-test)
    g_showBp = true; g_showDisasm = false;
    dbgWatchToggle((uint32_t)strtol(w, nullptr, 16), WATCH_R | WATCH_W);
  }
}

void desktopUiShutdown()
{
  if (!ImGui::GetCurrentContext()) return;
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
}

void desktopUiProcessEvent(const SDL_Event *e)
{
  if (ImGui::GetCurrentContext()) ImGui_ImplSDL2_ProcessEvent(e);
}

bool desktopUiWantCaptureMouse()    { return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse; }
// Only divert the keyboard from the emulator while an ImGui TEXT FIELD is being edited (goto/filter).
// Using WantCaptureKeyboard here instead would let any focused panel swallow the emulator's keys.
bool desktopUiWantCaptureKeyboard() { return ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput; }

bool desktopUiMapToEmu(int winX, int winY, int *ox, int *oy)
{
  if (g_dstRect.w <= 0 || g_dstRect.h <= 0) return false;
  if (winX < g_dstRect.x || winX >= g_dstRect.x + g_dstRect.w ||
      winY < g_dstRect.y || winY >= g_dstRect.y + g_dstRect.h) return false;
  float fx = (winX - g_dstRect.x) / (float)g_dstRect.w;
  float fy = (winY - g_dstRect.y) / (float)g_dstRect.h;
  int x = g_srcRect.x + (int)(fx * g_srcRect.w);
  int y = g_srcRect.y + (int)(fy * g_srcRect.h);
  if (ox) *ox = x;
  if (oy) *oy = y;
  return true;
}

// --- menu bar -------------------------------------------------------------------------------------
static void buildMenuBar()
{
  if (!ImGui::BeginMainMenuBar()) return;

  if (ImGui::BeginMenu("System")) {
    if (ImGui::BeginMenu("Platform")) {
      int cur = dbgPlatform();
      for (int p = 0; p < dbgPlatformCount(); p++)
        if (ImGui::MenuItem(dbgPlatformName(p), nullptr, p == cur)) dbgSwitchPlatform(p);  // reboots into p
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Load disk / cartridge...")) g_showLoad = true;
    ImGui::MenuItem("Settings", nullptr, &g_showSettings);
    ImGui::Separator();
    bool p = dbgIsPaused();
    if (ImGui::MenuItem(p ? "Resume" : "Pause", "F5")) dbgSetPaused(!p);
    if (ImGui::MenuItem("Step", "F10", false, p && dbgStepSupported())) dbgStep();
    ImGui::Separator();
    if (ImGui::MenuItem("Reset (reboot)")) dbgReset();
    if (ImGui::MenuItem("Quit")) { desktopUiSaveState(); running = false; SDL_Quit(); std::exit(0); }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Linear filter (smooth)", nullptr, &g_filterLinear);
    ImGui::MenuItem("Crop borders (active video only)", nullptr, &g_cropToVideo);
    ImGui::Separator();
    ImGui::MenuItem("ImGui demo window", nullptr, &g_showDemo);
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Debug")) {
    ImGui::MenuItem("Execution control", nullptr, &g_showCtrl);
    ImGui::MenuItem("CPU state", nullptr, &g_showCpu);
    ImGui::MenuItem("Disassembly", nullptr, &g_showDisasm);
    ImGui::MenuItem("Breakpoints / watchpoints", nullptr, &g_showBp);
    ImGui::MenuItem("Memory", nullptr, &g_showMem);
    ImGui::MenuItem("I/O (soft switches)", nullptr, &g_showIo);
    ImGui::MenuItem("Heat map (memory)", nullptr, &g_showHeat);
    ImGui::MenuItem("Heat map (disk reads)", nullptr, &g_showDisk);
    ImGui::MenuItem("Audio spectrum", nullptr, &g_showSpectrum);
    ImGui::Separator();
    ImGui::MenuItem("VIC-II (C64)", nullptr, &g_showVic, dbgVicSupported());
    ImGui::MenuItem("SID (C64)",    nullptr, &g_showSid, dbgSidSupported());
    ImGui::MenuItem("PPU / VRAM (NES)", nullptr, &g_showPpu, dbgNesPpuSupported());
    ImGui::MenuItem("VDP / VRAM (MSX)", nullptr, &g_showVdp, dbgMsxVdpSupported());
    ImGui::EndMenu();
  }

  // right-aligned state + FPS readout
  char info[64];
  snprintf(info, sizeof(info), "%s   %.0f FPS", dbgIsPaused() ? "PAUSED" : "RUNNING", ImGui::GetIO().Framerate);
  float w = ImGui::CalcTextSize(info).x;
  ImGui::SameLine(ImGui::GetWindowWidth() - w - 16.0f);
  ImGui::TextColored(dbgIsPaused() ? ImVec4(1.0f, 0.7f, 0.2f, 1.0f) : ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s", info);

  ImGui::EndMainMenuBar();
}

// --- debug panels ---------------------------------------------------------------------------------
static void buildControlPanel()
{
  if (!g_showCtrl) return;
  if (ImGui::Begin("Control", &g_showCtrl)) {
    bool p = dbgIsPaused();
    if (p) { if (ImGui::Button("Resume", ImVec2(80, 0))) dbgSetPaused(false); }
    else   { if (ImGui::Button("Pause",  ImVec2(80, 0))) dbgSetPaused(true);  }
    ImGui::SameLine();
    ImGui::BeginDisabled(!(p && dbgStepSupported()));
    if (ImGui::Button("Step", ImVec2(80, 0))) dbgStep();            // step into
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reboot", ImVec2(80, 0))) dbgReset();          // re-exec (loses session)

    // second row: subroutine-aware stepping + a session-preserving soft reset (6502)
    ImGui::BeginDisabled(!(p && dbgRunControlSupported()));
    if (ImGui::Button("Step Over", ImVec2(80, 0))) dbgStepOver();
    ImGui::SameLine();
    if (ImGui::Button("Step Out", ImVec2(80, 0))) dbgStepOut();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!dbgSoftResetSupported());
    if (ImGui::Button("Soft Reset", ImVec2(80, 0))) dbgSoftReset();
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Text("State: %s", p ? "PAUSED" : "RUNNING");
    ImGui::Text("CPU:   %s", dbgCpuName());
    if (dbgMemReadable()) ImGui::Text("PC:    $%04X", dbgGetPC());
    if (!dbgStepSupported())
      ImGui::TextDisabled("(single-step not wired for this platform)");

    // speed controller — "Full speed (uncapped host)" on EVERY platform; Apple II also gets a target-MHz
    // slider when paced. Atari/PC-XT/tiny386 are always uncapped on desktop (the toggle is fixed-on).
    if (dbgFullSpeedSupported()) {
      ImGui::SeparatorText("Speed");
      bool fixed = dbgFullSpeedFixed();
      bool full  = dbgGetFullSpeed();
      ImGui::BeginDisabled(fixed);
      if (ImGui::Checkbox("Full speed (uncapped host)", &full)) dbgSetFullSpeed(full);
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::TextDisabled(full ? (fixed ? "(always uncapped)" : "(uncapped)") : "(paced to real HW)");
      if (dbgClockSupported()) {                 // Apple II: variable target clock when paced
        ImGui::BeginDisabled(full);
        float mhz = dbgGetClockMhz();
        ImGui::SetNextItemWidth(150);
        if (ImGui::SliderFloat("##mhz", &mhz, 0.25f, 8.0f, "%.2f MHz")) dbgSetClockMhz(mhz);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Stock speed")) { dbgSetFullSpeed(false); dbgSetClockMhz(dbgClockDefaultMhz()); }
        ImGui::Text("measured: %.2f MHz", dbgGetMeasuredMhz());
      }
    }
  }
  ImGui::End();
}

static void buildCpuPanel()
{
  if (!g_showCpu) return;
  if (ImGui::Begin("CPU", &g_showCpu)) {
    ImGui::TextDisabled("%s", dbgCpuName());
    DbgReg regs[16];
    int n = dbgGetRegs(regs, 16);
    int spVal = -1;
    if (n == 0) {
      ImGui::TextDisabled("(registers not wired for this platform yet)");
    } else if (ImGui::BeginTable("regs", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
      for (int i = 0; i < n; i++) {
        if (strcmp(regs[i].name, "SP") == 0) spVal = (int)regs[i].value;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(regs[i].name);
        ImGui::TableNextColumn();
        int digits = regs[i].bits / 4;
        ImGui::Text("$%0*X", digits, regs[i].value);
      }
      ImGui::EndTable();
    }

    // status flags: uppercase = set, dim lowercase = clear
    const char *const *labels; uint32_t fv; int fc;
    if (dbgGetFlags(&labels, &fv, &fc)) {
      ImGui::Spacing();
      ImGui::TextUnformatted("Flags:");
      for (int i = 0; i < fc; i++) {
        bool set = (fv >> (fc - 1 - i)) & 1;
        ImGui::SameLine();
        ImGui::TextColored(set ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(0.45f, 0.45f, 0.45f, 1.0f),
                           "%s", labels[i]);
      }
    }

    // Stack readout: 6502 = the bytes pushed in page 1 ($0100+SP .. $01FF), newest first; Z80 = the
    // 16-bit downward stack from SP upward (SP points at the most-recently pushed byte), newest first.
    if (spVal >= 0 && dbgMemReadable()) {
      uint16_t sp = (uint16_t)spVal;
      ImGui::SeparatorText("Stack");
      ImGui::BeginChild("stk", ImVec2(0, 110));
      if (dbgStack16()) {
        for (int k = 0; k < 24; k++) { uint16_t a = (uint16_t)(sp + k); ImGui::Text("$%04X: $%02X", a, dbgPeek(a)); }
      } else if (sp >= 0x01FF) {
        ImGui::TextDisabled("(empty)");
      } else {
        for (uint16_t a = sp + 1; a <= 0x01FF; a++) ImGui::Text("$%04X: $%02X", a, dbgPeek(a));
      }
      ImGui::EndChild();
    }
  }
  ImGui::End();
}

static void buildDisasmPanel()
{
  if (!g_showDisasm) return;
  if (ImGui::Begin("Disassembly", &g_showDisasm)) {
    static bool focusOnce = getenv("EMU_DBG_PAUSE") != nullptr;   // offline captures: bring disasm to front
    if (focusOnce) { ImGui::SetWindowFocus(); focusOnce = false; }
    if (!dbgDisasmSupported()) {
      ImGui::TextDisabled("(disassembly not wired for this platform yet)");
      ImGui::End();
      return;
    }
    static uint16_t viewAddr = 0;
    static char gotoBuf[8] = "";
    uint16_t pc = (uint16_t)dbgGetPC();

    ImGui::Checkbox("Follow PC", &g_disasmFollow);
    ImGui::SameLine(); ImGui::TextDisabled("goto");
    ImGui::SameLine(); ImGui::SetNextItemWidth(56);
    if (ImGui::InputText("##goto", gotoBuf, sizeof(gotoBuf),
                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
      viewAddr = (uint16_t)strtol(gotoBuf, nullptr, 16); g_disasmFollow = false;
    }
    if (dbgBpSupported()) { ImGui::SameLine(); if (ImGui::Button("Clear BPs")) dbgBpClearAll(); }
    if (g_disasmFollow) viewAddr = pc;

    ImGui::Separator();
    if (ImGui::BeginChild("disasm_lines")) {
      uint16_t a = viewAddr;
      for (int i = 0; i < 96; i++) {
        char dis[40]; int len = dbgDisasm(a, dis, sizeof(dis));
        char bytes[16] = "";
        for (int k = 0; k < len; k++) { char t[4]; snprintf(t, sizeof(t), "%02X ", dbgPeek((uint16_t)(a + k))); strcat(bytes, t); }
        bool isPC = (a == pc);
        bool bp   = dbgBpSupported() && dbgBpAt(a);
        char line[80];
        snprintf(line, sizeof(line), "%s %s%04X  %-9s %s", bp ? "*" : " ", isPC ? ">" : " ", a, bytes, dis);
        ImGui::PushID(a);
        ImGui::PushStyleColor(ImGuiCol_Text, isPC ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) : ImVec4(0.82f, 0.82f, 0.82f, 1.0f));
        bool clicked = ImGui::Selectable(line, bp);
        ImGui::PopStyleColor();
        if (clicked && dbgBpSupported()) dbgBpToggle(a);                        // left-click toggles a breakpoint
        if (ImGui::BeginPopupContextItem()) {                                   // right-click menu
          if (dbgRunControlSupported() && ImGui::MenuItem("Run to here")) dbgRunTo(a);
          if (dbgBpSupported() && ImGui::MenuItem(bp ? "Remove breakpoint" : "Add breakpoint")) dbgBpToggle(a);
          ImGui::EndPopup();
        }
        ImGui::PopID();
        a = (uint16_t)(a + len);
      }
    }
    ImGui::EndChild();
  }
  ImGui::End();
}

static void buildMemoryPanel()
{
  if (!g_showMem) return;
  if (ImGui::Begin("Memory", &g_showMem)) {
    if (dbgMemReadable()) {
      g_memEdit.WriteFn = dbgPokeSupported() ? memWrite : nullptr;   // read-only where poke is unsafe
      g_memEdit.DrawContents(nullptr, (size_t)dbgMemSize(), 0);
    } else {
      ImGui::TextDisabled("(memory view not wired for this platform yet)");
    }
  }
  ImGui::End();
}

// --- I/O / soft-switch state (Apple II banking + video switches) --------------------------------
static void buildIoPanel()
{
  if (!g_showIo) return;
  if (ImGui::Begin("I/O", &g_showIo)) {
    DbgFlag fl[24];
    int n = dbgGetIoState(fl, 24);
    if (n == 0) {
      ImGui::TextDisabled("(no soft-switch state for this platform yet)");
    } else if (ImGui::BeginTable("io", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      for (int i = 0; i < n; i++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", fl[i].label);
        ImGui::TableNextColumn();
        ImGui::TextColored(fl[i].active ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f) : ImVec4(0.78f, 0.78f, 0.78f, 1.0f),
                           "%s", fl[i].value);
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
}

// --- breakpoints + watchpoints manager ----------------------------------------------------------
static void buildBpPanel()
{
  if (!g_showBp) return;
  if (ImGui::Begin("Breakpoints", &g_showBp)) {
    ImGui::SeparatorText("Breakpoints");
    if (!dbgBpSupported()) {
      ImGui::TextDisabled("(not supported on this platform)");
    } else {
      ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);
      if (ImGui::SmallButton("Clear all##bp")) dbgBpClearAll();
      uint16_t bps[256]; int nb = dbgBpList(bps, 256);
      if (nb == 0) ImGui::TextDisabled("(none — left-click a disassembly line to add)");
      for (int i = 0; i < nb; i++) {
        ImGui::PushID(2000 + i);
        if (ImGui::SmallButton("X")) dbgBpToggle(bps[i]);
        ImGui::SameLine();
        char dis[40]; dbgDisasm(bps[i], dis, sizeof(dis));
        if (ImGui::Selectable("##bprow", false)) dbgRunTo(bps[i]);   // click row -> run to it
        ImGui::SameLine(); ImGui::Text("$%04X  %s", bps[i], dis);
        ImGui::PopID();
      }
    }

    ImGui::SeparatorText("Watchpoints");
    if (!dbgWatchSupported()) {
      ImGui::TextDisabled("(not supported on this platform)");
    } else {
      static char wbuf[8] = ""; static bool wR = true, wW = true;
      ImGui::SetNextItemWidth(56);
      ImGui::InputTextWithHint("##waddr", "addr", wbuf, sizeof(wbuf), ImGuiInputTextFlags_CharsHexadecimal);
      ImGui::SameLine(); ImGui::Checkbox("R", &wR);
      ImGui::SameLine(); ImGui::Checkbox("W", &wW);
      ImGui::SameLine();
      if (ImGui::SmallButton("Add##wp") && wbuf[0]) {
        uint16_t a = (uint16_t)strtol(wbuf, nullptr, 16);
        uint8_t want = (uint8_t)((wR ? WATCH_R : 0) | (wW ? WATCH_W : 0));
        dbgWatchToggle(a, (uint8_t)(dbgWatchAt(a) ^ want));   // set this address to exactly R/W
      }
      ImGui::SameLine(); if (ImGui::SmallButton("Clear all##wp")) dbgWatchClearAll();
      if (g_dbgWatchHit >= 0)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "last hit: $%04X", g_dbgWatchHit);
      int shown = 0;
      for (int a = 0; a < 0x10000 && shown < 64; a++) {
        uint8_t m = dbgWatchAt((uint32_t)a);
        if (!m) continue;
        shown++;
        ImGui::PushID(3000 + a);
        if (ImGui::SmallButton("X")) dbgWatchToggle((uint32_t)a, m);   // clear all bits at this addr
        ImGui::SameLine();
        ImGui::Text("$%04X  %s%s", a, (m & WATCH_R) ? "R" : "-", (m & WATCH_W) ? "W" : "-");
        ImGui::PopID();
      }
    }
  }
  ImGui::End();
}

// Name of the memory region containing `addr` (zero page / stack / screen / ROM / ...), or null.
static const char *heatRegionName(int addr) {
  const DbgRegion *rg; int n = dbgMemRegions(&rg);
  for (int i = 0; i < n; i++) if (addr >= rg[i].start && addr <= rg[i].end) return rg[i].name;
  return nullptr;
}

// Memory-access heat map: 256x256 texture, 1px = 1 byte of CPU space, row = one 256-byte page.
// Green = reads, red = writes, blue = executed (opcode fetch); log-scaled so light use still shows.
static void buildHeatPanel()
{
  if (!g_showHeat) return;
  if (ImGui::Begin("Heat map", &g_showHeat)) {
    if (!dbgMemReadable()) { ImGui::TextDisabled("(no memory map for this platform yet)"); ImGui::End(); return; }
    bool on = dbgHeatEnabled();
    if (ImGui::Checkbox("Record", &on)) dbgHeatEnable(on);
    ImGui::SameLine(); if (ImGui::Button("Clear")) dbgHeatClear();
    ImGui::SameLine(); ImGui::Checkbox("Fade", &g_heatFade);
    if (dbgVicSupported()) { ImGui::SameLine(); ImGui::Checkbox("VIC", &g_heatVic); }
    { const DbgRegion *rg; if (dbgMemRegions(&rg) > 0) { ImGui::SameLine(); ImGui::Checkbox("Regions", &g_heatRegions); } }
    ImGui::SameLine(); if (ImGui::Button("1x")) g_heatZoom = 1.0f;
    ImGui::SameLine(); ImGui::TextDisabled("%.0f%%", g_heatZoom * 100.0f);
    ImGui::TextDisabled("value gray  +green read +red write +blue exec%s;  wheel zoom, RMB drag pan, LMB -> Memory",
                        dbgVicSupported() ? "  +orange VIC" : "");

    if (!g_heatTex) {
      g_heatTex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 256, 256);
      SDL_SetTextureScaleMode(g_heatTex, SDL_ScaleModeNearest);
    }
    // build the 256x256 image: base grayscale = byte VALUE (00->black, FF->~50% gray), then add the
    // R/W/X access heat as green/red/blue when Record is on.
    bool heat = on && dbgHeatBuf(DBG_HEAT_R);
    const uint32_t *R = nullptr, *W = nullptr, *X = nullptr, *V = nullptr; float lm = 1.0f;
    if (heat) {
      if (g_heatFade) dbgHeatDecay(0.90f);
      R = dbgHeatBuf(DBG_HEAT_R); W = dbgHeatBuf(DBG_HEAT_W); X = dbgHeatBuf(DBG_HEAT_X); V = dbgHeatBuf(DBG_HEAT_V);
      uint32_t mx = 1;
      for (int i = 0; i < 65536; i++) { if (R[i] > mx) mx = R[i]; if (W[i] > mx) mx = W[i]; if (X[i] > mx) mx = X[i]; }
      lm = logf(1.0f + (float)mx);
    }
    static uint32_t px[256 * 256];
    for (int a = 0; a < 65536; a++) {
      int val  = dbgPeek((uint32_t)a);                      // live data byte (screen code / bitmap / colour)
      int base = val >> 1;                                  // 0x00->0, 0xFF->127 (~50% gray)
      int r = base, g = base, b = base;
      if (heat) {
        g += (int)(255.0f * logf(1.0f + (float)R[a]) / lm); // read  -> green
        r += (int)(255.0f * logf(1.0f + (float)W[a]) / lm); // write -> red
        b += (int)(255.0f * logf(1.0f + (float)X[a]) / lm); // exec  -> blue
        if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
        // VIC-II DMA region -> ORANGE, brightness following the live byte VALUE so the actual screen
        // content (and its changes) shows through. Overlaid with max() so CPU writes (red) still read.
        if (g_heatVic && V && V[a]) {
          int oR = 40 + val;        if (oR > 255) oR = 255;
          int oG = 14 + val / 2;    if (oG > 255) oG = 255;
          if (oR > r) r = oR;
          if (oG > g) g = oG;
        }
      }
      px[a] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;   // ABGR8888
    }
    SDL_UpdateTexture(g_heatTex, nullptr, px, 256 * sizeof(uint32_t));

    // scrollable, mouse-wheel-zoomable view (zoom centered on the cursor; nearest = crisp pixels)
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float bs = (avail.x < avail.y ? avail.x : avail.y); if (bs < 64) bs = 64;
    ImGui::BeginChild("heatview", avail, false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 p = ImGui::GetCursorScreenPos(), mo = io.MousePos;
    float dim = bs * g_heatZoom;
    float relx = (mo.x - p.x) / dim, rely = (mo.y - p.y) / dim;
    bool hov = ImGui::IsWindowHovered();

    // Right-button drag PANS the view (keep panning even if the cursor briefly leaves the child).
    static bool panning = false;
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) panning = true;
    if (panning) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      } else {
        panning = false;
      }
    }

    if (hov && io.MouseWheel != 0.0f && relx >= 0 && relx <= 1 && rely >= 0 && rely <= 1) {
      float nz = g_heatZoom * powf(1.2f, io.MouseWheel);
      if (nz < 1.0f) nz = 1.0f; else if (nz > 48.0f) nz = 48.0f;
      float nd = bs * nz;
      float ox = p.x + ImGui::GetScrollX(), oy = p.y + ImGui::GetScrollY();
      ImGui::SetScrollX(ox - mo.x + relx * nd);            // keep the byte under the cursor fixed
      ImGui::SetScrollY(oy - mo.y + rely * nd);
      g_heatZoom = nz; dim = nd;
    }
    ImGui::Image((ImTextureID)(intptr_t)g_heatTex, ImVec2(dim, dim));

    // Region overlay: a faint boundary line at each region's start page, with its name (labels are
    // skipped when too close to the previous one so packed low pages don't overprint). Each region
    // spans rows [start>>8 .. end>>8] in the 256-byte-per-row map, so a boundary is a horizontal line.
    if (g_heatRegions) {
      const DbgRegion *rg; int nrg = dbgMemRegions(&rg);
      ImDrawList *dl = ImGui::GetWindowDrawList();
      float lastLabelY = -1e9f;
      for (int i = 0; i < nrg; i++) {
        float y = p.y + (rg[i].start >> 8) * dim / 256.0f;
        dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + dim, y), IM_COL32(255, 255, 255, 55));
        if (y - lastLabelY > 13.0f) {
          dl->AddText(ImVec2(p.x + 4, y + 1), IM_COL32(255, 240, 170, 230), rg[i].name);
          lastLabelY = y;
        }
      }
    }

    if (hov && relx >= 0 && relx < 1 && rely >= 0 && rely < 1) {
      int a = ((int)(rely * 256)) * 256 + (int)(relx * 256);
      const char *rn = heatRegionName(a);
      if (heat) ImGui::SetTooltip("$%04X = $%02X   [%s]\nR:%u W:%u X:%u\n(left-click -> Memory; right-drag -> pan)",
                                  a, dbgPeek(a), rn ? rn : "?", R[a], W[a], X[a]);
      else      ImGui::SetTooltip("$%04X = $%02X   [%s]\n(left-click -> Memory; right-drag -> pan)",
                                  a, dbgPeek(a), rn ? rn : "?");
      // Left-click jumps the Memory hex editor to this byte (opens + focuses it, highlights the cell).
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_memEdit.GotoAddrAndHighlight((size_t)a, (size_t)a + 1);
        g_showMem = true;
        ImGui::SetWindowFocus("Memory");
      }
    }
    ImGui::EndChild();
  }
  ImGui::End();
}

// --- emulator display window ----------------------------------------------------------------------
static void buildDisplayWindow(SDL_Texture *emuTex, int fbW, int fbH)
{
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (ImGui::Begin("Emulator")) {
    // source sub-rect of the framebuffer: full 320x240, or just the active video content if cropping
    int sx = 0, sy = 0, sw = fbW, sh = fbH;
    if (g_cropToVideo) {
      int l, t, w, h; desktopGetVideoRect(&l, &t, &w, &h);
      if (w > 0 && h > 0) { sx = l; sy = t; sw = w; sh = h; }
    }
    g_srcRect = SDL_Rect{ sx, sy, sw, sh };

    // aspect-fit the source rect into the window's content area, centered (letterbox/pillarbox)
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float ar = (float)sw / (float)sh;
    float dw = avail.x, dh = dw / ar;
    if (dh > avail.y) { dh = avail.y; dw = dh * ar; }
    ImVec2 cur = ImGui::GetCursorScreenPos();
    ImVec2 pos = ImVec2(cur.x + (avail.x - dw) * 0.5f, cur.y + (avail.y - dh) * 0.5f);
    ImGui::SetCursorScreenPos(pos);

    SDL_SetTextureScaleMode(emuTex, g_filterLinear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    ImVec2 uv0(sx / (float)fbW, sy / (float)fbH);
    ImVec2 uv1((sx + sw) / (float)fbW, (sy + sh) / (float)fbH);
    ImGui::Image((ImTextureID)(intptr_t)emuTex, ImVec2(dw, dh), uv0, uv1);

    g_dstRect = SDL_Rect{ (int)pos.x, (int)pos.y, (int)dw, (int)dh };
  } else {
    g_dstRect = SDL_Rect{ 0, 0, 0, 0 };   // window collapsed -> no touch target
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

// Headless self-check: dump the FULL composited window (ImGui chrome + emulator image), not just the
// 320x240 framebuffer, to a BMP at frame EMU_UI_DUMP_AT. EMU_UI_QUIT=1 exits right after, so the UI
// can be validated offline. Call after the ImGui draw data is rendered, before Present.
static void uiMaybeCapture()
{
  static const char *path = getenv("EMU_UI_DUMP");
  if (!path) return;
  static long frame = -1; frame++;
  static long at = []{ const char *s = getenv("EMU_UI_DUMP_AT"); return s ? atol(s) : 120L; }();
  if (frame < at) return;
  int w = 0, h = 0; SDL_GetRendererOutputSize(g_ren, &w, &h);
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  if (s) {
    SDL_RenderReadPixels(g_ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    fprintf(stderr, "[ui] saved %dx%d window capture -> %s\n", w, h, path);
  }
  // EMU_DBG_DUMP="addr,len" (hex) — dump a CPU-memory range via the side-effect-free peek, for offline
  // diagnosis (e.g. inspecting where a hung cart's loader is looping). Also prints PC.
  if (const char *d = getenv("EMU_DBG_DUMP")) {
    unsigned addr = 0, len = 64; sscanf(d, "%x,%x", &addr, &len);
    fprintf(stderr, "[dump] PC=$%04X  $%04X..$%04X:", (unsigned)dbgGetPC(), addr, addr + len - 1);
    for (unsigned i = 0; i < len; i++) {
      if (i % 16 == 0) fprintf(stderr, "\n%04X: ", addr + i);
      fprintf(stderr, "%02X ", dbgPeek(addr + i));
    }
    fprintf(stderr, "\n");
  }
  if (getenv("EMU_UI_QUIT")) {
    if (getenv("EMU_UI_SAVE")) desktopUiSaveState();   // exercise the clean-quit persistence path
    SDL_Quit(); std::exit(0);
  }
}

// --- native SD file browser (System > Load disk / cartridge) ------------------------------------
static std::string lc(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }

static void buildLoadBrowser()
{
  if (!g_showLoad) return;
  ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Load disk / cartridge", &g_showLoad)) {
    namespace fs = std::filesystem;
    static std::string rel;                          // current dir relative to the SD root ("" = root)
    std::string root = desktopSdRoot();
    std::string cur  = rel.empty() ? root : root + "/" + rel;

    ImGui::Text("SD: /%s", rel.c_str());
    ImGui::Text("Platform: %s", dbgPlatformName(dbgPlatform()));
    ImGui::SameLine(); ImGui::TextDisabled("(%s)", dbgFileExts());
    // PC platforms (PC-XT, tiny386) have A: + C: drives — pick which slot a clicked image mounts into
    // (so you can put a boot floppy in A: and a hard disk in C:). Other platforms have one slot.
    static int g_mountSlot = -1;   // -1 = Auto (by size), 0 = A:, 1 = C:
    if (dbgHasDriveSlots()) {
      ImGui::TextDisabled("Mount into:");
      ImGui::SameLine(); ImGui::RadioButton("Auto##slot", &g_mountSlot, -1);
      ImGui::SameLine(); ImGui::RadioButton("A: floppy##slot", &g_mountSlot, 0);
      ImGui::SameLine(); ImGui::RadioButton("C: hard disk##slot", &g_mountSlot, 1);
      // eject buttons — enabled only when that slot holds an image (right-click a green file also ejects)
      bool hasA = dbgMountedSlotPath(0)[0] != 0, hasC = dbgMountedSlotPath(1)[0] != 0;
      ImGui::BeginDisabled(!hasA); if (ImGui::SmallButton("Eject A:")) dbgEjectSlot(0); ImGui::EndDisabled();
      ImGui::SameLine(); ImGui::BeginDisabled(!hasC); if (ImGui::SmallButton("Eject C:")) dbgEjectSlot(1); ImGui::EndDisabled();
      ImGui::TextDisabled("Tip: load A: first, then C: (C: re-POSTs and boots A: if it's bootable).");
    }
    static char filter[64] = "";
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filt", "filter by name...", filter, sizeof(filter));
    ImGui::Separator();

    // current platform's accepted extensions
    std::vector<std::string> exts; { std::string e;
      for (const char *s = dbgFileExts(); ; s++) {
        if (*s == ' ' || *s == 0) { if (!e.empty()) exts.push_back(e); e.clear(); if (*s == 0) break; }
        else e += *s;
      } }
    std::string flc = lc(filter);

    if (ImGui::BeginChild("files")) {
      std::error_code ec;
      std::vector<std::string> dirs, files;
      for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
          if (flc.empty() || lc(name).find(flc) != std::string::npos) dirs.push_back(name);
          continue;
        }
        if (!it->is_regular_file(ec)) continue;
        std::string ext = lc(it->path().extension().string());       // ".dsk"
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        bool ok = false; for (auto &x : exts) if (x == ext) { ok = true; break; }
        if (!ok) continue;
        if (!flc.empty() && lc(name).find(flc) == std::string::npos) continue;
        files.push_back(name);
      }
      std::sort(dirs.begin(), dirs.end());
      std::sort(files.begin(), files.end());

      // ".." goes up one level (only when below the root)
      if (!rel.empty() && ImGui::Selectable("[..]")) {
        size_t slash = rel.find_last_of('/');
        rel = (slash == std::string::npos) ? std::string() : rel.substr(0, slash);
      }
      // sub-folders first (blue), click to enter
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.78f, 1.0f, 1.0f));
      for (auto &d : dirs) {
        std::string label = "[" + d + "]";
        if (ImGui::Selectable(label.c_str())) { rel = rel.empty() ? d : rel + "/" + d; }
      }
      ImGui::PopStyleColor();
      // Same path ignoring a leading "/" and case (mounted names may be stored with or without it).
      auto pathEq = [](const std::string &a, const char *b) {
        if (!b || !*b) return false;
        std::string x = a, y = b;
        if (!x.empty() && x[0] == '/') x.erase(0, 1);
        if (!y.empty() && y[0] == '/') y.erase(0, 1);
        return lc(x) == lc(y);
      };
      // then the loadable files, click to mount. PC platforms show which drive (A:/C:) each is set to.
      for (auto &name : files) {
        std::string sd = "/" + (rel.empty() ? name : rel + "/" + name);
        bool inA = dbgHasDriveSlots() && pathEq(sd, dbgMountedSlotPath(0));
        bool inC = dbgHasDriveSlots() && pathEq(sd, dbgMountedSlotPath(1));
        std::string label = std::string(inA ? "[A:] " : inC ? "[C:] " : "") + name;
        if (inA || inC) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.5f, 1.0f));  // mounted = green
        ImGui::PushID(sd.c_str());
        bool clicked = ImGui::Selectable(label.c_str());
        if (inA || inC) ImGui::PopStyleColor();
        if (inA || inC) {                                   // right-click a mounted image -> eject it
          if (ImGui::BeginPopupContextItem()) {
            if (inA && ImGui::MenuItem("Eject from A:")) dbgEjectSlot(0);
            if (inC && ImGui::MenuItem("Eject from C:")) dbgEjectSlot(1);
            ImGui::EndPopup();
          }
        }
        ImGui::PopID();
        if (clicked) {
          if (dbgHasDriveSlots()) {
            dbgLoadFileToSlot(sd.c_str(), g_mountSlot);      // PC: mount into A:/C:/auto, keep browser open
          } else if (dbgLoadFile(sd.c_str())) {
            g_showLoad = false;                              // single-slot: close on success (Apple II/IIGS re-exec)
          }
        }
      }
      if (dirs.empty() && files.empty()) ImGui::TextDisabled("(no folders or matching files here)");
    }
    ImGui::EndChild();
  }
  ImGui::End();
}

// --- native settings (System > Settings) --------------------------------------------------------
static void buildSettings()
{
  if (!g_showSettings) return;
  ImGui::SetNextWindowSize(ImVec2(340, 240), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Settings", &g_showSettings)) {
    ImGui::Checkbox("Sound", &sound);
    if (dbgAppleModelSupported()) {
      ImGui::SeparatorText("Apple II");
      bool iie = dbgGetAppleIIe();
      if (ImGui::Checkbox("Enhanced IIe (vs II+)", &iie)) dbgSetAppleIIe(iie);   // reboots the machine
      ImGui::Checkbox("Color video", &videoColor);
      ImGui::Checkbox("Smooth upscale", &smoothUpscale);
    }
    ImGui::Separator();
    if (ImGui::Button("Save to EEPROM")) saveConfig();
    ImGui::SameLine(); ImGui::TextDisabled("changes apply live");
  }
  ImGui::End();
}

// --- disk-read heat map: a track x position-in-track grid (Apple II Disk II) --------------------
// read -> green, write -> red (both -> yellow), same convention as the memory map. log-scaled.
static inline ImU32 diskCellColor(uint32_t rd, uint32_t wr, float lmR, float lmW) {
  if (!rd && !wr) return IM_COL32(24, 24, 32, 255);
  int g = rd ? (int)(255.0f * logf(1.0f + (float)rd) / lmR) : 0;
  int r = wr ? (int)(255.0f * logf(1.0f + (float)wr) / lmW) : 0;
  return IM_COL32(r, g, 36, 255);
}

// HD / ROM case: the original track x position-in-track grid.
static void drawDiskHeatGrid(int ROWS, int COLS, float lmR, float lmW)
{
  ImVec2 avail = ImGui::GetContentRegionAvail();
  const float labelW = 30.0f, ch = 12.0f;
  float cw = (avail.x - labelW) / COLS; if (cw < 3) cw = 3;
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  for (int r = 0; r < ROWS; r++) {
    if (r % 5 == 0) { char t[8]; snprintf(t, sizeof(t), "%d", r);
      dl->AddText(ImVec2(p0.x, p0.y + r * ch), IM_COL32(150, 150, 150, 255), t); }
    for (int c = 0; c < COLS; c++) {
      int i = r * COLS + c;
      ImVec2 a(p0.x + labelW + c * cw, p0.y + r * ch);
      dl->AddRectFilled(a, ImVec2(a.x + cw - 1, a.y + ch - 1), diskCellColor(g_dbgDiskHeat[i], g_dbgDiskHeatW[i], lmR, lmW));
    }
  }
  ImGui::Dummy(ImVec2(avail.x, ROWS * ch + 4));
  if (ImGui::IsItemHovered()) {
    ImVec2 m = ImGui::GetIO().MousePos;
    int c = (int)((m.x - (p0.x + labelW)) / cw), r = (int)((m.y - p0.y) / ch);
    if (r >= 0 && r < ROWS && c >= 0 && c < COLS) { int i = r * COLS + c;
      ImGui::SetTooltip("track %d   pos %d/%d   R:%u  W:%u", r, c, COLS, g_dbgDiskHeat[i], g_dbgDiskHeatW[i]); }
  }
}

// FLOPPY case: a circular platter — concentric rings = tracks (0 = outer edge), angular wedges =
// position-in-track (~sectors), filled by read intensity. The current head track gets a yellow ring.
static void drawDiskHeatCircular(int ROWS, int COLS, float lmR, float lmW)
{
  ImVec2 avail = ImGui::GetContentRegionAvail();
  float sz = (avail.x < avail.y ? avail.x : avail.y); if (sz < 140) sz = 140;
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImVec2 ctr(p0.x + avail.x * 0.5f, p0.y + sz * 0.5f);
  float rMax = sz * 0.5f - 6.0f, rHub = rMax * 0.16f, thick = (rMax - rHub) / ROWS;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float TWO_PI = 6.2831853f, A0 = -1.5708f;   // start angle at 12 o'clock
  dl->AddCircleFilled(ctr, rMax + 2.0f, IM_COL32(8, 8, 12, 255), 64);
  const int SUB = 3;                                  // arc sub-steps per wedge for smooth rings
  for (int t = 0; t < ROWS; t++) {
    float rO = rMax - t * thick, rI = rO - thick + 0.6f;
    for (int b = 0; b < COLS; b++) {
      int i = t * COLS + b;
      ImU32 col = diskCellColor(g_dbgDiskHeat[i], g_dbgDiskHeatW[i], lmR, lmW);
      float aw = TWO_PI / COLS;
      for (int s = 0; s < SUB; s++) {
        float aa = A0 + aw * (b + (float)s / SUB), ab = A0 + aw * (b + (float)(s + 1) / SUB);
        ImVec2 i0(ctr.x + rI * cosf(aa), ctr.y + rI * sinf(aa));
        ImVec2 o0(ctr.x + rO * cosf(aa), ctr.y + rO * sinf(aa));
        ImVec2 o1(ctr.x + rO * cosf(ab), ctr.y + rO * sinf(ab));
        ImVec2 i1(ctr.x + rI * cosf(ab), ctr.y + rI * sinf(ab));
        dl->AddQuadFilled(i0, o0, o1, i1, col);
      }
    }
  }
  if (g_dbgDiskTrack >= 0 && g_dbgDiskTrack < ROWS)   // live head position
    dl->AddCircle(ctr, rMax - g_dbgDiskTrack * thick - thick * 0.5f, IM_COL32(255, 225, 110, 255), 64, 1.6f);
  dl->AddCircleFilled(ctr, rHub, IM_COL32(40, 40, 48, 255), 32);
  dl->AddCircle(ctr, rHub, IM_COL32(90, 90, 100, 255), 32, 1.0f);

  ImGui::Dummy(ImVec2(avail.x, sz));
  if (ImGui::IsItemHovered()) {
    ImVec2 m = ImGui::GetIO().MousePos;
    float dx = m.x - ctr.x, dy = m.y - ctr.y, r = sqrtf(dx * dx + dy * dy);
    if (r <= rMax && r >= rHub) {
      int t = (int)((rMax - r) / thick); if (t < 0) t = 0; if (t >= ROWS) t = ROWS - 1;
      float ang = atan2f(dy, dx) - A0; if (ang < 0) ang += TWO_PI;
      int b = (int)(ang / TWO_PI * COLS) % COLS; int i = t * COLS + b;
      ImGui::SetTooltip("track %d   pos %d/%d   R:%u  W:%u", t, b, COLS, g_dbgDiskHeat[i], g_dbgDiskHeatW[i]);
    }
  }
}

// C64 cartridge ROM-access grid: rows = 8K banks, cols = 1K region of the $8000-$BFFF window.
// read intensity -> green (log-scaled); the currently-mapped bank gets a highlighted label.
static void drawCartHeatGrid()
{
  int banks = g_dbgCartMaxBank + 1; if (banks < 1) banks = 1; if (banks > DBG_CART_BANKS) banks = DBG_CART_BANKS;
  const int COLS = DBG_CART_BINS;
  uint32_t mx = 1;
  for (int i = 0; i < banks * COLS; i++) if (g_dbgCartHeat[i] > mx) mx = g_dbgCartHeat[i];
  float lm = logf(1.0f + (float)mx);

  ImVec2 avail = ImGui::GetContentRegionAvail();
  const float labelW = 52.0f, ch = 13.0f;
  float cw = (avail.x - labelW) / COLS; if (cw < 3) cw = 3;
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  for (int r = 0; r < banks; r++) {
    char t[16]; snprintf(t, sizeof(t), "bank %d", r);
    dl->AddText(ImVec2(p0.x, p0.y + r * ch), r == g_dbgCartBank ? IM_COL32(255, 225, 110, 255)
                                                                : IM_COL32(150, 150, 150, 255), t);
    for (int c = 0; c < COLS; c++) {
      uint32_t rd = g_dbgCartHeat[r * COLS + c];
      ImU32 col = rd ? IM_COL32(36, (int)(255.0f * logf(1.0f + (float)rd) / lm), 40, 255)
                     : IM_COL32(24, 24, 32, 255);
      ImVec2 a(p0.x + labelW + c * cw, p0.y + r * ch);
      dl->AddRectFilled(a, ImVec2(a.x + cw - 1, a.y + ch - 1), col);
    }
  }
  ImGui::Dummy(ImVec2(avail.x, banks * ch + 4));
  if (ImGui::IsItemHovered()) {
    ImVec2 m = ImGui::GetIO().MousePos;
    int c = (int)((m.x - (p0.x + labelW)) / cw), r = (int)((m.y - p0.y) / ch);
    if (r >= 0 && r < banks && c >= 0 && c < COLS) {
      uint16_t lo = (uint16_t)(0x8000 + c * (0x4000 / COLS));
      ImGui::SetTooltip("bank %d   $%04X..$%04X   reads: %u", r, lo, (uint16_t)(lo + 0x4000 / COLS - 1),
                        g_dbgCartHeat[r * COLS + c]);
    }
  }
}

static void buildDiskHeatPanel()
{
  if (!g_showDisk) return;
  if (ImGui::Begin("Disk read", &g_showDisk)) {
    if (!dbgDiskHeatSupported()) {
      ImGui::TextDisabled("(disk-read heat map: Apple II / C64, or an MSX with a .dsk mounted)");
      ImGui::End(); return;
    }
    bool on = dbgDiskHeatEnabled();
    if (ImGui::Checkbox("Record", &on)) dbgDiskHeatEnable(on);
    ImGui::SameLine(); if (ImGui::Button("Clear")) dbgDiskHeatClear();
    ImGui::SameLine(); ImGui::Checkbox("Fade", &g_diskFade);
    if (on && g_diskFade) dbgDiskHeatDecay(0.94f);

    // A mounted .crt cartridge -> show the cartridge ROM bank-access map instead of the disk platter.
    if (dbgCartActive()) {
      ImGui::SameLine(); ImGui::Text("bank: %d / %d", g_dbgCartBank, dbgCartBankCount());
      ImGui::TextDisabled("rows = 8K cart banks, cols = 1K of $8000-$BFFF;  green = read (exec/data)");
      if (!on) { ImGui::TextDisabled("Enable Record, then run the cartridge."); ImGui::End(); return; }
      drawCartHeatGrid();
      ImGui::End(); return;
    }

    ImGui::SameLine(); ImGui::Text("track: %d", g_dbgDiskTrack);
    bool floppy = dbgDiskIsFloppy();
    ImGui::TextDisabled(floppy ? "rings = tracks (0 = edge), wedges = sector;  green = read, red = write"
                               : "rows = tracks, cols = position;  green = read, red = write");
    if (!on) { ImGui::TextDisabled("Enable Record, then boot/load from disk."); ImGui::End(); return; }

    const int ROWS = dbgDiskTrackCount(), COLS = DBG_DISK_BINS;
    uint32_t mxR = 1, mxW = 1;
    for (int i = 0; i < ROWS * COLS; i++) { if (g_dbgDiskHeat[i] > mxR) mxR = g_dbgDiskHeat[i];
                                            if (g_dbgDiskHeatW[i] > mxW) mxW = g_dbgDiskHeatW[i]; }
    float lmR = logf(1.0f + (float)mxR), lmW = logf(1.0f + (float)mxW);

    if (floppy) drawDiskHeatCircular(ROWS, COLS, lmR, lmW);   // disk image -> circular platter
    else        drawDiskHeatGrid(ROWS, COLS, lmR, lmW);        // HD / block image -> grid
  }
  ImGui::End();
}

// --- VIC-II / SID control windows (C64) ---------------------------------------------------------
// Approximate C64 (colodore) palette for the colour swatches.
static const ImU32 kC64Pal[16] = {
  IM_COL32(0,0,0,255),       IM_COL32(255,255,255,255), IM_COL32(129,51,43,255),   IM_COL32(117,193,201,255),
  IM_COL32(132,62,153,255),  IM_COL32(85,160,73,255),   IM_COL32(58,42,148,255),   IM_COL32(200,214,137,255),
  IM_COL32(135,82,30,255),   IM_COL32(86,63,0,255),     IM_COL32(180,103,93,255),  IM_COL32(78,78,78,255),
  IM_COL32(120,120,120,255), IM_COL32(149,224,138,255), IM_COL32(120,105,209,255), IM_COL32(170,170,170,255) };

// A C64-colour swatch button that opens a 16-colour picker popup; writes the low nibble of *val.
static bool c64ColorPick(const char *id, uint8_t *val) {
  bool changed = false;
  ImGui::PushID(id);
  ImVec4 col = ImGui::ColorConvertU32ToFloat4(kC64Pal[*val & 0x0F]);
  if (ImGui::ColorButton("##sw", col, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18)))
    ImGui::OpenPopup("c64pal");
  if (ImGui::BeginPopup("c64pal")) {
    for (int i = 0; i < 16; i++) {
      ImGui::PushID(i);
      ImVec4 c = ImGui::ColorConvertU32ToFloat4(kC64Pal[i]);
      if (ImGui::ColorButton("##c", c, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18))) {
        *val = (uint8_t)((*val & 0xF0) | i); changed = true; ImGui::CloseCurrentPopup();
      }
      ImGui::PopID();
      if ((i & 7) != 7) ImGui::SameLine();
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

static void buildVicPanel()
{
  if (!g_showVic) return;
  if (ImGui::Begin("VIC-II", &g_showVic)) {
    if (!dbgVicSupported()) { ImGui::TextDisabled("(VIC-II only on the Commodore 64)"); ImGui::End(); return; }
    uint8_t r[0x2F];
    if (dbgVicReadRegs(r, 0x2F) < 0x2F) { ImGui::TextDisabled("(VIC state unavailable)"); ImGui::End(); return; }
    uint16_t bank = dbgVicBankBase();
    uint8_t d011 = r[0x11], d016 = r[0x16], d018 = r[0x18];
    bool ecm = d011 & 0x40, bmm = d011 & 0x20, den = d011 & 0x10, mcm = d016 & 0x10;
    const char *mode = bmm ? (mcm ? "Multicolour bitmap" : "Hi-res bitmap")
                           : ecm ? "Extended-BG text" : mcm ? "Multicolour text" : "Standard text";
    uint16_t screen = (uint16_t)(bank + (((d018 >> 4) & 0x0F) * 0x0400));
    uint16_t charB  = (uint16_t)(bank + (((d018 >> 1) & 0x07) * 0x0800));
    uint16_t bmapB  = (uint16_t)(bank + ((d018 & 0x08) ? 0x2000 : 0x0000));
    unsigned raster = (unsigned)(r[0x12] | ((d011 & 0x80) ? 0x100 : 0));

    ImGui::Text("Mode: %s%s", mode, den ? "" : "  (display OFF)");
    ImGui::Text("VIC bank: %d   base $%04X", (bank >> 14) & 3, bank);
    ImGui::Text("Screen RAM: $%04X", screen);
    if (bmm) ImGui::Text("Bitmap:     $%04X", bmapB);
    else     ImGui::Text("Charset:    $%04X%s", charB,
                         (charB == 0x1000 || charB == 0x1800 || charB == 0x9000 || charB == 0x9800) ? "  (char ROM)" : "");
    ImGui::Text("Raster: %u   Scroll X:%d Y:%d   %s x %s",
                raster, d016 & 7, d011 & 7, (d016 & 8) ? "40col" : "38col", (d011 & 8) ? "25row" : "24row");

    ImGui::SeparatorText("Colours");
    static const struct { const char *l; int idx; } cols[] = {
      {"Border", 0x20}, {"Background 0", 0x21}, {"Background 1", 0x22}, {"Background 2", 0x23}, {"Background 3", 0x24} };
    for (auto &c : cols) {
      uint8_t v = r[c.idx];
      if (c64ColorPick(c.l, &v)) dbgVicWriteReg(c.idx, v);
      ImGui::SameLine(); ImGui::Text("%s  ($D0%02X = %u)", c.l, c.idx, r[c.idx] & 0x0F);
    }

    ImGui::SeparatorText("Sprites");
    uint8_t en = r[0x15], msb = r[0x10], expx = r[0x1D], expy = r[0x17], mcr = r[0x1C], pri = r[0x1B];
    if (ImGui::BeginTable("spr", 6, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
      ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn("On"); ImGui::TableSetupColumn("X");
      ImGui::TableSetupColumn("Y"); ImGui::TableSetupColumn("Col"); ImGui::TableSetupColumn("Flags");
      ImGui::TableHeadersRow();
      for (int s = 0; s < 8; s++) {
        int x = r[0x00 + s * 2] | ((msb & (1 << s)) ? 0x100 : 0);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d", s);
        ImGui::TableNextColumn();
        ImGui::TextColored((en & (1 << s)) ? ImVec4(0.4f,1,0.5f,1) : ImVec4(0.5f,0.5f,0.5f,1), "%s", (en & (1 << s)) ? "on" : "-");
        ImGui::TableNextColumn(); ImGui::Text("%d", x);
        ImGui::TableNextColumn(); ImGui::Text("%d", r[0x01 + s * 2]);
        ImGui::TableNextColumn();
        { uint8_t cv = r[0x27 + s]; ImGui::PushID(s); if (c64ColorPick("sc", &cv)) dbgVicWriteReg(0x27 + s, cv); ImGui::PopID(); }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s%s%s%s", (mcr & (1 << s)) ? "MC " : "", (expx & (1 << s)) ? "X2 " : "",
                            (expy & (1 << s)) ? "Y2 " : "", (pri & (1 << s)) ? "bg" : "");
      }
      ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Raw registers")) {
      if (ImGui::BeginTable("vicregs", 4, ImGuiTableFlags_BordersInner | ImGuiTableFlags_RowBg)) {
        for (int i = 0; i < 0x2F; i++) {
          ImGui::TableNextColumn();
          ImGui::PushID(i);
          ImGui::TextDisabled("D0%02X", i); ImGui::SameLine();
          ImGui::SetNextItemWidth(40);
          uint8_t v8 = r[i];
          if (ImGui::InputScalar("##v", ImGuiDataType_U8, &v8, nullptr, nullptr, "%02X", ImGuiInputTextFlags_CharsHexadecimal))
            dbgVicWriteReg(i, v8);
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
    }
  }
  ImGui::End();
}

static void buildSidPanel()
{
  if (!g_showSid) return;
  if (ImGui::Begin("SID", &g_showSid)) {
    if (!dbgSidSupported()) { ImGui::TextDisabled("(SID only on the Commodore 64)"); ImGui::End(); return; }
    uint8_t r[0x19];
    if (dbgSidReadRegs(r, 0x19) < 0x19) { ImGui::TextDisabled("(SID state unavailable)"); ImGui::End(); return; }
    static const char *st[5] = {"off", "attack", "decay", "sustain", "release"};
    for (int v = 0; v < 3; v++) {
      ImGui::PushID(v);
      int b = v * 7;
      uint16_t freq = r[b] | (r[b + 1] << 8);
      uint16_t pw   = (r[b + 2] | (r[b + 3] << 8)) & 0x0FFF;
      uint8_t ctrl = r[b + 4], ad = r[b + 5], sr = r[b + 6];
      float hz = freq * 0.0587f;   // PAL: Freg * 985248 / 16777216
      char title[16]; snprintf(title, sizeof(title), "Voice %d", v + 1);
      ImGui::SeparatorText(title);
      ImGui::Text("Freq $%04X (~%.0f Hz)   PW $%03X (%.0f%%)", freq, hz, pw, pw * 100.0f / 4095.0f);
      char w[40] = "";
      if (ctrl & 0x10) strcat(w, "tri ");
      if (ctrl & 0x20) strcat(w, "saw ");
      if (ctrl & 0x40) strcat(w, "pulse ");
      if (ctrl & 0x80) strcat(w, "noise ");
      ImGui::Text("Wave: %-16s %s%s%s%s", w[0] ? w : "(none) ",
                  (ctrl & 0x01) ? "GATE " : "", (ctrl & 0x02) ? "SYNC " : "",
                  (ctrl & 0x04) ? "RING " : "", (ctrl & 0x08) ? "TEST" : "");
      ImGui::Text("ADSR: A=%u D=%u S=%u R=%u", ad >> 4, ad & 0x0F, sr >> 4, sr & 0x0F);
      float env = 0; uint8_t state = 0; dbgSidVoice(v, &env, &state);
      ImGui::Text("Env: %-8s", st[state < 5 ? state : 0]);
      ImGui::SameLine(); ImGui::ProgressBar(env / 255.0f, ImVec2(-1, 0));
      ImGui::PopID();
    }
    ImGui::SeparatorText("Filter / master");
    uint16_t cutoff = (r[0x15] & 7) | (r[0x16] << 3);
    uint8_t froute = r[0x17] & 0x0F, mvol = r[0x18];
    ImGui::Text("Cutoff $%03X   Resonance %u   Routing %c%c%c%s",
                cutoff, r[0x17] >> 4, (froute & 1) ? '1' : '-', (froute & 2) ? '2' : '-',
                (froute & 4) ? '3' : '-', (froute & 8) ? " extIN" : "");
    ImGui::Text("Volume %u   Filter %s%s%s%s", mvol & 0x0F,
                (mvol & 0x10) ? "LP " : "", (mvol & 0x20) ? "BP " : "",
                (mvol & 0x40) ? "HP " : "", (mvol & 0x80) ? "voice3-off" : "");
    if (ImGui::CollapsingHeader("Raw registers")) {
      if (ImGui::BeginTable("sidregs", 4, ImGuiTableFlags_BordersInner | ImGuiTableFlags_RowBg)) {
        for (int i = 0; i < 0x19; i++) {
          ImGui::TableNextColumn();
          ImGui::PushID(i);
          ImGui::TextDisabled("D4%02X", i); ImGui::SameLine();
          ImGui::SetNextItemWidth(40);
          uint8_t v8 = r[i];
          if (ImGui::InputScalar("##v", ImGuiDataType_U8, &v8, nullptr, nullptr, "%02X", ImGuiInputTextFlags_CharsHexadecimal))
            dbgSidWriteReg(i, v8);
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
    }
  }
  ImGui::End();
}

// NES PPU / VRAM inspector — the analog of the C64 VIC-II panel. Shows the palette (live swatches),
// both pattern tables (CHR), all four nametables (VRAM) with the current scroll viewport, and OAM.
static void buildPpuPanel()
{
  if (!g_showPpu) return;
  if (ImGui::Begin("PPU / VRAM", &g_showPpu)) {
    if (!dbgNesPpuSupported()) { ImGui::TextDisabled("(PPU/VRAM only on the NES)"); ImGui::End(); return; }

    // --- Palette RAM ($3F00-$3F1F): 16 BG + 16 sprite entries, each resolved via the master palette ---
    uint8_t pal[0x20];
    if (dbgNesReadPalette(pal, 0x20) >= 0x20) {
      ImGui::SeparatorText("Palette  ($3F00-$3F1F)");
      auto swatchRow = [&](const char *label, int base) {
        ImGui::TextDisabled("%s", label); ImGui::SameLine(70);
        for (int i = 0; i < 16; i++) {
          ImGui::PushID(base + i);
          ImVec4 c = ImGui::ColorConvertU32ToFloat4(dbgNesMasterRGBA(pal[base + i]));
          ImGui::ColorButton("##s", c, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16));
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("$3F%02X = $%02X", base + i, pal[base + i]);
          ImGui::PopID();
          if (i != 15) ImGui::SameLine(0, ((i & 3) == 3) ? 8.0f : 2.0f);   // gap between 4-colour groups
        }
      };
      swatchRow("BG",     0x00);
      swatchRow("Sprite", 0x10);
    }

    // --- Pattern tables (CHR): two 128x128 tile sheets ($0000 left, $1000 right), colourised by a
    //     selectable 4-colour palette. Scaled to fit the panel width (nearest -> crisp pixels). ---
    ImGui::SeparatorText("Pattern tables (CHR)");
    static int patPal = 0;
    ImGui::SetNextItemWidth(140);
    ImGui::Combo("Palette##pat", &patPal, "BG 0\0BG 1\0BG 2\0BG 3\0SPR 0\0SPR 1\0SPR 2\0SPR 3\0");
    static uint32_t pbuf[2][128 * 128];
    float ts = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
    if (ts > 256.0f) ts = 256.0f; if (ts < 96.0f) ts = 96.0f;
    for (int half = 0; half < 2; half++) {
      if (!g_ppuPatTex[half]) {
        g_ppuPatTex[half] = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 128, 128);
        SDL_SetTextureScaleMode(g_ppuPatTex[half], SDL_ScaleModeNearest);
      }
      dbgNesRenderPattern(half, patPal, pbuf[half]);
      SDL_UpdateTexture(g_ppuPatTex[half], nullptr, pbuf[half], 128 * sizeof(uint32_t));
      ImGui::Image((ImTextureID)(intptr_t)g_ppuPatTex[half], ImVec2(ts, ts));
      if (half == 0) ImGui::SameLine();
    }
    ImGui::TextDisabled("left $0000   right $1000");

    // --- Nametables (VRAM): all four logical tables (512x480), with the live scroll viewport drawn ---
    ImGui::SeparatorText("Nametables (VRAM)");
    if (!g_ntTex) {
      g_ntTex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 512, 480);
      SDL_SetTextureScaleMode(g_ntTex, SDL_ScaleModeNearest);
    }
    static uint32_t *ntbuf = (uint32_t *)malloc(512 * 480 * sizeof(uint32_t));
    if (ntbuf) {
      dbgNesRenderNametables(ntbuf);
      SDL_UpdateTexture(g_ntTex, nullptr, ntbuf, 512 * sizeof(uint32_t));
      float w = ImGui::GetContentRegionAvail().x; if (w > 512.0f) w = 512.0f; if (w < 128.0f) w = 128.0f;
      float scale = w / 512.0f;
      ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::Image((ImTextureID)(intptr_t)g_ntTex, ImVec2(w, 480.0f * scale));
      int vx = 0, vy = 0; dbgNesViewport(&vx, &vy);   // 256x240 viewport (wraps the 512x480 torus)
      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImVec2 a(p.x + vx * scale, p.y + vy * scale);
      dl->AddRect(a, ImVec2(a.x + 256 * scale, a.y + 240 * scale), IM_COL32(255, 80, 80, 220), 0, 0, 1.5f);
    }

    // --- OAM (sprite table): 64 entries Y/tile/attr/X, like the VIC-II sprite table ---
    if (ImGui::CollapsingHeader("OAM (sprites)")) {
      uint8_t oam[0x100];
      if (dbgNesReadOam(oam, 0x100) >= 0x100 &&
          ImGui::BeginTable("oam", 6, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
        ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn("X"); ImGui::TableSetupColumn("Y");
        ImGui::TableSetupColumn("Tile"); ImGui::TableSetupColumn("Pal"); ImGui::TableSetupColumn("Flags");
        ImGui::TableHeadersRow();
        for (int s = 0; s < 64; s++) {
          uint8_t y = oam[s * 4 + 0], tile = oam[s * 4 + 1], attr = oam[s * 4 + 2], x = oam[s * 4 + 3];
          bool onScreen = (y < 0xEF);
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextColored(onScreen ? ImVec4(1,1,1,1) : ImVec4(0.5f,0.5f,0.5f,1), "%d", s);
          ImGui::TableNextColumn(); ImGui::Text("%d", x);
          ImGui::TableNextColumn(); ImGui::Text("%d", y);
          ImGui::TableNextColumn(); ImGui::Text("$%02X", tile);
          ImGui::TableNextColumn(); ImGui::Text("%d", 4 + (attr & 3));   // sprite palettes are 4..7
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s%s%s", (attr & 0x20) ? "bg " : "", (attr & 0x40) ? "Hflip " : "",
                              (attr & 0x80) ? "Vflip" : "");
        }
        ImGui::EndTable();
      }
    }
  }
  ImGui::End();
}

// MSX TMS9918 VDP / VRAM inspector — the analog of the NES PPU panel. The VDP's 16K VRAM is a separate
// bus (ports $98/$99), so it can't appear in the CPU Memory panel; this shows the fixed palette, the
// five table base addresses (derived from R0-R7), a pattern-generator tile sheet, the sprite attribute
// table, and a raw editable VRAM hex view.
static void buildVdpPanel()
{
  if (!g_showVdp) return;
  if (ImGui::Begin("VDP / VRAM", &g_showVdp)) {
    if (!dbgMsxVdpSupported()) { ImGui::TextDisabled("(VDP/VRAM only on the MSX)"); ImGui::End(); return; }
    uint8_t r[8] = {0}; dbgMsxVdpRegs(r, 8);
    bool m1 = (r[1] & 0x10), m2 = (r[0] & 0x02), m3 = (r[1] & 0x08);   // mode-select bits
    const char *mode = m1 ? "Text 1 (40x24)" : m2 ? "Graphic 2 (256x192)" : m3 ? "Multicolor" : "Graphic 1 (32x24)";
    ImGui::Text("Mode: %s", mode);
    ImGui::SameLine(); ImGui::TextDisabled(" display %s", (r[1] & 0x40) ? "ON" : "off");

    // --- fixed 16-colour TMS9918 palette ---
    ImGui::SeparatorText("Palette (TMS9918, fixed)");
    for (int i = 0; i < 16; i++) {
      ImGui::PushID(i);
      ImVec4 c = ImGui::ColorConvertU32ToFloat4(dbgMsxPaletteRGBA(i));
      ImGui::ColorButton("##s", c, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("colour %d", i);
      ImGui::PopID();
      if (i != 15) ImGui::SameLine(0, (i & 3) == 3 ? 8.0f : 2.0f);
    }

    // --- VRAM table base addresses (derived from the VDP registers, mode-aware) ---
    uint16_t nameB = (uint16_t)((r[2] & 0x0F) << 10);
    uint16_t patB  = m2 ? ((r[4] & 0x04) ? 0x2000 : 0x0000) : (uint16_t)((r[4] & 0x07) << 11);
    uint16_t colB  = m2 ? ((r[3] & 0x80) ? 0x2000 : 0x0000) : (uint16_t)(r[3] << 6);
    uint16_t satB  = (uint16_t)((r[5] & 0x7F) << 7);
    uint16_t sppB  = (uint16_t)((r[6] & 0x07) << 11);
    ImGui::SeparatorText("VRAM tables");
    if (ImGui::BeginTable("vtab", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
      auto trow = [&](const char *n, uint16_t a) {
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", n);
        ImGui::TableNextColumn(); ImGui::Text("$%04X", a);
      };
      trow("Name table", nameB); trow("Pattern generator", patB); trow("Colour table", colB);
      trow("Sprite attributes", satB); trow("Sprite patterns", sppB);
      ImGui::EndTable();
    }

    // --- pattern generator as a 16x16 tile sheet (128x128, white = set bit) ---
    ImGui::SeparatorText("Pattern generator");
    static int patBank = 0;
    if (m2) { ImGui::SetNextItemWidth(150); ImGui::Combo("Bank##vdp", &patBank, "0 (rows 0-7)\0001 (rows 8-15)\0002 (rows 16-23)\0"); }
    else patBank = 0;
    if (!g_vdpPatTex) {
      g_vdpPatTex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 128, 128);
      SDL_SetTextureScaleMode(g_vdpPatTex, SDL_ScaleModeNearest);
    }
    static uint32_t vpbuf[128 * 128];
    dbgMsxRenderPatterns(vpbuf, patBank);
    SDL_UpdateTexture(g_vdpPatTex, nullptr, vpbuf, 128 * sizeof(uint32_t));
    float ts = ImGui::GetContentRegionAvail().x; if (ts > 256.0f) ts = 256.0f; if (ts < 128.0f) ts = 128.0f;
    ImGui::Image((ImTextureID)(intptr_t)g_vdpPatTex, ImVec2(ts, ts));
    ImGui::TextDisabled("256 tiles, 16x16 grid");

    // --- sprite attribute table (32 entries: Y, X, pattern, colour + flags); $D0 ends the list ---
    if (ImGui::CollapsingHeader("Sprites (32)")) {
      int ssize = (r[1] & 0x02) ? 16 : 8, smag = (r[1] & 0x01) ? 2 : 1;
      ImGui::TextDisabled("size %dx%d   magnify x%d", ssize, ssize, smag);
      if (ImGui::BeginTable("vspr", 6, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
        ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn("X"); ImGui::TableSetupColumn("Y");
        ImGui::TableSetupColumn("Pat"); ImGui::TableSetupColumn("Col"); ImGui::TableSetupColumn("Flags");
        ImGui::TableHeadersRow();
        bool ended = false;
        for (int s = 0; s < 32; s++) {
          uint8_t y = dbgMsxPeekVram(satB + s * 4 + 0);
          if (y == 0xD0) ended = true;                       // list terminator: this + rest are inactive
          uint8_t x = dbgMsxPeekVram(satB + s * 4 + 1);
          uint8_t pat = dbgMsxPeekVram(satB + s * 4 + 2);
          uint8_t cb = dbgMsxPeekVram(satB + s * 4 + 3);
          ImVec4 col = ended ? ImVec4(0.5f, 0.5f, 0.5f, 1) : ImVec4(1, 1, 1, 1);
          ImGui::TableNextRow();
          ImGui::TableNextColumn(); ImGui::TextColored(col, "%d", s);
          ImGui::TableNextColumn(); ImGui::TextColored(col, "%d", (cb & 0x80) ? x - 32 : x);   // early clock
          ImGui::TableNextColumn(); ImGui::TextColored(col, "%d", y);
          ImGui::TableNextColumn(); ImGui::TextColored(col, "$%02X", pat);
          ImGui::TableNextColumn();
          ImGui::PushID(s);
          ImVec4 sc = ImGui::ColorConvertU32ToFloat4(dbgMsxPaletteRGBA(cb & 0x0F));
          ImGui::ColorButton("##sc", sc, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
          ImGui::SameLine(); ImGui::TextColored(col, "%d", cb & 0x0F);
          ImGui::PopID();
          ImGui::TableNextColumn();
          ImGui::TextColored(col, "%s%s", (cb & 0x80) ? "EC " : "", (y == 0xD0) ? "END" : "");
        }
        ImGui::EndTable();
      }
    }

    // --- raw VRAM hex ($0000-$3FFF), editable (writes show next frame) ---
    ImGui::SeparatorText("VRAM  ($0000-$3FFF)");
    g_vramEdit.ReadFn = vramRead; g_vramEdit.WriteFn = vramWrite;
    g_vramEdit.DrawContents(nullptr, 0x4000, 0);
  }
  ImGui::End();
}

void desktopUiFrame(SDL_Texture *emuTex, int fbW, int fbH)
{
  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();

  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGuiID dockId = ImGui::DockSpaceOverViewport(0, vp, ImGuiDockNodeFlags_None);
  if (g_buildLayout) {
    g_buildLayout = false;
    // default layout: emulator fills the left ~2/3, debug panels stack on the right
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, vp->Size);
    ImGuiID rightId, leftId;
    ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, 0.34f, &rightId, &leftId);
    ImGui::DockBuilderDockWindow("Emulator", leftId);
    // right column stacked top->bottom: Control, CPU, Memory (all visible at once)
    ImGuiID ctrlId, rest1;
    ImGui::DockBuilderSplitNode(rightId, ImGuiDir_Up, 0.34f, &ctrlId, &rest1);
    ImGui::DockBuilderDockWindow("Control", ctrlId);
    ImGuiID cpuId, memId;
    ImGui::DockBuilderSplitNode(rest1, ImGuiDir_Up, 0.42f, &cpuId, &memId);
    ImGui::DockBuilderDockWindow("CPU", cpuId);
    ImGui::DockBuilderDockWindow("I/O", cpuId);         // tabbed with CPU
    ImGui::DockBuilderDockWindow("Disassembly", memId);
    ImGui::DockBuilderDockWindow("Breakpoints", memId); // tabbed with Disassembly
    ImGui::DockBuilderDockWindow("Memory", memId);
    ImGui::DockBuilderDockWindow("Heat map", memId);
    ImGui::DockBuilderDockWindow("Disk read", memId);
    ImGui::DockBuilderDockWindow("Audio spectrum", memId);
    ImGui::DockBuilderDockWindow("VIC-II", memId);
    ImGui::DockBuilderDockWindow("SID", memId);
    ImGui::DockBuilderDockWindow("PPU / VRAM", memId);
    ImGui::DockBuilderFinish(dockId);
  }

  // debug hotkeys (F5 pause/resume, F10 step) — handled here so they work over the whole window
  if (!ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false))  dbgSetPaused(!dbgIsPaused());
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) dbgStep();
  }

  // Offline self-check: EMU_DBG_STEP=N issues one single-step per frame (so the CPU thread actually
  // runs each), proving stepping advances PC. Paired with EMU_DBG_PAUSE + a capture.
  static int autoStep = []{ const char *s = getenv("EMU_DBG_STEP"); return s ? atoi(s) : 0; }();
  if (autoStep > 0) { dbgStep(); autoStep--; }

  // Offline self-test: EMU_DBG_SWAP=/img.dsk hot-swaps the disk at frame EMU_DBG_SWAP_AT (default 250)
  // to prove it does NOT re-exec (no second "DiskII Setup"/"Ready." in the log).
  static const char *swapTo = getenv("EMU_DBG_SWAP");
  static long swapAt = []{ const char *s = getenv("EMU_DBG_SWAP_AT"); return s ? atol(s) : 250L; }();
  static long swFrame = -1; swFrame++;
  if (swapTo && swFrame == swapAt) { fprintf(stderr, "[swap] -> %s\n", swapTo); dbgLoadFile(swapTo); }
  static long rebootAt = []{ const char *s = getenv("EMU_DBG_REBOOT_AT"); return s ? atol(s) : 0L; }();
  if (rebootAt > 0 && swFrame == rebootAt) { fprintf(stderr, "[reboot] emulator-only reset\n"); dbgReset(); }

  buildMenuBar();
  buildDisplayWindow(emuTex, fbW, fbH);
  buildControlPanel();
  buildCpuPanel();
  buildIoPanel();
  buildDisasmPanel();
  buildBpPanel();
  buildMemoryPanel();
  buildHeatPanel();
  buildDiskHeatPanel();
  buildSpectrumPanel();
  buildVicPanel();
  buildSidPanel();
  buildPpuPanel();
  buildVdpPanel();
  buildLoadBrowser();
  buildSettings();
  desktopUiAutoSaveCfg();   // continuously persist view options (debounced), not just on quit
  if (g_showDemo) ImGui::ShowDemoWindow(&g_showDemo);

  ImGui::Render();
  SDL_SetRenderDrawColor(g_ren, 12, 12, 14, 255);
  SDL_RenderClear(g_ren);
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_ren);
  uiMaybeCapture();                    // optional offline screenshot of the full composited window
  SDL_RenderPresent(g_ren);
}

#endif // BOARD_DESKTOP
