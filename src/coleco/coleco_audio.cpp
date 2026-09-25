// This ColecoVision translation unit is compiled out entirely on boards that clear
// BOARD_HAS_COLECO_CORE (it needs both the MSX VDP and the SMS PSG -- see board.h).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro;
// the guard below then empties the file, keeping the Z80 core's static RAM out of the image.
#include "../../emu.h"
#if BOARD_HAS_COLECO_CORE

// coleco_audio.cpp - SN76489 PSG -> I2S output for the ColecoVision. The SAME chip and PSG core as the
// SMS (sms::psgGenSample), so this mirrors sms_audio.cpp: a core-0 task
// pulls 22050 Hz samples from sms::psgGenSample() and feeds the internal DAC (CYD) or the external I2S
// amp (JC4827W543/S3). Started LAST in setup() so its I2S DMA comes up after SD.

#include "../../emu.h"
#include "../sms/sms.h"
#include "driver/i2s.h"
#include "driver/adc.h"     // adc_power_acquire(): the built-in DAC powers down the SAR ADC otherwise

#define COL_AUD_FS 22050

static void colecoAudioTask(void *) {
  uint16_t out[128];
  size_t wrote;
  while (running) {
    bool mute = (!sound) || OptionsWindow;
    for (int i = 0; i < 128; i++) out[i] = (uint16_t)(sms::psgGenSample((int)volume, mute) << 8);
#if BOARD_AUDIO_DAC
    i2s_write(I2S_NUM_0, out, sizeof(out), &wrote, portMAX_DELAY);
#else
    ampWriteDac8(out, 128);
#endif
  }
  vTaskDelete(NULL);
}

void colecoPsgSetup() {
#if BOARD_AUDIO_DAC
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  cfg.sample_rate = COL_AUD_FS;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 128;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) { printLog("COLECO audio: I2S install failed"); return; }
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);     // left = DAC2 = GPIO26
  i2s_zero_dma_buffer(I2S_NUM_0);
  adc_power_acquire();                            // keep analogRead() (joystick/buttons) alive
  xTaskCreatePinnedToCore(colecoAudioTask, "colAud", 4096, NULL, 2, NULL, 0);
  printLog("COLECO audio: SN76489 on (I2S DAC GPIO26)");
#else
  ampBegin(COL_AUD_FS);
#if defined(BOARD_PICOCALC)
  // Static stack -- a 4KB task create that fails is FATAL on this board, and this is the
  // last thing the boot asks of the heap. See picocalcStartAudioTask (audio_picocalc.cpp).
  picocalcStartAudioTask(colecoAudioTask, "colAud", 2);
#else
  xTaskCreatePinnedToCore(colecoAudioTask, "colAud", 4096, NULL, 2, NULL, 0);
#endif
  printLog("COLECO audio: SN76489 on (I2S amp)");
#endif
}

#endif  // BOARD_HAS_COLECO_CORE
