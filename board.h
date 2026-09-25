// board.h - per-board hardware selection for the multi-platform emulator.
//
// Targets share this codebase, picked at COMPILE time by a single macro:
//   * (default)            ESP32 "Cheap Yellow Display": ILI9341 320x240 SPI via TFT_eSPI,
//                          ADC analog joystick, internal-DAC audio, XPT2046 touch on the
//                          display SPI bus.
//   * BOARD_JC4827W543     Guition JC4827W543: ESP32-S3, NV3041A 480x272 QSPI via Arduino_GFX,
//                          USB SNES gamepad, I2S external-amp audio, XPT2046 touch on its own SPI.
//   * BOARD_JC1060P470     Guition JC1060P470: ESP32-P4, JD9165 1024x600 MIPI-DSI panel (esp_lcd,
//                          wrapped by the same DisplayGFX canvas), GT911 capacitive touch (I2C),
//                          ES8311 codec + NS4150B amp (I2S), microSD over SD_MMC, USB-HS host.
//                          Built against Arduino-ESP32 core 3.x (IDF 5.x) — see sketch.yaml /
//                          the P4 build task — isolated from the 2.0.17 CYD/S3 toolchain.
//   * BOARD_PICOCALC       ClockworkPi PicoCalc + Raspberry Pi Pico 2 (RP2350): ST7365P/ILI9488
//                          320x320 SPI panel (spi1), STM32 QWERTY keyboard over I2C, microSD on
//                          spi0, PWM stereo audio. Built with the earlephilhower arduino-pico
//                          core (NOT Arduino-ESP32) -- see the PicoCalc build task. The ESP-only
//                          APIs the shared code uses are shimmed in src/picocalc/pico_shim/.
//   * BOARD_DESKTOP        Windows/Linux SDL2 debug target (set by CMake, never by arduino-cli).
//
// The non-default boards are selected by defining their macro from the build task (e.g.
// -DBOARD_JC1060P470 via compiler.cpp.extra_flags). When none is set we fall back to the CYD.
//
// This header defines (a) capability macros the rest of the code switches on and (b) the
// GPIO pin map. Display pins for the CYD live in User_Setup.h (TFT_eSPI); display pins for
// the Arduino_GFX / DSI boards live here because the panel is configured in code (display_gfx.cpp).

#pragma once

#if defined(BOARD_DESKTOP)

// ===================== Desktop (Windows/Linux, SDL2) — DEBUG TARGET =====================
// "Mais um board": compila os MESMOS cores/lógica que os devices; só as folhas de hardware
// (display/áudio/input/SD/EEPROM) trocam, atrás de #if defined(BOARD_DESKTOP). Backends em
// src/desktop/ (SDL2), shim do Arduino/ESP/FreeRTOS em src/desktop/arduino_shim/. Build via
// CMake (32-bit, -m32, ILP32 como o ESP32). O arduino-cli NUNCA define BOARD_DESKTOP, então os
// binários dos devices ficam intactos.
#define BOARD_NAME            "Desktop (SDL2)"

// --- capabilities (espelham o caminho I2S-amp / USB-input do S3, mas tudo em software) ---
#define BOARD_HAS_TFT_ESPI    0   // display: backend próprio (DisplayGFX em src/desktop/display_sdl)
#define BOARD_DISPLAY_GFX     0   // NÃO compila o Arduino_GFX backend (display_gfx.cpp fica fora)
#define BOARD_AUDIO_DAC       0   // sem DAC interno; áudio via ampBegin/ampWrite* (audio_sdl.cpp)
#define BOARD_INPUT_ANALOG    0   // sem ADC; input por teclado/gamepad SDL
#define BOARD_INPUT_USB       1   // reaproveita usbkeyboard.cpp (SDL keysym -> HID -> dispatch)
#define BOARD_TOUCH_VIA_TFT   0   // toque = mouse SDL (display backend implementa getTouchRaw)
#define BOARD_PANEL_DSI       0   // not a MIPI-DSI panel
#define BOARD_TOUCH_GT911     0   // not a GT911 capacitive touch
#define BOARD_AUDIO_CODEC     0   // not an I2C audio codec (ES8311)
#define BOARD_SD_MMC          0   // SD is emulated on the host FS, not SD_MMC

// --- pinos: todos dummies (-1); nenhum GPIO real existe no PC. Os caminhos que tocam GPIO
//     já guardam em capability macros e/ou checagem (pin < 0). ---
#define SD_SCK_PIN           -1
#define SD_MISO_PIN          -1
#define SD_MOSI_PIN          -1
#define SD_CS_PIN            -1
#define TOUCH_SCK_PIN        -1
#define TOUCH_MISO_PIN       -1
#define TOUCH_MOSI_PIN       -1
#define TOUCH_CS_PIN         -1
#define TOUCH_INT_PIN        -1
#define I2S_BCLK_PIN         -1
#define I2S_LRCLK_PIN        -1
#define I2S_DOUT_PIN         -1
#define LED_PIN              -1
#define SPEAKER_PIN          -1
#define KEYBOARD_DATA_PIN    -1
#define KEYBOARD_IRQ_PIN     -1
#define ANALOG_X_PIN         -1
#define ANALOG_Y_PIN         -1
#define DIGITAL_BUTTON12_PIN -1

#elif defined(BOARD_PICOCALC)

// ===================== ClockworkPi PicoCalc (Raspberry Pi Pico 2 / RP2350) =====================
// Handheld: 320x320 ST7365P (ILI9488-compatible) SPI panel, STM32F103 QWERTY keyboard over I2C,
// microSD on its own SPI bus, PWM stereo audio, 18650 battery. The mainboard carries 8MB of PSRAM
// but on PLAIN GPIOs (bit-bang/PIO SPI), NOT the RP2350 memory-mapped QSPI PSRAM bus -- it cannot
// back a raw pointer, so BOARD_HAS_PSRAM is 0 and the cores that need MBs of guest RAM (IIGS,
// PC-XT, tiny386) are compiled out. Everything else fits the 520KB SRAM.
//
// Toolchain: earlephilhower arduino-pico, FQBN rp2040:rp2040:rpipico2 with arch=arm + os=freertos
// (FreeRTOS SMP is ARM-only) + freq=200 (see the SPI note below). Pin numbers below are taken from
// clockworkpi/PicoCalc's own sources (Code/picocalc_helloworld/), not from third-party writeups.
#define BOARD_NAME            "ClockworkPi PicoCalc (RP2350)"

// --- capabilities (1 = present / use this path) ---
#define BOARD_HAS_TFT_ESPI    0   // display: own DisplayGFX backend (src/picocalc/display_picocalc)
#define BOARD_DISPLAY_GFX     0   // NOT the Arduino_GFX canvas backend (display_gfx.cpp stays out)
#define BOARD_PANEL_DSI       0   // 4-wire SPI panel, not MIPI-DSI
#define BOARD_AUDIO_DAC       0   // no internal DAC; audio is PWM (see BOARD_AUDIO_PWM)
#define BOARD_AUDIO_CODEC     0   // no I2C codec
#define BOARD_AUDIO_PWM       1   // NEW: PWMAudio (arduino-pico) drives the two speaker pins
#define BOARD_INPUT_ANALOG    0   // GPIO26/27 (= A0/A1) are the audio pins -> no ADC joystick
#define BOARD_INPUT_USB       1   // reuse usbkeyboard.cpp's HID dispatcher; input_picocalc.cpp
                                  // translates the I2C keyboard into HID boot reports and feeds it
#define BOARD_TOUCH_VIA_TFT   0   // no touchscreen at all
#define BOARD_TOUCH_GT911     0
#define BOARD_SD_MMC          0   // microSD over SPI (spi0), not SDIO
#define BOARD_HAS_BLE         0   // plain Pico 2 has no radio
#define BOARD_HAS_PSRAM       0   // see the header comment: PSRAM is not memory-mapped here

// --- display: ST7365P/ILI9488 320x320 on spi1. CS/DC/RST are driven as plain GPIOs (the PL022
//     hardware SSn toggles per frame, which breaks multi-byte writes -- see display_picocalc.cpp).
//     arduino-pico's rpipico2 variant defaults spi1 to different pins, so these are applied with
//     SPI1.setSCK/setTX/setRX before begin(). ---
#define LCD_SCK_PIN       10
#define LCD_MOSI_PIN      11
#define LCD_MISO_PIN      12
#define LCD_CS_PIN        13
#define LCD_DC_PIN        14
#define LCD_RST_PIN       15
#define PANEL_NATIVE_W    320
#define PANEL_NATIVE_H    320
// SPI clock. The PL022 baud is clk_peri / (even_prescale * (1+SCR)) and rounds DOWN, so the
// reachable rates are quantised and asking for 50MHz at the wrong sysclk silently gives less:
//   RP2350 at the stock 150MHz -> only 75 / 37.5 / 25 are reachable, so 50 becomes 37.5
//   RP2040 at the stock 133MHz -> 50 becomes 133/4 = 33.25
// BOTH build tasks therefore select freq=200, where clk_peri/4 is exactly 50MHz (the rate proven
// on this panel) -- see .vscode/tasks.json. If the ribbon shows artifacts, halve THIS constant
// rather than lowering the sysclk; the 6502 interpreter is CPU-bound and wants the clock.
#define LCD_SPI_HZ        50000000

// --- keyboard: STM32F103 on i2c1, 10kHz (its firmware is slow; faster clocks drop bytes) ---
#define KBD_I2C_SDA_PIN   6
#define KBD_I2C_SCL_PIN   7
#define KBD_I2C_ADDR      0x1F
#define KBD_I2C_HZ        10000
// How long setup() waits for the PicoCalc mainboard rail before giving up and booting anyway.
// The Pico runs off its own USB, so it boots whether the unit is switched on or not.
#define PICOCALC_MAINBOARD_WAIT_MS 30000

// --- microSD (SPI). These match arduino-pico's DEFAULT spi0 pin assignment for rpipico2 exactly,
//     so no setRX/setTX/setSCK remap is needed for the card. Separate bus from the panel, so the
//     touch-vs-SD arbitration the ESP32 boards need (busTake/busGive) is a no-op here. ---
#define SD_SCK_PIN        18
#define SD_MISO_PIN       16
#define SD_MOSI_PIN       19
#define SD_CS_PIN         17

// --- audio: two PWM speaker pins on the SAME slice (GPIO26 = PWM5A, GPIO27 = PWM5B), which is
//     what PWMAudio's stereo mode expects (it muxes pin and pin+1). The emulated machines are all
//     mono, so audio_picocalc.cpp writes each sample to both channels. ---
#define AUDIO_PWM_L_PIN   26
#define AUDIO_PWM_R_PIN   27

// --- PSRAM (present but NOT memory-mapped; documented for a possible future PIO driver, unused) ---
#define PSRAM_CS_PIN      20
#define PSRAM_SCK_PIN     21
#define PSRAM_MOSI_PIN    2
#define PSRAM_MISO_PIN    3

// --- peripherals not present on this board: harmless placeholders so shared code that references
//     them still compiles (paths guard on capability macros and/or a < 0 pin check). ---
#define TOUCH_SCK_PIN        -1
#define TOUCH_MISO_PIN       -1
#define TOUCH_MOSI_PIN       -1
#define TOUCH_CS_PIN         -1
#define TOUCH_INT_PIN        -1
#define I2S_BCLK_PIN         -1
#define I2S_LRCLK_PIN        -1
#define I2S_DOUT_PIN         -1
#define LED_PIN              -1   // the Pico's LED is on the module, not visible in the case
#define SPEAKER_PIN          -1   // no DAC pin; audio goes through AUDIO_PWM_*
#define KEYBOARD_DATA_PIN    -1   // no PS/2 header (the I2C keyboard replaces it)
#define KEYBOARD_IRQ_PIN     -1
#define ANALOG_X_PIN         -1
#define ANALOG_Y_PIN         -1
#define DIGITAL_BUTTON12_PIN -1

#elif defined(BOARD_JC4827W543)

// ===================== Guition JC4827W543 (ESP32-S3) =====================
#define BOARD_NAME            "JC4827W543 (ESP32-S3)"

// --- capabilities (1 = present / use this path) ---
#define BOARD_HAS_TFT_ESPI    0   // display: 0 = Arduino_GFX (DisplayGFX), 1 = TFT_eSPI
#define BOARD_DISPLAY_GFX     1   // compile the Arduino_GFX DisplayGFX backend (display_gfx.cpp)
#define BOARD_AUDIO_DAC       0   // 0 = no internal DAC (S3); audio goes to an I2S amp (M5)
#define BOARD_INPUT_ANALOG    0   // 0 = no ADC joystick; input is the USB SNES gamepad (M3)
#define BOARD_INPUT_USB       1   // USB-HID host gamepad
#define BOARD_TOUCH_VIA_TFT   0   // 0 = XPT2046 on a dedicated SPI bus (M4), not the display bus
#define BOARD_PANEL_DSI       0   // panel is QSPI (NV3041A), not MIPI-DSI
#define BOARD_TOUCH_GT911     0   // touch is XPT2046 (resistive), not GT911
#define BOARD_AUDIO_CODEC     0   // dumb I2S amp (NS4168), no I2C codec
#define BOARD_SD_MMC          0   // microSD over SPI, not SD_MMC

// --- display: NV3041A 480x272 QSPI (Arduino_ESP32QSPI + Arduino_NV3041A) ---
#define GFX_QSPI_CS_PIN   45
#define GFX_QSPI_SCK_PIN  47
#define GFX_QSPI_D0_PIN   21
#define GFX_QSPI_D1_PIN   48
#define GFX_QSPI_D2_PIN   40
#define GFX_QSPI_D3_PIN   39
#define GFX_RST_PIN       -1      // no dedicated reset GPIO (panel reset is internal)
#define GFX_BL_PIN        1       // backlight, active HIGH, PWM-capable
#define PANEL_NATIVE_W    480
#define PANEL_NATIVE_H    272

// --- touch (XPT2046) + microSD SHARE one SPI bus (HSPI/SPI3): SCK=12, MISO=13, MOSI=11.
//     Each has its own CS: touch=38, SD=10. The display is on a separate QSPI bus (SPI2), so
//     it does not contend. (Verified: SCK=12/MISO=13/MOSI=11, SD_CS=10 mounts the card;
//     the manufacturer IO-table pins 7/8/9/10 did NOT work.) ---
#define TOUCH_SCK_PIN     12
#define TOUCH_MISO_PIN    13
#define TOUCH_MOSI_PIN    11
#define TOUCH_CS_PIN      38
#define TOUCH_INT_PIN     3

// --- microSD (SPI, shared bus with the touch controller above) ---
#define SD_SCK_PIN        12
#define SD_MISO_PIN       13
#define SD_MOSI_PIN       11
#define SD_CS_PIN         10

// --- audio: onboard NS4168 mono I2S Class-D amp (SPECK_* nets). No enable/shutdown GPIO; the
//     amp plays whenever it has a valid I2S clock. Standard 16-bit stereo I2S (amp picks 1 ch). ---
#define I2S_BCLK_PIN      42
#define I2S_LRCLK_PIN     2
#define I2S_DOUT_PIN      41

// --- peripherals not present on this board: define harmless placeholders so shared code
//     that references them still compiles. Code paths guard on the capability macros and/or
//     a < 0 pin check before touching these. ---
#define LED_PIN              -1
#define SPEAKER_PIN          -1   // no internal DAC pin; M5 routes audio through I2S
#define KEYBOARD_DATA_PIN    -1   // no PS/2 keyboard on this board (use USB + touch OSK)
#define KEYBOARD_IRQ_PIN     -1
#define ANALOG_X_PIN         -1
#define ANALOG_Y_PIN         -1
#define DIGITAL_BUTTON12_PIN -1

#elif defined(BOARD_JC1060P470)

// ===================== Guition JC1060P470 (ESP32-P4) =====================
// 7" 1024x600 IPS, JD9165 MIPI-DSI panel; GT911 capacitive touch (I2C); ES8311 codec + NS4150B
// amp (I2S); microSD over SD_MMC; native USB-HS host. 32MB PSRAM / 16MB flash. Built on Arduino
// core 3.x (IDF 5.x). Pins taken from the cheops JC1060P470C reference repo (pins_config.h) and
// the board's ESPHome config; items marked CONFIRM should be checked against 5-Schematic.
#define BOARD_NAME            "JC1060P470 (ESP32-P4)"

// --- capabilities (1 = present / use this path) ---
#define BOARD_HAS_TFT_ESPI    0   // display: Arduino_Canvas (DisplayGFX) flushed to a DSI panel
#define BOARD_DISPLAY_GFX     1   // reuse the DisplayGFX canvas/UI/fill-screen backend (display_gfx.cpp)
#define BOARD_PANEL_DSI       1   // NEW: output is a JD9165 MIPI-DSI panel via esp_lcd (not QSPI)
#define BOARD_AUDIO_DAC       0   // no internal DAC on the P4
#define BOARD_AUDIO_CODEC     1   // NEW: ES8311 I2C codec drives the NS4150B amp (not a dumb amp)
#define BOARD_INPUT_ANALOG    0   // no ADC joystick
#define BOARD_INPUT_USB       1   // USB-HID host on the native USB-HS OTG. Uses the EspUsbHost fork
                                  // patched for IDF 5.x (its rom/usb/usb_common.h include -> usb/usb_types_ch9.h;
                                  // see src/shared/p4/README.md). GT911 touch + OSK remain the primary input.
#define BOARD_TOUCH_VIA_TFT   0
#define BOARD_TOUCH_GT911     1   // NEW: GT911 capacitive touch over I2C
#define BOARD_SD_MMC          1   // NEW: microSD over the SDMMC (SDIO) peripheral, not SPI
#define BOARD_HAS_BLE         0   // the ESP32-P4 has NO radio (BLE/WiFi live on the companion C6),
                                  // so the BLE library is absent — don't include it (it's unused anyway)

// --- display: JD9165 1024x600 MIPI-DSI (2 data lanes). The DSI bus/timings live in the JD9165
//     esp_lcd vendor driver (src/shared/p4/, see display_gfx.cpp); only the control GPIOs are here. ---
#define GFX_RST_PIN       27      // panel reset
#define GFX_BL_PIN        23      // backlight (active HIGH, PWM-capable)
#define PANEL_NATIVE_W    1024
#define PANEL_NATIVE_H    600

// --- shared I2C bus (GT911 touch + ES8311 codec live on the same SDA/SCL) ---
#define I2C_SDA_PIN       7
#define I2C_SCL_PIN       8

// --- touch: GT911 (I2C addr 0x5D by default; some units strap to 0x14). INT/RST used for the
//     power-on address-select pulse and to clear the IRQ latch. ---
#define GT911_ADDR        0x5D
#define TOUCH_INT_PIN     21
#define TOUCH_RST_PIN     22

// --- audio: ES8311 codec (I2C addr 0x18) -> NS4150B mono amp. Standard Philips I2S with MCLK.
//     SPK_EN gates the amp (drive HIGH to un-mute). ---
#define ES8311_ADDR       0x18
#define I2S_MCLK_PIN      13
#define I2S_BCLK_PIN      12
#define I2S_LRCLK_PIN     10
#define I2S_DOUT_PIN      9
#define I2S_DIN_PIN       11      // codec ADC (mic) — unused by the emulator, kept for completeness
#define AUDIO_SPK_EN_PIN  20

// --- microSD over SD_MMC (SDIO). CONFIRM pins + bus width against 5-Schematic; we mount 1-bit
//     (uses CLK/CMD/D0 only) for maximum compatibility. ---
#define SDMMC_CLK_PIN     43
#define SDMMC_CMD_PIN     44
#define SDMMC_D0_PIN      39
#define SDMMC_D1_PIN      40
#define SDMMC_D2_PIN      41
#define SDMMC_D3_PIN      42

// --- peripherals not present / not used on this board: harmless placeholders so shared code that
//     references them still compiles (paths guard on capability macros and/or a < 0 pin check). ---
#define SD_SCK_PIN           -1   // SD is SD_MMC here, not SPI
#define SD_MISO_PIN          -1
#define SD_MOSI_PIN          -1
#define SD_CS_PIN            -1
#define TOUCH_SCK_PIN        -1   // touch is I2C (GT911), not SPI
#define TOUCH_MISO_PIN       -1
#define TOUCH_MOSI_PIN       -1
#define TOUCH_CS_PIN         -1
#define LED_PIN              -1
#define SPEAKER_PIN          -1
#define KEYBOARD_DATA_PIN    -1
#define KEYBOARD_IRQ_PIN     -1
#define ANALOG_X_PIN         -1
#define ANALOG_Y_PIN         -1
#define DIGITAL_BUTTON12_PIN -1

#else

// ===================== ESP32 "Cheap Yellow Display" (default) =====================
#define BOARD_NAME            "ESP32 CYD"

// --- capabilities ---
#define BOARD_HAS_TFT_ESPI    1   // display via TFT_eSPI (User_Setup.h)
#define BOARD_DISPLAY_GFX     0
#define BOARD_AUDIO_DAC       1   // internal DAC on GPIO26 (DAC channel 2)
#define BOARD_INPUT_ANALOG    1   // ADC analog joystick + resistor-ladder buttons
#define BOARD_INPUT_USB       0
#define BOARD_TOUCH_VIA_TFT   1   // XPT2046 read through TFT_eSPI on the display SPI bus
#define BOARD_PANEL_DSI       0   // ILI9341 SPI panel, not MIPI-DSI
#define BOARD_TOUCH_GT911     0   // touch is XPT2046 (resistive), not GT911
#define BOARD_AUDIO_CODEC     0   // internal DAC, no I2C codec
#define BOARD_SD_MMC          0   // microSD over SPI, not SD_MMC

// --- board pins (display pins are in User_Setup.h for the TFT_eSPI build) ---
#define SD_SCK_PIN           18
#define SD_MISO_PIN          19
#define SD_MOSI_PIN          23
#define SD_CS_PIN            5
#define KEYBOARD_DATA_PIN    21
#define KEYBOARD_IRQ_PIN     22
#define ANALOG_X_PIN         4
#define ANALOG_Y_PIN         35
#define LED_PIN              17
#define DIGITAL_BUTTON12_PIN 34   // joystick buttons 0-3 (resistor ladder)
#define SPEAKER_PIN          26   // GPIO26 = DAC channel 2

#endif

// Fill-screen video toggle (SCREEN: FILL / ORIG) in Settings. The S3 panel (480x272), the P4 DSI
// panel (1024x600) and the desktop window can scale the 320x240 video to fill (on the big P4 panel
// fill is effectively mandatory — centered 320x240 would be tiny); the CYD is already 320x240.
// The PicoCalc (320x320) uses it for MSX/SMS only: FILL scales their 256x192 to 320x240 (4:3),
// which costs ~57% more SPI time per frame (~40fps shown instead of ~60; emulation speed unchanged).
#if defined(BOARD_DESKTOP) || defined(BOARD_PICOCALC)
  #define BOARD_HAS_SCREENFILL 1
#else
  #define BOARD_HAS_SCREENFILL BOARD_DISPLAY_GFX
#endif

// BLE is declared-but-unused in emu.h. ESP32 / ESP32-S3 ship the BLE library and the desktop has a
// shim; only the radio-less ESP32-P4 lacks it, so it sets BOARD_HAS_BLE 0 in its section above.
#ifndef BOARD_HAS_BLE
#define BOARD_HAS_BLE 1
#endif

// Capabilities introduced by a later board, defaulted here so the older branches above (which
// predate them) don't each need a line. Only the PicoCalc sets BOARD_AUDIO_PWM; only the PicoCalc
// clears BOARD_HAS_PSRAM -- every ESP32 target here has real, memory-mapped PSRAM behind ps_malloc.
#ifndef BOARD_AUDIO_PWM
#define BOARD_AUDIO_PWM 0
#endif
#ifndef BOARD_HAS_PSRAM
#define BOARD_HAS_PSRAM 1
#endif

// Keep the 16696-byte enhanced IIe ROM in flash (src/apple2/iie_rom_flash.cpp) instead of reading
// it off the SD card into the heap. Only the PicoCalc needs this, and only because the IIe memory
// map leaves it 25404 bytes to work with: paying 16696 of them for a ROM that is never written to
// is what kept the IIe from booting there. The ESP32 boards have the heap for it and load the same
// image from /roms/apple2/iie.bin as before, so their flash stays as it is.
#ifndef BOARD_A2_ROM_IN_FLASH
#if defined(BOARD_PICOCALC)
#define BOARD_A2_ROM_IN_FLASH 1
#else
#define BOARD_A2_ROM_IN_FLASH 0
#endif
#endif

// Cores that need megabytes of contiguous, pointer-addressable guest RAM (Apple IIGS banks, the
// PC-XT's 1MB, tiny386's guest+VGA memory) only exist on boards with real PSRAM. On the others
// they are compiled out of the platform switch entirely -- see emu8.ino and the boot splash.
#define BOARD_HAS_BIGRAM_CORES BOARD_HAS_PSRAM

// The two Z80 cores (MSX, SMS) keep separate gates, but both are on for every board today. Neither
// carries meaningful statics: RAM, VRAM and (on the SMS) the 32KB of on-cart battery RAM are heap,
// allocated only when that core is the booted system -- the SMS cart RAM only once a game actually
// maps it -- and the framebuffer is sharedBigBuf. So linking them costs the Apple II nothing on the
// PicoCalc's ORIGINAL RP2040 mainboard, whose 264KB leaves ~102KB of heap once statics are placed
// while the Apple II's memoryAlloc() needs ~101KB. (The SMS used to hold cartRam as a 32KB static,
// which is what kept it off that board.) What makes both fit the RP2040's heap once booted is
// keeping the BIOS and cartridge images in flash instead -- see BOARD_ROM_IN_FLASH below.
#define BOARD_HAS_MSX_CORE 1
#define BOARD_HAS_SMS_CORE 1

// MSX BIOS + MSX/SMS cartridge images live in spare on-board flash (src/picocalc/romflash_picocalc.cpp)
// instead of ps_malloc'd RAM. Only the PicoCalc: it has no PSRAM, and its flash is XIP-mapped, so
// a const pointer into it reads like ROM. On the RP2040 this is what makes the Z80 cores fit at all
// (the heap cannot hold a 32K BIOS next to 64K RAM + 16K VRAM, nor a 256K SMS game); on the RP2350
// it lets images of any size up to 1MB load without eating the heap.
#if defined(BOARD_PICOCALC)
#define BOARD_ROM_IN_FLASH 1
#else
#define BOARD_ROM_IN_FLASH 0
#endif

// (The Apple II used to need a third gate here -- BOARD_APPLE2_LEAN -- because memoryAlloc()
// built the II+ and the IIe map at the same time so a settings toggle could switch between them
// live, and both together do not fit in 264KB. The splash now offers II+ and IIe as two separate
// systems and exactly one map is ever allocated, so the gate is gone: see src/apple2/memory.cpp.)

// The PicoCalc is built with the earlephilhower arduino-pico core instead of Arduino-ESP32, so the
// ESP-IDF APIs the shared sources call (IRAM_ATTR, heap_caps_*, esp_reset_reason, the core-pinned
// task helpers, ...) do not exist. Pulling the shim in HERE gives every translation unit the
// replacements automatically -- board.h is the one header the whole tree already includes.
#if defined(BOARD_PICOCALC)
#include "src/picocalc/pico_shim/pico_shim.h"
#endif

// NOTE on relocating code into SRAM (IRAM_ATTR) on PicoCalc -- there is a hard budget.
//
// The RP2040 executes from external flash through a 16 KB XIP cache shared by BOTH cores, so the
// 6502 interpreter and the raster loop evict each other continuously. Moving cpuLoop and the
// three opcode tables into SRAM is worth ~19% on 6502 throughput and is done (see cpu.cpp).
//
// Going further is NOT free: every byte relocated comes straight out of the FreeRTOS heap.
// Adding renderLoop (4,356 B) and processSoftSwitches (1,136 B) dropped free RAM from 132,808 to
// 126,556 bytes and the CORE0 task then died at startup with
//     FATAL: FreeRTOS pvPortMalloc failed (out of heap) in task 'CORE0'
// which presents as a black screen, because the emulator never starts. Measure the heap before
// relocating anything else.
//
// That FATAL is worth understanding, because it is the ONLY way this board dies of a full heap.
// A plain malloc() that comes back NULL is handled everywhere in this tree (the VIC falls back to
// text mode, the Apple II says so on the panel). But arduino-pico builds FreeRTOS with
// configUSE_MALLOC_FAILED_HOOK, so an allocation made by the KERNEL -- a task stack, a queue, a
// semaphore -- has no soft path: pvPortMalloc() calls the hook, which parks the board
// (src/picocalc/fault_picocalc.cpp, where the message now also carries the free/used heap).
// The C64 boot used to end with exactly such an allocation, the 4KB sound-core task stack, asked
// for AFTER the guest's 64K of RAM and its ROMs were already on the heap. Both it and the render
// task now use static stacks (picocalcStartAudioTask in src/picocalc/audio_picocalc.cpp, and
// renderTaskStack in src/shared/video.cpp), so nothing in a normal boot can reach the hook.
