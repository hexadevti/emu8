// board.h - per-board hardware selection for the multi-platform emulator.
//
// Two targets share this codebase, picked at COMPILE time by a single macro:
//   * (default)            ESP32 "Cheap Yellow Display": ILI9341 320x240 SPI via TFT_eSPI,
//                          ADC analog joystick, internal-DAC audio, XPT2046 touch on the
//                          display SPI bus.
//   * BOARD_JC4827W543     Guition JC4827W543: ESP32-S3, NV3041A 480x272 QSPI via Arduino_GFX,
//                          USB SNES gamepad, I2S external-amp audio, XPT2046 touch on its own SPI.
//
// The JC4827W543 build is selected by defining BOARD_JC4827W543 from the build task
// (-DBOARD_JC4827W543). When it is absent we fall back to the original CYD board, so the
// existing tasks keep producing an unchanged CYD firmware.
//
// This header defines (a) capability macros the rest of the code switches on and (b) the
// GPIO pin map. Display pins for the CYD live in User_Setup.h (TFT_eSPI); display pins for
// the JC4827W543 live here because Arduino_GFX is configured in code (see display_gfx.cpp).

#pragma once

#if defined(BOARD_JC4827W543)

// ===================== Guition JC4827W543 (ESP32-S3) =====================
#define BOARD_NAME            "JC4827W543 (ESP32-S3)"

// --- capabilities (1 = present / use this path) ---
#define BOARD_HAS_TFT_ESPI    0   // display: 0 = Arduino_GFX (DisplayGFX), 1 = TFT_eSPI
#define BOARD_DISPLAY_GFX     1   // compile the Arduino_GFX DisplayGFX backend (display_gfx.cpp)
#define BOARD_AUDIO_DAC       0   // 0 = no internal DAC (S3); audio goes to an I2S amp (M5)
#define BOARD_INPUT_ANALOG    0   // 0 = no ADC joystick; input is the USB SNES gamepad (M3)
#define BOARD_INPUT_USB       1   // USB-HID host gamepad
#define BOARD_TOUCH_VIA_TFT   0   // 0 = XPT2046 on a dedicated SPI bus (M4), not the display bus

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
