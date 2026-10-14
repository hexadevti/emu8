// jd9165_dsi.h - MIPI-DSI (JD9165) panel output for the JC1060P470 (ESP32-P4), via esp_lcd.
//
// The emulator keeps drawing into the Arduino_Canvas PSRAM framebuffer that DisplayGFX owns
// (all the UI / video / fill-screen logic in display_gfx.cpp is unchanged). The ONLY board
// difference on the P4 is the panel OUTPUT: instead of pushing the canvas over QSPI to an
// NV3041A, we push it to a JD9165 1024x600 MIPI-DSI panel through ESP-IDF's esp_lcd driver.
// These three functions are that output; display_gfx.cpp calls them from begin()/flush().
//
// Requires the JD9165 esp_lcd vendor driver (esp_lcd_jd9165.c/.h) to be present in this folder
// (src/shared/p4/) — copy it from the board reference repo:
//   https://github.com/cheops/JC1060P470C_I_W  (1-Demo/Demo_Arduino/.../src/lcd/esp_lcd_jd9165.*)
// It carries the panel's manufacturer init sequence + the DSI/DPI timing config macros and is
// MIT/Apache-licensed (ESP IoT Solution). We only wrap it here.
#pragma once

#include "../../../board.h"

#if BOARD_PANEL_DSI

#include <stdint.h>

// Power up the DSI PHY, create the JD9165 panel (reset + init), and turn the backlight on.
// Returns false if any esp_lcd step fails (DisplayGFX logs and leaves the screen dark).
bool dsiPanelBegin();

// Copy a w x h RGB565 block to the panel at (x,y). The DPI panel owns its framebuffer in PSRAM;
// this is esp_lcd_panel_draw_bitmap (a CPU/2D-DMA copy). DisplayGFX flushes the whole 1024x600
// frame each rendered frame.
void dsiPanelDrawBitmap(int x, int y, int w, int h, const uint16_t *rgb565);

// Fill the whole panel with one RGB565 color (used to clear the static border in the NES
// direct-to-panel path). Slow row-by-row draw — only used on rare mode changes, not per frame.
void dsiPanelFillScreen(uint16_t color);

#endif // BOARD_PANEL_DSI
