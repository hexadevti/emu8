// sd_host.cpp — desktop replacement for src/shared/sd.cpp (excluded on BOARD_DESKTOP).
//
// The emulated SD card is a host directory (g_sdRoot). Default = the repo's sdcard/ (absolute, so the
// ROMs under sdcard/roms/ are found no matter where the .exe is launched from), overridable via the
// EMU_SD_DIR env var. SD-relative paths the cores pass ("/roms/c64/basic.bin") are mapped under it by
// sdHostPath(). Also defines the globals that sd.cpp owned on the device (hspi, gBusLock, SD).
#if defined(BOARD_DESKTOP)

#include "../../emu.h"

#include <sys/stat.h>
#include <cstdlib>
#include <string>
#if defined(_WIN32)
  #include <direct.h>
#endif

// Globals that sd.cpp defines on the device:
SPIClass          hspi { HSPI };
SemaphoreHandle_t gBusLock = NULL;
SDClass           SD;                     // the emulated card (FSTYPE == SD via emu.h)

// SD-card root on the host + relative-path mapper (declared in FS.h). Absolute path to the repo's
// sdcard/ so /roms/<platform>/*.bin resolve regardless of the working directory; EMU_SD_DIR overrides.
std::string g_sdRoot = "C:/Users/lucia/repos/emu8/sdcard";

// The host directory backing the emulated SD card (for the native UI's file browser).
const char *desktopSdRoot() { return g_sdRoot.c_str(); }

std::string sdHostPath(const char *sdRelative) {
  std::string rel = sdRelative ? sdRelative : "/";
  // Already a host path under the root? (e.g. a path we built ourselves) -> pass through.
  if (rel.rfind(g_sdRoot, 0) == 0) return rel;
  if (rel.empty()) return g_sdRoot;
  if (rel[0] != '/') rel = "/" + rel;
  return g_sdRoot + rel;                  // e.g. "./sdcard" + "/game.rom"
}

static void ensureDir(const std::string &p) {
  struct stat st;
  if (stat(p.c_str(), &st) == 0) return;
#if defined(_WIN32)
  _mkdir(p.c_str());
#else
  mkdir(p.c_str(), 0777);
#endif
}

void FSSetup() {
  if (!gBusLock) gBusLock = xSemaphoreCreateMutex();
  hdAttached  = HdDisk;
  diskAttached = !HdDisk;

  if (const char *env = getenv("EMU_SD_DIR")) g_sdRoot = env;
  ensureDir(g_sdRoot);

  sprintf(buf, "SD (host dir): %s", g_sdRoot.c_str());
  printLog(buf);
}

#endif // BOARD_DESKTOP
