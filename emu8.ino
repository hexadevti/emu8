#include "emu.h"

void setup() {
  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH); // Turn off green LED (boards without an LED define LED_PIN = -1)
  }
#if defined(BOARD_PICOCALC)
  // Order matters twice over. The panel, the SD slot and the keyboard are all on the PicoCalc
  // mainboard, which the Pico's own USB does NOT power, so nothing may be initialised until the
  // unit is switched on. And the loading screen goes up BEFORE logSetup(), because that waits up
  // to five seconds for a USB terminal to attach -- five seconds that used to be a black screen
  // with no way to tell a slow boot from a dead one. Serial.begin() comes first so the waits can
  // still log; logSetup() calls it again, which is harmless.
  Serial.begin(115200);
  picocalcWaitForMainboard();
  bootProgressBegin();
  bootProgressStep("Waiting for USB serial");
#endif
  logSetup();
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // Arduino-ESP32 core 3.x (ESP32-P4): the loop task feeds the Task WDT each iteration but isn't
  // subscribed to it, so the fast CPU loop() floods the log with "esp_task_wdt_reset: task not
  // found". Stop the loop task from feeding the WDT (it isn't monitored, so this is safe).
  disableLoopWDT();
#endif
  bootProgressStep("Reading settings");
  epromSetup();   // loads currentPlatform (and all saved settings) from EEPROM

#if !BOARD_HAS_PCXT_CORE
  if (currentPlatform == PLATFORM_PCXT) {
    printLog("Platform needs PSRAM and this board has none -> falling back to Apple II");
    currentPlatform = PLATFORM_APPLE2;
  }
#endif
#if !BOARD_HAS_SMS_CORE
  // Same story for the SMS: this board does not link it (board.h), but EEPROM can still name it
  // from a build that did, and loop() would dispatch into a core that is gone.
  if (currentPlatform == PLATFORM_SMS) {
    printLog("Platform has no RAM budget on this board -> falling back to Apple II");
    currentPlatform = PLATFORM_APPLE2;
  }
#endif
#if !BOARD_HAS_COLECO_CORE
  if (currentPlatform == PLATFORM_COLECO) {
    printLog("Platform not linked on this board -> falling back to Apple II");
    currentPlatform = PLATFORM_APPLE2;
  }
#endif
#if !BOARD_HAS_ZX_CORE
  if (currentPlatform == PLATFORM_ZX) {
    printLog("Platform not linked on this board -> falling back to Apple II");
    currentPlatform = PLATFORM_APPLE2;
  }
#endif
  // SD Manager: picked on the splash, which reboots into it for exactly this one boot. Checked after
  // the normalisation above because it is not a saved platform -- currentPlatform still names the
  // emulator it was entered from until here, and saveConfig() never writes this one back.
  if (sdManagerBootRequested()) currentPlatform = PLATFORM_SDMANAGER;
  c64FreeBtMem();   // BOTH platforms: reclaim the unused BT controller DRAM (~36K) up front so
                    // tasks/buffers have heap room (Apple's render/joystick tasks were failing).

  // Platform-specific core init. The display (videoSetup -> renderLoop + boot splash)
  // and touch keyboard (oskSetup) are shared; each core initialises before videoSetup
  // so the render loop has valid state to draw (C64's render is null-guarded anyway).
  if (currentPlatform == PLATFORM_SDMANAGER) {
    // No emulator: the card, the panel (a status screen, see sdManagerRender in video.cpp) and the
    // file server. The keyboard needs nothing of its own here -- on the PicoCalc it is pumped from
    // the render loop's flush, which is also where Ctrl-F8 takes it back to the system menu.
    bootProgressStep("Mounting SD card");
    FSSetup();
    bootProgressStep("Starting the SD manager");
    videoSetup();
    sdSerialSetup();
  } else if (currentPlatform == PLATFORM_C64) {
    bootProgressStep("Mounting SD card");
    FSSetup();         // SD next: its DMA buffer needs the contiguous low-DRAM region before
                       // the big C64 allocations (64K RAM + framebuffer) fragment it.
    bootProgressStep("Starting the C64");
    c64Setup();        // 64K RAM, ROMs, VIC/CIA, reset 6510
    videoSetup();      // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();
    sidSetup();        // 3-voice SID -> I2S DAC (GPIO26), LAST so its I2S DMA comes after SD
    c64Autostart();    // boot-autoload the saved image, if enabled
  } else if (currentPlatform == PLATFORM_NES) {
    bootProgressStep("Mounting SD card");
    FSSetup();         // SD first: nesSetup loads the first .nes off the card
    bootProgressStep("Loading the NES cartridge");
    nesSetup();        // 2K RAM, PPU, iNES loader (mappers 0-4), framebuffer = sharedBigBuf
    videoSetup();      // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();   // analog stick + buttons -> NES controller 1
    nesApuSetup();     // APU -> I2S DAC (GPIO26), LAST so its I2S DMA comes after SD (like SID)
  } else if (currentPlatform == PLATFORM_ATARI) {
    bootProgressStep("Mounting SD card");
    FSSetup();         // SD first: atariSetup loads the first .a26/.bin off the card
    bootProgressStep("Loading the Atari cartridge");
    atariSetup();      // 128B RAM, TIA, RIOT, cartridge loader, framebuffer = sharedBigBuf
    videoSetup();      // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();   // analog stick + buttons -> 2600 joystick + console switches
    atariAudioSetup(); // TIA audio -> I2S DAC (GPIO26), LAST so its I2S DMA comes after SD
#if BOARD_HAS_MSX_CORE
  } else if (currentPlatform == PLATFORM_MSX) {
    FSSetup();          // SD first: msxSetup loads the BIOS / first cart off the card
    msxSetup();         // 64K RAM + 16K VRAM + BIOS (SD or C-BIOS) + reset Z80/VDP/PPI/PSG
    loadMsxFilesSync(); // scan SD root so the options ROM/disk browser is populated
    videoSetup();       // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();    // analog stick + buttons -> MSX joystick (PSG port A)
    msxPsgSetup();      // AY-3-8910 -> I2S (M3.5), LAST so its I2S DMA comes after SD
#endif
#if BOARD_HAS_SMS_CORE
  } else if (currentPlatform == PLATFORM_SMS) {
    FSSetup();          // SD first: smsSetup loads the first .sms/.bin off the card
    smsSetup();         // 8K RAM + 16K VRAM + reset Z80/VDP/PSG; auto-load saved ROM (no BIOS)
    loadSmsFilesSync(); // scan SD root so the options ROM browser is populated
    videoSetup();       // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();    // analog stick + buttons -> SMS controller port 1
    smsPsgSetup();      // SN76489 -> I2S, LAST so its I2S DMA comes after SD
#endif
#if BOARD_HAS_COLECO_CORE
  } else if (currentPlatform == PLATFORM_COLECO) {
    FSSetup();          // SD first: colecoSetup loads the BIOS and the saved cartridge off the card
    colecoSetup();      // 1K RAM + 16K VRAM + BIOS; reset Z80/VDP/PSG; auto-load saved cartridge
    loadColecoFilesSync(); // scan SD root so the options cartridge browser is populated
    videoSetup();       // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();    // analog stick + buttons -> controller 1 (stick, fire buttons, keypad)
    colecoPsgSetup();   // SN76489 -> I2S, LAST so its I2S DMA comes after SD
#endif
#if BOARD_HAS_ZX_CORE
  } else if (currentPlatform == PLATFORM_ZX) {
    FSSetup();          // SD first: zxSetup loads the ROM and the saved snapshot / tape off the card
    zxSetup();          // 48K RAM (sharedBigBuf) + ROM; reset; auto-load the saved file
    loadZxFilesSync();  // scan SD root so the options file browser is populated
    videoSetup();       // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();    // analog stick + buttons -> Kempston joystick
    zxAudioSetup();     // beeper -> I2S, LAST so its I2S DMA comes after SD
#endif
#if BOARD_HAS_PCXT_CORE
  } else if (currentPlatform == PLATFORM_PCXT) {
    FSSetup();          // SD first: pcxtSetup auto-mounts the saved A:/C: disk images
    pcxtSetup();        // 1MB RAM (PSRAM; paged SRAM on the PicoCalc) + video RAM + install BIOS ROM + wire 8086/PIC/PIT/i8042/CGA
    loadPcxtFilesSync();// scan SD root so the options disk browser is populated
    videoSetup();       // TFT + render loop (+ splash)
    oskSetup();
    joystickSetup();    // gamepad -> arrow/enter scancodes
    speakerSetup();     // PC-speaker (PIT ch2) -> I2S amp, LAST so its I2S DMA comes after SD
#endif
  } else {             // Apple II
    bootProgressStep(AppleIIe ? "Building the Apple IIe memory map"
                              : "Building the Apple II+ memory map");
    memoryAlloc();
    bootProgressStep("Mounting SD card");
    FSSetup();
    apple2LoadRoms();  // system ROMs from /roms/apple2 (sets apple2RomLoadFailed -> halt on failure)
    // Either failure means the 6502 is not going to run, so skip the two SD scans and go straight
    // to the warning screen. apple2RomLoadFailed belongs in this test as much as the alloc one: a
    // ROM that would not fit leaves the heap nearly empty, and scanning the card from there ran it
    // down far enough that FreeRTOS could not allocate a task stack -- a hard hang, not a message.
    if (!apple2MemAllocFailed && !apple2RomLoadFailed) {
      bootProgressStep("Scanning disk images");
      diskSetup();
      bootProgressStep("Scanning hard disk images");
      HDSetup();
    }
    bootProgressStep("Starting the emulator");
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
    case PLATFORM_SDMANAGER: delay(50); break;   // the server is its own task; nothing to run here
#if BOARD_HAS_MSX_CORE
    case PLATFORM_MSX:    msxLoop(); break;
#endif
#if BOARD_HAS_SMS_CORE
    case PLATFORM_SMS:    smsLoop(); break;
#endif
#if BOARD_HAS_COLECO_CORE
    case PLATFORM_COLECO: colecoLoop(); break;
#endif
#if BOARD_HAS_ZX_CORE
    case PLATFORM_ZX:     zxLoop(); break;
#endif
#if BOARD_HAS_PCXT_CORE
    case PLATFORM_PCXT:   pcxtLoop(); break;
#endif
    default:              cpuLoop(); break;
  }
}
