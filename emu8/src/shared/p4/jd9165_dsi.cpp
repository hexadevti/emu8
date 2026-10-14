// jd9165_dsi.cpp - esp_lcd MIPI-DSI output for the JC1060P470 (ESP32-P4 / JD9165 1024x600).
// See jd9165_dsi.h. Compiled only on the P4 board (BOARD_PANEL_DSI). Built against Arduino-ESP32
// core 3.x (ESP-IDF 5.x), which bundles the esp_lcd MIPI-DSI driver used below.

#include "jd9165_dsi.h"

#if BOARD_PANEL_DSI

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_jd9165.h"   // JD9165 vendor driver — copy into this folder from the cheops repo

#include "../../../board.h"

// NOTE: the esp_lcd setup below is modeled on the ESP IoT Solution JD9165 example (the same code
// the board's own Arduino demo uses, src/lcd/jd9165_lcd.cpp). The DSI lane count, lane bit-rate and
// all panel timings (pixel clock, porches, pulse widths) live inside the vendor macros
// JD9165_PANEL_BUS_DSI_2CH_CONFIG / JD9165_PANEL_IO_DBI_CONFIG / JD9165_1024_600_PANEL_60HZ_DPI_CONFIG
// from esp_lcd_jd9165.h, so we don't repeat them here. If a struct field name below does not match
// the version of esp_lcd_jd9165.h you copy in, reconcile it against that header / jd9165_lcd.cpp.

static esp_lcd_panel_handle_t s_panel = nullptr;

// The DPI panel's draw_bitmap is ASYNC (the copy into the scanned-out framebuffer runs in the
// background). Pushing the next frame before the previous transfer finished floods the log with
// "previous draw operation is not finished" and drops frames. This binary semaphore serializes it:
// the on_color_trans_done callback gives it when a transfer completes; dsiPanelDrawBitmap waits for
// it before issuing the next draw. This also paces the render loop (like the S3's blocking QSPI flush).
static SemaphoreHandle_t s_drawDone = nullptr;

static bool IRAM_ATTR dsiOnTransDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *) {
  BaseType_t hpw = pdFALSE;
  if (s_drawDone) xSemaphoreGiveFromISR(s_drawDone, &hpw);
  return hpw == pdTRUE;
}

bool dsiPanelBegin() {
  // 1) The MIPI DSI D-PHY on the ESP32-P4 is powered by the internal LDO (VO3), 2.5V on chan 3.
  static esp_ldo_channel_handle_t s_ldo = nullptr;
  esp_ldo_channel_config_t ldo_cfg = {};
  ldo_cfg.chan_id = 3;
  ldo_cfg.voltage_mv = 2500;
  if (esp_ldo_acquire_channel(&ldo_cfg, &s_ldo) != ESP_OK) {
    Serial.println("JD9165: MIPI DPHY LDO acquire FAILED");
    return false;
  }

  // 2) DSI bus (2 data lanes).
  esp_lcd_dsi_bus_handle_t mipi_dsi_bus = nullptr;
  esp_lcd_dsi_bus_config_t bus_config = JD9165_PANEL_BUS_DSI_2CH_CONFIG();
  if (esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus) != ESP_OK) {
    Serial.println("JD9165: new_dsi_bus FAILED");
    return false;
  }

  // 3) DBI control IO (panel command/parameter channel over DSI).
  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_dbi_io_config_t dbi_config = JD9165_PANEL_IO_DBI_CONFIG();
  if (esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io) != ESP_OK) {
    Serial.println("JD9165: new_panel_io_dbi FAILED");
    return false;
  }

  // 4) DPI (video) timing + the JD9165 vendor panel (carries the manufacturer init sequence).
  //    The lane count (num_data_lanes=2) and lane bit rate live in the bus config macro above;
  //    init_cmds is left NULL/0 so the vendor driver uses its built-in JD9165 init sequence.
  esp_lcd_dpi_panel_config_t dpi_config = JD9165_1024_600_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
  jd9165_vendor_config_t vendor_config = {};
  vendor_config.mipi_config.dsi_bus    = mipi_dsi_bus;
  vendor_config.mipi_config.dpi_config = &dpi_config;

  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = GFX_RST_PIN;
  panel_config.bits_per_pixel = 16;            // RGB565
  panel_config.vendor_config  = &vendor_config;
  // Colour order is left zero-initialized (= RGB; the vendor driver reads it as color_space==0 ->
  // madctl 0). If red and blue look swapped on the panel, set the colour-order field your IDF uses
  // (panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, older IDF: .color_space).

  if (esp_lcd_new_panel_jd9165(io, &panel_config, &s_panel) != ESP_OK) {
    Serial.println("JD9165: new_panel_jd9165 FAILED");
    s_panel = nullptr;
    return false;
  }
  esp_lcd_panel_reset(s_panel);
  esp_lcd_panel_init(s_panel);
  esp_lcd_panel_disp_on_off(s_panel, true);    // some panels need the display explicitly enabled

  // Serialize async draws: create the "ready" semaphore (available so the first draw proceeds) and
  // register the transfer-done callback that re-arms it.
  s_drawDone = xSemaphoreCreateBinary();
  if (s_drawDone) xSemaphoreGive(s_drawDone);
  esp_lcd_dpi_panel_event_callbacks_t cbs = {};
  cbs.on_color_trans_done = dsiOnTransDone;
  esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, nullptr);

  // 5) Backlight on (active HIGH).
  if (GFX_BL_PIN >= 0) {
    pinMode(GFX_BL_PIN, OUTPUT);
    digitalWrite(GFX_BL_PIN, HIGH);
  }
  Serial.println("JD9165: MIPI-DSI panel 1024x600 ready");
  return true;
}

void dsiPanelDrawBitmap(int x, int y, int w, int h, const uint16_t *rgb565) {
  if (!s_panel || !rgb565) return;
  // Wait for the previous async transfer to finish (timeout-guarded so a missed callback degrades
  // to the old behaviour instead of deadlocking), then issue this one.
  if (s_drawDone) xSemaphoreTake(s_drawDone, pdMS_TO_TICKS(100));
  esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, rgb565);
}

void dsiPanelFillScreen(uint16_t color) {
  if (!s_panel) return;
  // Fill a full-frame buffer and push it in ONE serialized draw. (Doing 600 row-draws back-to-back
  // here was unsynchronized and flooded "previous draw operation is not finished" at startup.)
  static uint16_t *fb = nullptr;
  if (!fb) fb = (uint16_t *)ps_malloc((size_t)PANEL_NATIVE_W * PANEL_NATIVE_H * 2);
  if (!fb) return;
  for (size_t i = 0; i < (size_t)PANEL_NATIVE_W * PANEL_NATIVE_H; i++) fb[i] = color;
  dsiPanelDrawBitmap(0, 0, PANEL_NATIVE_W, PANEL_NATIVE_H, fb);   // goes through the draw-done sync
}

#endif // BOARD_PANEL_DSI
