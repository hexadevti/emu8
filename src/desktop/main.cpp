// main.cpp — desktop entry point. SDL on the MAIN thread (window/events/present), the emulator CPU
// loop() on a worker thread (faithful to the device's core0=render / core1=CPU split).
//
//   SDL_Init -> setup() [platform init + videoSetup; render task NOT spawned on desktop]
//            -> spawn CPU thread running loop()
//            -> renderLoop(NULL) on the main thread (infinite; its displayFlush() pumps SDL events).
//
// Pick the platform with EMU_PLATFORM (apple2|c64|nes|atari|iigs|msx|sms) to boot straight in
// and skip the splash (see eprom.cpp + hal.cpp esp_reset_reason). SD card = ./sdcard (EMU_SD_DIR).
#if defined(BOARD_DESKTOP)

#define SDL_MAIN_HANDLED          // we provide a normal main() — no SDL2main / WinMain hijack
#include <SDL.h>
#include <thread>
#include "../../emu.h"

void setup();   // emu8.ino
void loop();    // emu8.ino

extern char **g_emuArgv;   // hal.cpp: ESP.restart() re-execs this binary for a true reboot

int main(int /*argc*/, char **argv) {
  g_emuArgv = argv;                      // so the "Reboot" button can re-exec instead of exit
  setvbuf(stdout, nullptr, _IONBF, 0);   // live logs (debug tool; also visible if killed)
  setvbuf(stderr, nullptr, _IONBF, 0);
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  setup();                                          // creates the SDL window on this (main) thread

  std::thread cpu([]() { while (running) loop(); });  // emulator CPU (like core 1 on the device)

  renderLoop(nullptr);                              // render + event pump on the main thread (forever)

  running = false;                                  // (reached only if renderLoop ever returns)
  if (cpu.joinable()) cpu.join();
  SDL_Quit();
  return 0;
}

#endif // BOARD_DESKTOP
