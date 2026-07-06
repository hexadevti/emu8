// msx_audio.cpp - AY-3-8910 PSG -> I2S output for the MSX1 platform. Mirrors atari_audio.cpp:
// a core-0 task pulls 22050 Hz samples from msx::psgGenSample() and feeds the internal DAC (CYD)
// or the external I2S amp (JC4827W543/S3). Started LAST in setup() so its I2S DMA comes up after SD.

#include "../../emu.h"
#include "msx.h"
#include "driver/i2s.h"
#include "driver/adc.h"     // adc_power_acquire(): the built-in DAC powers down the SAR ADC otherwise

#define MSX_AUD_FS 22050

static void msxAudioTask(void *) {
  uint16_t out[128];
  size_t wrote;
  while (running) {
    bool mute = (!sound) || OptionsWindow;
    for (int i = 0; i < 128; i++) out[i] = (uint16_t)(msx::psgGenSample((int)volume, mute) << 8);
#if BOARD_AUDIO_DAC
    i2s_write(I2S_NUM_0, out, sizeof(out), &wrote, portMAX_DELAY);
#else
    ampWriteDac8(out, 128);
#endif
  }
  vTaskDelete(NULL);
}

void msxPsgSetup() {
#if BOARD_AUDIO_DAC
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  cfg.sample_rate = MSX_AUD_FS;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 128;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) { printLog("MSX audio: I2S install failed"); return; }
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);     // left = DAC2 = GPIO26
  i2s_zero_dma_buffer(I2S_NUM_0);
  adc_power_acquire();                            // keep analogRead() (joystick/buttons) alive
  xTaskCreatePinnedToCore(msxAudioTask, "msxAud", 4096, NULL, 2, NULL, 0);
  printLog("MSX audio: AY-3-8910 on (I2S DAC GPIO26)");
#else
  ampBegin(MSX_AUD_FS);
  xTaskCreatePinnedToCore(msxAudioTask, "msxAud", 4096, NULL, 2, NULL, 0);
  printLog("MSX audio: AY-3-8910 on (I2S amp)");
#endif
}
