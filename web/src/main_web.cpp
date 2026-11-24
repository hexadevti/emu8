// main_web.cpp — browser entry point (Emscripten, -sPROXY_TO_PTHREAD).
//
// main() runs on a worker, so it can block forever like the desktop's: setup(), the CPU loop on its
// own thread, renderLoop() here. The page owns the canvas / audio / touch UI and talks to us through
// the EMSCRIPTEN_KEEPALIVE exports in display_web / audio_web / input_web.
//
// Also the web's take on device.cpp (one fixed "device": an ESP32-class board with PC-XT) and the
// reboot path: the page persists the IndexedDB-backed folders and reloads (web_reboot in app.js).
#if defined(BOARD_DESKTOP) && defined(EMU_WEB)

#include "emu.h"
#include "src/desktop/device.h"
#include <emscripten.h>
#include <thread>
#include <cstdio>

void setup();
void loop();
void desktopResetInit();

// ---- device.h (fixed: the web build is one board) ----
static const DesktopDeviceInfo kWebDev = {"web", "Browser", 320, 240, false, true};
void desktopDeviceInit(int, char **) {}
int desktopDevice() { return DEV_CYD; }
const DesktopDeviceInfo &desktopDeviceInfo(int) { return kWebDev; }
bool desktopDeviceHasPcxt() { return true; }
bool desktopDeviceAvailable(int dev) { return dev == DEV_CYD; }
void desktopSwitchDevice(int) {}
void desktopUiSaveState() {}

int main(int, char **) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  desktopResetInit();
  setup();
  std::thread cpu([]() { while (running) loop(); });
  renderLoop(nullptr);
  running = false;
  if (cpu.joinable()) cpu.join();
  return 0;
}

#endif // BOARD_DESKTOP && EMU_WEB
