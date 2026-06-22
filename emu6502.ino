#include "emu.h"
#include "src/iigs/m0_bench.h"   // IIGS feasibility gate; compiles to nothing unless -DIIGS_M0_BENCH

void setup() {
  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH); // Turn off green LED (boards without an LED define LED_PIN = -1)
  }
  logSetup();
#ifdef IIGS_M0_BENCH
  // Throwaway Apple IIGS memory-feasibility benchmark. Runs at the very top of setup() so the
  // render task / QSPI canvas flush / audio DMA don't perturb timing, then halts so one
  // power-cycle-synced serial capture gets everything. Build with -DIIGS_M0_BENCH (S3 only).
  runIIgsM0Bench();
  Serial.println("M0 DONE -- power-cycle to re-run.");
  Serial.flush();
  while (true) { delay(1000); }
#endif
  epromSetup();   // loads currentPlatform (and all saved settings) from EEPROM
  c64FreeBtMem();   // BOTH platforms: reclaim the unused BT controller DRAM (~36K) up front so
                    // tasks/buffers have heap room (Apple's render/joystick tasks were failing).

  // Platform-specific core init. The display (videoSetup -> renderLoop + boot splash)
  // and touch keyboard (oskSetup) are shared; each core initialises before videoSetup
  // so the render loop has valid state to draw (C64's render is null-guarded anyway).
  if (currentPlatform == PLATFORM_C64) {
    FSSetup();         // SD next: its DMA buffer needs the contiguous low-DRAM region before
                       // the big C64 allocations (64K RAM + framebuffer) fragment it.
    c64Setup();        // 64K RAM, ROMs, VIC/CIA, reset 6510
    videoSetup();      // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();
    sidSetup();        // 3-voice SID -> I2S DAC (GPIO26), LAST so its I2S DMA comes after SD
    c64Autostart();    // boot-autoload the saved image, if enabled
  } else if (currentPlatform == PLATFORM_NES) {
    FSSetup();         // SD first: nesSetup loads the first .nes off the card
    nesSetup();        // 2K RAM, PPU, iNES loader (mappers 0-4), framebuffer = sharedBigBuf
    videoSetup();      // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();   // analog stick + buttons -> NES controller 1
    nesApuSetup();     // APU -> I2S DAC (GPIO26), LAST so its I2S DMA comes after SD (like SID)
  } else if (currentPlatform == PLATFORM_ATARI) {
    FSSetup();         // SD first: atariSetup loads the first .a26/.bin off the card
    atariSetup();      // 128B RAM, TIA, RIOT, cartridge loader, framebuffer = sharedBigBuf
    videoSetup();      // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();   // analog stick + buttons -> 2600 joystick + console switches
    atariAudioSetup(); // TIA audio -> I2S DAC (GPIO26), LAST so its I2S DMA comes after SD
  } else {             // Apple II
    memoryAlloc();
    FSSetup();
    diskSetup();
    HDSetup();
    videoSetup();
    keyboardSetup();
    oskSetup();
    speakerSetup();
    joystickSetup();
  }
  printLog("Ready.");
}

void loop() {
  // Platform dispatch: each emulator core has its own main loop. Only the Apple II
  // core exists today; C64 / NES are selected on the boot splash (video.ino) and
  // will plug in here once implemented.
  switch (currentPlatform) {
    case PLATFORM_APPLE2: cpuLoop(); break;
    case PLATFORM_C64:    c64Loop(); break;
    case PLATFORM_NES:    nesLoop(); break;
    case PLATFORM_ATARI:  atariLoop(); break;
    default:              cpuLoop(); break;
  }
}
