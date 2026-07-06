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
#if defined(BOARD_DESKTOP)
  #define BOARD_HAS_SCREENFILL 1
#else
  #define BOARD_HAS_SCREENFILL BOARD_DISPLAY_GFX
#endif

// BLE is declared-but-unused in emu.h. ESP32 / ESP32-S3 ship the BLE library and the desktop has a
// shim; only the radio-less ESP32-P4 lacks it, so it sets BOARD_HAS_BLE 0 in its section above.
#ifndef BOARD_HAS_BLE
#define BOARD_HAS_BLE 1
#endif
