// device.cpp — the simulated board (see device.h): device selection, persistence and the re-exec
// that moves between emu8.exe and emu8-picocalc.exe.
#if defined(BOARD_DESKTOP)

#include "device.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if defined(_WIN32)
  #include <process.h>     // _execv
#else
  #include <unistd.h>      // execv
#endif

extern char **g_emuArgv;          // hal.cpp: ESP.restart() re-execs g_emuArgv[0] with these args
const char *desktopBaseDir();     // hal.cpp
void desktopUiSaveState();        // ui_imgui.cpp

static const DesktopDeviceInfo kDevices[DEV_COUNT] = {
  { "cyd",             "CYD (ESP32, 320x240)",                   320,  240, false, true  },
  { "s3",              "ESP32-S3 JC4827W543 (480x272)",          480,  272, false, true  },
  { "p4",              "ESP32-P4 JC1060P470 (1024x600)",        1024,  600, false, true  },
  { "picocalc-rp2040", "PicoCalc - Raspberry Pi Pico (RP2040)",  320,  320, true,  false },
  { "picocalc-rp2350", "PicoCalc - Raspberry Pi Pico 2 (RP2350)",320,  320, true,  true  },
};

#if defined(EMU_SIM_PICOCALC)
static const bool kThisIsPicoCalc = true;
static int g_dev = DEV_PICOCALC_RP2040;
#else
static const bool kThisIsPicoCalc = false;
static int g_dev = DEV_CYD;
#endif

#if defined(_WIN32)
static const char *kExeEsp32 = "emu8.exe", *kExePicoCalc = "emu8-picocalc.exe";
#else
static const char *kExeEsp32 = "emu8",     *kExePicoCalc = "emu8-picocalc";
#endif

static std::vector<std::string> g_args;   // argv minus argv[0] and any --device=
static std::vector<std::string> g_argStore;
static std::vector<char *>      g_argv;   // what g_emuArgv points at

int desktopDevice() { return g_dev; }
const DesktopDeviceInfo &desktopDeviceInfo(int dev) {
  return kDevices[(dev >= 0 && dev < DEV_COUNT) ? dev : 0];
}
bool desktopDeviceHasPcxt() { return desktopDeviceInfo().pcxt; }

static int deviceFromId(const char *id) {
  if (!id) return -1;
  for (int i = 0; i < DEV_COUNT; i++) if (strcmp(id, kDevices[i].id) == 0) return i;
  if (strcmp(id, "picocalc") == 0) return DEV_PICOCALC_RP2350;
  return -1;
}

static std::string exePath(bool picocalc) {
  return std::string(desktopBaseDir()) + (picocalc ? kExePicoCalc : kExeEsp32);
}
static bool fileExists(const std::string &p) {
  FILE *f = fopen(p.c_str(), "rb");
  if (f) fclose(f);
  return f != nullptr;
}
bool desktopDeviceAvailable(int dev) {
  return desktopDeviceInfo(dev).picocalc == kThisIsPicoCalc || fileExists(exePath(desktopDeviceInfo(dev).picocalc));
}

// The last device used, shared by both executables so either one opens on it.
static std::string cfgPath() { return std::string(desktopBaseDir()) + "emu8-device.cfg"; }
static int loadSavedDevice() {
  FILE *f = fopen(cfgPath().c_str(), "r");
  if (!f) return -1;
  char id[64] = {0};
  int d = (fscanf(f, "device=%63s", id) == 1) ? deviceFromId(id) : -1;
  fclose(f);
  return d;
}
static void saveDevice(int dev) {
  if (FILE *f = fopen(cfgPath().c_str(), "w")) { fprintf(f, "device=%s\n", desktopDeviceInfo(dev).id); fclose(f); }
}

// argv for `exe --device=<dev> <the other original args>`; also what g_emuArgv points at.
static char **buildArgv(const std::string &exe, int dev) {
  g_argStore.clear();
  g_argStore.push_back(exe);
  g_argStore.push_back(std::string("--device=") + desktopDeviceInfo(dev).id);
  for (auto &a : g_args) g_argStore.push_back(a);
  g_argv.clear();
  for (auto &s : g_argStore) g_argv.push_back(const_cast<char *>(s.c_str()));
  g_argv.push_back(nullptr);
  return g_argv.data();
}

static void execInto(const std::string &exe, int dev) {
  char **av = buildArgv(exe, dev);
  fflush(nullptr);
#if defined(_WIN32)
  _execv(exe.c_str(), (const char *const *)av);
#else
  execv(exe.c_str(), av);
#endif
  fprintf(stderr, "device: could not start %s\n", exe.c_str());
}

void desktopDeviceInit(int argc, char **argv) {
  int dev = -1;
  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "--device=", 9) == 0) { int d = deviceFromId(argv[i] + 9); if (d >= 0) dev = d; }
    else g_args.push_back(argv[i]);
  }
  if (dev < 0) dev = deviceFromId(getenv("EMU_DEVICE"));
  if (dev < 0) dev = loadSavedDevice();
  if (dev < 0) dev = g_dev;

  if (desktopDeviceInfo(dev).picocalc != kThisIsPicoCalc) {
    std::string other = exePath(desktopDeviceInfo(dev).picocalc);
    if (fileExists(other)) execInto(other, dev);          // returns only if the exec failed
    fprintf(stderr, "device: %s needs %s, which is missing - using this executable's default\n",
            desktopDeviceInfo(dev).id, other.c_str());
    dev = g_dev;
  }
  g_dev = dev;
  saveDevice(dev);
  g_emuArgv = buildArgv(argc > 0 ? argv[0] : exePath(kThisIsPicoCalc), dev);
  printf("device: %s\n", desktopDeviceInfo(dev).name);
}

void desktopSwitchDevice(int dev) {
  if (dev < 0 || dev >= DEV_COUNT || dev == g_dev || !desktopDeviceAvailable(dev)) return;
  desktopUiSaveState();                                   // window/layout + emulator settings
  saveDevice(dev);
  bool pc = desktopDeviceInfo(dev).picocalc;
  execInto(pc == kThisIsPicoCalc ? std::string(g_emuArgv[0]) : exePath(pc), dev);
}

#endif // BOARD_DESKTOP
