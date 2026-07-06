#if !defined(BOARD_JC4827W543)  // tiny386 is not built for the S3 board (too big; vendored core not wired for the device toolchain)
// tiny386.cpp — emu8 glue for the vendored tiny386 i386 PC emulator (hchunhui/tiny386, BSD-3).
//
// This TU includes emu.h (tft / printLog / dispatch surface) and drives the machine through the
// opaque t386_core_* bridge (tiny386_core.cpp). The split exists because pc.h's `PC` machine type
// clashes with the Apple II 6502 program-counter global `PC` (proto.h, pulled in by emu.h).
//
// Threading mirrors the rest of emu8: the i386 runs in tiny386Loop() (core 1 on device / the CPU
// thread on desktop); the framebuffer is pushed to the panel by tiny386RenderFrame() from the core-0
// render loop (src/shared/video.cpp). The VGA core renders RGB565 directly (vga.h BPP=16); we
// nearest-scale that framebuffer to the panel, like pcxtRenderFrame() pushes CGA bands.

#include "emu.h"
#include "tiny386.h"
#include "tiny386_core.h"
#include <dirent.h>
#include <ctype.h>

// Read a whole file from the SD card into a freshly malloc'd buffer (caller frees). load_rom() in
// tiny386_core.cpp uses this to pull SeaBIOS/VGABIOS from /roms/tiny386 (formerly embedded arrays);
// that TU can't include emu.h (pc.h's `PC` machine type clashes), so the SD access lives here.
extern "C" unsigned char* t386_read_sd(const char* sdPath, int* outLen) {
  *outLen = 0;
  File f = FSTYPE.open(sdPath, FILE_READ);
  if (!f) { sprintf(buf, "tiny386: ROM missing: %s", sdPath); printLog(buf); return nullptr; }
  int len = f.size();
  if (len <= 0) { f.close(); return nullptr; }
  unsigned char* b = (unsigned char*)malloc(len);
  if (!b) { f.close(); printLog("tiny386: ROM alloc failed"); return nullptr; }
  int rd = 0;
  while (rd < len) { int n = f.read(b + rd, (len - rd > 8192) ? 8192 : (len - rd)); if (n <= 0) break; rd += n; }
  f.close();
  if (rd != len) { free(b); return nullptr; }
  *outLen = len;
  return b;
}

// ----------------------------------------------------------------------------------------------
// Per-board guest sizing. The P4 (32MB PSRAM) gets a Windows-95-capable machine; the S3 (8MB) a
// reduced DOS/Win3.x one; desktop a middle ground. cpu_gen 3=386, 4=486.
// ----------------------------------------------------------------------------------------------
#if defined(BOARD_JC1060P470)
  // P4: 16 MB guest is plenty for Win95 (the reference runs it in ~7.5 MB). Kept BELOW 32MB PSRAM so
  // the 4MB VGA + the display canvas (1.2MB) + the on-screen-keyboard overlay (1.2MB, allocated on the
  // first OSK open) + the framebuffer all still fit -- at 24MB the OSK overlay alloc failed (no keyboard).
  #define T386_RAM_SIZE   (16u * 1024 * 1024)
  #define T386_VGA_SIZE   ( 4u * 1024 * 1024)
  #define T386_CPU_GEN    4
#elif defined(BOARD_JC4827W543)
  #define T386_RAM_SIZE   ( 6u * 1024 * 1024)   // S3: reduced (DOS / Windows 3.1)
  #define T386_VGA_SIZE   ( 2u * 1024 * 1024)
  #define T386_CPU_GEN    3
#else                                            // desktop / fallback
  #define T386_RAM_SIZE   (16u * 1024 * 1024)
  #define T386_VGA_SIZE   ( 2u * 1024 * 1024)
  #define T386_CPU_GEN    4
#endif

// Framebuffer the VGA core renders into. Sized to hold the largest text/graphics mode we display
// (720x400 text, 640x480 graphics); the active mode is centered in it (vga.c, no SCALE define).
#define T386_FB_W  720
#define T386_FB_H  480

#if defined(BOARD_DESKTOP)
  #define T386_TW 720      // render at native VGA resolution (no 720->320 downscale) so the authentic
  #define T386_TH 480      // VGA text/graphics font stays crisp; the desktop fb is sized to match
  void desktopSetEmuResolution(int w, int h);   // display_sdl.cpp — size the fb before begin()
#elif BOARD_DISPLAY_GFX
  #define T386_TW PANEL_NATIVE_W      // S3 480x272 / P4 1024x600 — render panel-native
  #define T386_TH PANEL_NATIVE_H
#else                                  // CYD (TFT_eSPI 320x240): tiny386 can't run (no PSRAM), but compile
  #define T386_TW 320
  #define T386_TH 240
#endif

static void    *s_pc       = nullptr;   // opaque PC* from the core bridge
static uint16_t *s_band    = nullptr;   // one 8-row scaled output band
static int      *s_xmap    = nullptr;   // [T386_TW] source-column lookup for the nearest scaler
static bool      s_init    = false;
static volatile bool s_resetReq = false;
static char      s_hdaPath[160], s_fdaPath[160];

// Resolve a Settings-selected SD image (the EEPROM marker, e.g. "/win95.img") to a full path ide.c /
// emulink can fopen via the IDF VFS (SD_VFS_ROOT = "/sd"). Desktop: an env var (EMU_T386_HDA/FDA).
static const char *tiny386ResolveImage(const String &sel, const char *desktopEnv, char *buf, size_t bufsz)
{
#if defined(BOARD_DESKTOP)
  (void)sel; (void)buf; (void)bufsz;
  return getenv(desktopEnv);
#else
  (void)desktopEnv;
  if (sel.length() > 1 && sel[0] == '/' && FSTYPE.exists(sel.c_str())) {
    snprintf(buf, bufsz, "%s%s", SD_VFS_ROOT, sel.c_str());
    return buf;
  }
  return nullptr;
#endif
}

// ----------------------------------------------------------------------------------------------
void tiny386Setup()
{
  printLog("TINY386 Setup... (Intel i386 + VGA, SeaBIOS/VGABIOS from /roms/tiny386)");

  menuScreen = (unsigned char *)malloc(0x546);
  menuColor  = (unsigned char *)malloc(0x546);

#if defined(BOARD_DESKTOP)
  desktopSetEmuResolution(T386_TW, T386_TH);   // 720x480 fb so the VGA framebuffer renders ~1:1 (crisp)
#endif

  T386Conf c;
  memset(&c, 0, sizeof(c));
  c.mem_size = T386_RAM_SIZE;
  c.vga_size = T386_VGA_SIZE;
  c.cpu_gen  = T386_CPU_GEN;
  c.fpu      = 0;
  c.fb_w     = T386_FB_W;
  c.fb_h     = T386_FB_H;

  c.hda = tiny386ResolveImage(selectedTiny386FileName,  "EMU_T386_HDA", s_hdaPath, sizeof(s_hdaPath));  // C:
  c.fda = tiny386ResolveImage(selectedTiny386FileNameA, "EMU_T386_FDA", s_fdaPath, sizeof(s_fdaPath));  // A:

  s_pc = t386_core_new(&c);
  if (!s_pc) { printLog("TINY386: core init FAIL (PSRAM?)"); return; }
  s_init = true;

  sprintf(buf, "TINY386 ready. guest=%uMB vga=%uKB%s%s",
          (unsigned)(T386_RAM_SIZE >> 20), (unsigned)(T386_VGA_SIZE >> 10),
          c.hda ? " C:" : "", c.fda ? " A:" : "");
  printLog(buf);
}

// ----------------------------------------------------------------------------------------------
// CPU loop. Each t386_core_step() runs a chunk of i386 instructions plus the device housekeeping
// (PIT/PIC/IDE/keyboard/DMA); t386_core_vga_step() renders into the fb when the VGA signals a
// refresh. We batch several steps per yield, then vTaskDelay so core-0 can render and the WDT is fed.
// ----------------------------------------------------------------------------------------------
void tiny386Loop()
{
  if (!s_init) { for (;;) vTaskDelay(pdMS_TO_TICKS(200)); }

  {   // one-time boot throughput benchmark (~0.5 s)
    uint32_t t0 = millis();
    uint32_t iters = 0;
    while ((uint32_t)(millis() - t0) < 500) { t386_core_step(s_pc); iters++; }
    uint32_t dt = millis() - t0;
    double stepsPerSec = dt > 0 ? (double)iters / ((double)dt / 1000.0) : 0.0;
    double ips = stepsPerSec * t386_core_step_count();
    tiny386MeasuredMhz = (float)(ips / 1000.0);   // ki/s, shown read-only in Settings
    sprintf(buf, "TINY386: %.0f k-i386-instr/s (%.0f pc_steps/s)", ips / 1000.0, stepsPerSec);
    printLog(buf);
  }

  for (;;) {
    if (OptionsWindow) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    if (s_resetReq)    { s_resetReq = false; t386_core_request_reset(s_pc); }
#if defined(BOARD_DESKTOP)
    for (int i = 0; i < 512; i++) t386_core_step(s_pc);   // desktop: big batch, no per-chunk sleep (the
    { int f = 0, on = 0; t386_core_speaker(s_pc, &f, &on); g_pcSpkFreq = f; g_pcSpkOn = on; }
    taskYIELD();             // device's 1ms throttle just slowed the i386 — render/input are other threads
#else
    for (int i = 0; i < 24; i++) t386_core_step(s_pc);
    { int f = 0, on = 0; t386_core_speaker(s_pc, &f, &on); g_pcSpkFreq = f; g_pcSpkOn = on; }  // PC-speaker -> audio ISR
    vTaskDelay(1);                      // feed WDT (the VGA now steps on core 0, in the render loop)
#endif
  }
}

// ----------------------------------------------------------------------------------------------
// Render: nearest-scale the RGB565 framebuffer (T386_FB_W x T386_FB_H) to the panel (T386_TW x
// T386_TH) in 8-row bands. Returns false when nothing changed so the render loop can skip the flush.
// ----------------------------------------------------------------------------------------------
bool tiny386RenderFrame()
{
  if (!s_init) return false;
  t386_core_vga_step(s_pc);   // step the VGA here (core 0) so core 1 stays 100% on the i386 -- big speedup
  uint16_t *fb = t386_core_fb();
  if (!fb) return false;
  if (!t386_core_take_dirty()) return false;

  const int PW = T386_TW, PH = T386_TH;        // target = panel (device) / 320x240 (desktop)

  // Stretch ONLY the active VGA mode region to fill the WHOLE panel -- no unused black area. The mode
  // (dw x dh) is centered in the larger framebuffer with a black border; we sample just that region
  // and scale it to PW x PH. This fills the screen (slight aspect change is accepted for full use).
  int dw = 0, dh = 0;
  t386_core_resolution(s_pc, &dw, &dh);
  if (dw <= 0 || dw > T386_FB_W) dw = T386_FB_W;
  if (dh <= 0 || dh > T386_FB_H) dh = T386_FB_H;
  const int x0 = (T386_FB_W - dw) / 2, y0 = (T386_FB_H - dh) / 2;

  // (Re)build the column map + clear the panel only when the active resolution changes (mode switch).
  static int s_dw = -1, s_dh = -1;
  if (dw != s_dw || dh != s_dh) {
    s_dw = dw; s_dh = dh;
    if (!s_xmap) s_xmap = (int *)malloc((PW > 0 ? PW : 1) * sizeof(int));
    if (s_xmap) for (int px = 0; px < PW; px++) s_xmap[px] = x0 + px * dw / PW;
    tft.fillScreen(TFT_BLACK);
#if BOARD_DISPLAY_GFX
    tft.fillPanelBlack();
#endif
  }
  if (!s_band) s_band = (uint16_t *)malloc((PW > 0 ? PW : 1) * 8 * sizeof(uint16_t));
  if (!s_xmap || !s_band) return false;

  displaySetUiMode(true);                       // UI mode -> the panel flush is 1:1 (no fill-screen zoom)

  for (int py = 0; py < PH; ) {
    int n = (PH - py < 8) ? (PH - py) : 8;
    for (int r = 0; r < n; r++) {
      int sy = y0 + (py + r) * dh / PH;
      const uint16_t *srow = fb + sy * T386_FB_W;
      uint16_t *drow = s_band + r * PW;
      for (int px = 0; px < PW; px++) drow[px] = srow[s_xmap[px]];
    }
#if BOARD_DISPLAY_GFX
    tft.drawCanvasRGB565(0, py, PW, n, s_band);   // fill the whole panel (S3 / P4)
#else
    tft.pushImage(0, py, PW, n, s_band);          // desktop 320x240 fb
#endif
    py += n;
  }
  return true;
}

void tiny386ForceRedraw()
{
  if (s_pc) t386_core_force_redraw(s_pc);
}

void tiny386HardReset()
{
  s_resetReq = true;
}

// ----------------------------------------------------------------------------------------------
// Input. USB HID usage -> PS/2 set-1 scancode -> i8042 (t386_core_put_keycode). ps2_put_keycode()
// takes a raw set-1 make code (<96; break = code|0x80) or 0xe0XX for the extended keys. This mirrors
// the PC-XT's hidToScan() table (XT scancodes == set-1 make codes).
// ----------------------------------------------------------------------------------------------
static uint8_t hidToSet1(uint8_t usage, bool *e0)
{
  *e0 = false;
  if (usage >= 0x04 && usage <= 0x1D) {            // A..Z (HID order) -> set-1 letter codes
    static const uint8_t L[26] = {
      0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
      0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
    return L[usage - 0x04];
  }
  if (usage >= 0x1E && usage <= 0x26) return (uint8_t)(0x02 + (usage - 0x1E));  // 1..9
  if (usage == 0x27) return 0x0B;                  // 0
  switch (usage) {
    case 0x28: return 0x1C;  case 0x29: return 0x01;  case 0x2A: return 0x0E;  // Enter Esc Bksp
    case 0x2B: return 0x0F;  case 0x2C: return 0x39;  case 0x2D: return 0x0C;  // Tab Space -
    case 0x2E: return 0x0D;  case 0x2F: return 0x1A;  case 0x30: return 0x1B;  // = [ ]
    case 0x31: return 0x2B;  case 0x33: return 0x27;  case 0x34: return 0x28;  // \ ; '
    case 0x35: return 0x29;  case 0x36: return 0x33;  case 0x37: return 0x34;  // ` , .
    case 0x38: return 0x35;  case 0x39: return 0x3A;                           // / CapsLock
    case 0x3A: return 0x3B;  case 0x3B: return 0x3C;  case 0x3C: return 0x3D;  // F1..F3
    case 0x3D: return 0x3E;  case 0x3E: return 0x3F;  case 0x3F: return 0x40;  // F4..F6
    case 0x40: return 0x41;  case 0x41: return 0x42;  case 0x42: return 0x43;  // F7..F9
    case 0x43: return 0x44;  case 0x44: return 0x57;  case 0x45: return 0x58;  // F10..F12
    case 0x4F: *e0 = true; return 0x4D;  case 0x50: *e0 = true; return 0x4B;   // Right Left (E0)
    case 0x51: *e0 = true; return 0x50;  case 0x52: *e0 = true; return 0x48;   // Down Up (E0)
    case 0xE0: return 0x1D;  case 0xE1: return 0x2A;  case 0xE2: return 0x38;  // LCtrl LShift LAlt
    case 0xE4: *e0 = true; return 0x1D;  case 0xE5: return 0x36;               // RCtrl RShift
    default:   return 0x00;
  }
}

void tiny386KeyDown(uint8_t hidUsage, bool /*shift*/, bool /*ctrl*/, bool /*alt*/)
{
  if (!s_pc) return;
  bool e0; uint8_t sc = hidToSet1(hidUsage, &e0);
  if (!sc) return;
  t386_core_put_keycode(s_pc, 1, e0 ? (0xe000 | sc) : sc);
}

void tiny386KeyUp(uint8_t hidUsage)
{
  if (!s_pc) return;
  bool e0; uint8_t sc = hidToSet1(hidUsage, &e0);
  if (!sc) return;
  t386_core_put_keycode(s_pc, 0, e0 ? (0xe000 | sc) : sc);
}

// Gamepad -> arrow keys + Enter/Esc (active-low mask: b0 up, b1 down, b2 left, b3 right, b4 A, b5 B).
void tiny386SetInput(uint8_t joyMask)
{
  static uint8_t prev = 0xFF;
  static const struct { uint8_t bit; uint8_t usage; } map[] = {
    {0x01, 0x52}, {0x02, 0x51}, {0x04, 0x50}, {0x08, 0x4F}, {0x10, 0x28}, {0x20, 0x29} };
  for (auto &m : map) {
    bool nowDown  = !(joyMask & m.bit);
    bool prevDown = !(prev   & m.bit);
    if (nowDown && !prevDown)       tiny386KeyDown(m.usage, false, false, false);
    else if (!nowDown && prevDown)  tiny386KeyUp(m.usage);
  }
  prev = joyMask;
}

// USB mouse -> PS/2 mouse (Windows 95 etc.). dx/dy = relative HID deltas (dy screen-down-positive);
// buttons bit0=Left bit1=Right bit2=Middle. Events are ignored until the guest enables the PS/2 mouse.
// Sensitivity multiplier (1 = raw PS/2 deltas; Windows applies its own acceleration on top).
#define T386_MOUSE_SCALE 1
void tiny386MouseInput(int dx, int dy, uint8_t buttons)
{
  if (!s_pc) return;
  t386_core_mouse(s_pc, dx * T386_MOUSE_SCALE, dy * T386_MOUSE_SCALE, 0, buttons & 0x07);
}

// ----------------------------------------------------------------------------------------------
// Settings / file browser (M1/M4). Mounting a new image rebuilds the PC, so persist + reboot.
// ----------------------------------------------------------------------------------------------
bool tiny386LoadSelected(const char * /*path*/) { return false; }   // mounting is via tiny386MountA/C
bool tiny386RenderLoadWarning() { return false; }

// Mount a Settings-selected SD image into the RUNNING machine WITHOUT rebooting the device. The
// marker (selected...FileName[A]) is persisted so it auto-mounts on the next boot too. `sel` is the
// SD-relative path (e.g. "/win95.img"); "" or NULL ejects.
bool tiny386MountA(const char *sel)
{
  if (!s_pc) return false;
  selectedTiny386FileNameA = (sel && sel[0]) ? sel : "";
  saveConfig();
  if (sel && sel[0]) {
    char full[200]; snprintf(full, sizeof(full), "%s%s", SD_VFS_ROOT, sel);
    return t386_core_mount_floppy(s_pc, full) == 0;   // live media change (the guest re-reads A:)
  }
  t386_core_mount_floppy(s_pc, nullptr);               // eject
  return true;
}
bool tiny386MountC(const char *sel)
{
  if (!s_pc) return false;
  selectedTiny386FileName = (sel && sel[0]) ? sel : "";
  saveConfig();
  if (sel && sel[0]) {
    char full[200]; snprintf(full, sizeof(full), "%s%s", SD_VFS_ROOT, sel);
    t386_core_mount_hd(s_pc, full);
  }
  s_resetReq = true;   // re-POST the emulated PC (not the device) so SeaBIOS detects + boots the new C:
  return true;
}

// Scan SD root for disk images (.img/.ima/.vhd/.hdd) into tiny386Files (the Settings disk browser).
#define TINY386_MAX_FILES 200
static bool t386EndsCI(const std::string &s, const char *ext)
{
  size_t n = strlen(ext);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)ext[i])) return false;
  return true;
}
void loadTiny386FilesSync()
{
  tiny386Files.clear();
  DIR *dp = opendir(SD_VFS_ROOT);
  if (dp) {
    struct dirent *de; int scanned = 0;
    while ((de = readdir(dp)) != nullptr) {
      if (de->d_type == DT_DIR) continue;
      std::string nm = de->d_name;
      if (t386EndsCI(nm, ".img") || t386EndsCI(nm, ".ima") ||
          t386EndsCI(nm, ".vhd") || t386EndsCI(nm, ".hdd"))
        tiny386Files.push_back(std::string("/") + nm);
      if ((++scanned & 0x3f) == 0) ::uiDirScanProgress((int)tiny386Files.size());
      if ((int)tiny386Files.size() >= TINY386_MAX_FILES) break;
    }
    closedir(dp);
  }
  sprintf(buf, "TINY386: %d disk image(s) on SD root", (int)tiny386Files.size());
  printLog(buf);
}
void tiny386ScanFiles() { loadTiny386FilesSync(); }

#endif // !defined(BOARD_JC4827W543)
