#include "emu.h"
#include "src/iigs/m0_bench.h"   // IIGS feasibility gate; compiles to nothing unless -DIIGS_M0_BENCH
#ifdef IIGS_M1_TEST
void runIIgsM1Test();            // src/iigs/cpu65816_test.cpp - 65C816 core self-test
#endif

void setup() {
  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH); // Turn off green LED (boards without an LED define LED_PIN = -1)
  }
  logSetup();
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // Arduino-ESP32 core 3.x (ESP32-P4): the loop task feeds the Task WDT each iteration but isn't
  // subscribed to it, so the fast CPU loop() floods the log with "esp_task_wdt_reset: task not
  // found". Stop the loop task from feeding the WDT (it isn't monitored, so this is safe).
  disableLoopWDT();
#endif
#if defined(IIGS_M0_BENCH) || defined(IIGS_M05_BENCH) || defined(IIGS_M1_TEST)
  // Throwaway Apple IIGS bring-up harnesses (memory benchmarks M0/M0.5, CPU core test M1). Run at
  // the very top of setup() so nothing perturbs them, looping forever (re-run every few seconds)
  // so a plain serial read always catches a full run -- this board's auto-reset is flaky from a
  // host script. Build with -DIIGS_M0_BENCH / -DIIGS_M05_BENCH / -DIIGS_M1_TEST (S3 only).
  while (true) {
  #ifdef IIGS_M0_BENCH
    runIIgsM0Bench();
  #endif
  #ifdef IIGS_M05_BENCH
    runIIgsM05Bench();
  #endif
  #ifdef IIGS_M1_TEST
    runIIgsM1Test();
  #endif
    Serial.println("--- done; next run in 4s ---");
    Serial.flush();
    delay(4000);
  }
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
  } else if (currentPlatform == PLATFORM_IIGS) {
    FSSetup();          // SD FIRST: ROM 01 loads from /roms/iigs, plus the 5.25" .dsk / HD images
    iigsSetup();        // alloc banks + load ROM 01 from SD + reset 65C816
    loadDiskFilesSync();// scan the SD root so the options DISK browser is populated
    loadHdFilesSync();  // ...and the HD browser (.po/.2mg/.hdv)
    if (HdDisk) {       // auto-mount the saved block image -> firmware scan-boots slot 7
      if (selectedHdFileName.length() > 1 && selectedHdFileName != "/")
        iigsLoadHD(selectedHdFileName.c_str());
    } else if (selectedDiskFileName.length() > 1 && selectedDiskFileName != "/") {
      iigsLoadDisk(selectedDiskFileName.c_str());   // or the saved .dsk -> slot 6
    }
    videoSetup();       // TFT + render loop (+ splash); the render task draws iigsRenderText()
    oskSetup();
    joystickSetup();
    speakerSetup();     // Apple II-compatible 1-bit speaker ($C030) -> I2S amp, LAST (I2S DMA after SD)
  } else if (currentPlatform == PLATFORM_MSX) {
    FSSetup();          // SD first: msxSetup loads the BIOS / first cart off the card
    msxSetup();         // 64K RAM + 16K VRAM + BIOS (SD or C-BIOS) + reset Z80/VDP/PPI/PSG
    loadMsxFilesSync(); // scan SD root so the options ROM/disk browser is populated
    videoSetup();       // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();    // analog stick + buttons -> MSX joystick (PSG port A)
    msxPsgSetup();      // AY-3-8910 -> I2S (M3.5), LAST so its I2S DMA comes after SD
  } else if (currentPlatform == PLATFORM_SMS) {
    FSSetup();          // SD first: smsSetup loads the first .sms/.bin off the card
    smsSetup();         // 8K RAM + 16K VRAM + reset Z80/VDP/PSG; auto-load saved ROM (no BIOS)
    loadSmsFilesSync(); // scan SD root so the options ROM browser is populated
    videoSetup();       // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();    // analog stick + buttons -> SMS controller port 1
    smsPsgSetup();      // SN76489 -> I2S, LAST so its I2S DMA comes after SD
  } else if (currentPlatform == PLATFORM_PCXT) {
    FSSetup();          // SD first: pcxtSetup auto-mounts the saved A:/C: disk images
    pcxtSetup();        // 1MB RAM (PSRAM) + 64K video RAM + install BIOS ROM + wire 8086/PIC/PIT/i8042/CGA
    loadPcxtFilesSync();// scan SD root so the options disk browser is populated
    videoSetup();       // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();    // gamepad -> arrow/enter scancodes
    speakerSetup();     // PC-speaker (PIT ch2) -> I2S amp, LAST so its I2S DMA comes after SD
  } else if (currentPlatform == PLATFORM_TINY386) {
    FSSetup();          // SD first: disk images live on the card
    tiny386Setup();     // alloc guest RAM/VGA (PSRAM) + framebuffer + embedded SeaBIOS; reset the i386
    loadTiny386FilesSync();
    videoSetup();       // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();    // gamepad -> arrow/enter scancodes (M4)
    speakerSetup();     // PC-speaker -> I2S amp, LAST so its I2S DMA comes after SD
  } else {             // Apple II
    memoryAlloc();
    FSSetup();
    apple2LoadRoms();  // system ROMs from /roms/apple2 (sets apple2RomLoadFailed -> halt on failure)
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
    case PLATFORM_APPLE2: if (apple2RomLoadFailed) { delay(50); break; } cpuLoop(); break;
    case PLATFORM_C64:    c64Loop(); break;
    case PLATFORM_NES:    nesLoop(); break;
    case PLATFORM_ATARI:  atariLoop(); break;
    case PLATFORM_IIGS:   iigsLoop(); break;
    case PLATFORM_MSX:    msxLoop(); break;
    case PLATFORM_SMS:    smsLoop(); break;
    case PLATFORM_PCXT:   pcxtLoop(); break;
    case PLATFORM_TINY386: tiny386Loop(); break;
    default:              cpuLoop(); break;
  }
}
