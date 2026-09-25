// emu.h must be included FIRST because it is what pulls in board.h and defines the macro.
#include "../../emu.h"
#if BOARD_HAS_ZX_CORE

// zx_audio.cpp - Spectrum beeper -> I2S output. The machine (zx_machine.cpp) integrates the EAR/MIC
// bits into 22050 Hz samples in a ring; this core-0 task drains it into the internal DAC (CYD) or the
// external I2S amp, like coleco_audio.cpp. The beeper's idle level is a DC offset, so a slow DC
// blocker keeps silence at the DAC midpoint. The emulated frame rate and the DAC clock drift apart
// slightly: an empty ring holds the last level, and a ring more than three frames deep is caught up.

#include "zx.h"
#include "driver/i2s.h"
#include "driver/adc.h"     // adc_power_acquire(): the built-in DAC powers down the SAR ADC otherwise

static void zxAudioTask(void *) {
  uint16_t out[128];
  size_t wrote;
  int32_t dc = 0;                       // running mean of the level, 8.8 fixed point
  uint8_t last = 0;
  while (running) {
    const bool mute = (!sound) || OptionsWindow;
    for (int i = 0; i < 128; i++) {
      uint8_t s;
      if (zx::beeperPop(&s)) {
        last = s;
        static int skip = 0;            // drop one sample in 32 while the ring runs deep
        if (zx::beeperDepth() > 3 * (zx::AUDIO_FS / 50) && ++skip >= 32) { skip = 0; zx::beeperPop(&s); }
      }
      dc += ((int32_t)last * 256 - dc) >> 9;
      int v = ((int32_t)last * 256 - dc) >> 8;
      v = v * (int)volume / 320;
      if (mute) v = 0;
      int d = 128 + v;
      d = d < 0 ? 0 : (d > 255 ? 255 : d);
      out[i] = (uint16_t)(d << 8);
    }
#if BOARD_AUDIO_DAC
    i2s_write(I2S_NUM_0, out, sizeof(out), &wrote, portMAX_DELAY);
#else
    (void)wrote;
    ampWriteDac8(out, 128);
#endif
  }
  vTaskDelete(NULL);
}

void zxAudioSetup() {
#if BOARD_AUDIO_DAC
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  cfg.sample_rate = zx::AUDIO_FS;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 128;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) { printLog("ZX audio: I2S install failed"); return; }
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);     // left = DAC2 = GPIO26
  i2s_zero_dma_buffer(I2S_NUM_0);
  adc_power_acquire();                            // keep analogRead() (joystick/buttons) alive
  xTaskCreatePinnedToCore(zxAudioTask, "zxAud", 4096, NULL, 2, NULL, 0);
  printLog("ZX audio: beeper on (I2S DAC GPIO26)");
#else
  ampBegin(zx::AUDIO_FS);
#if defined(BOARD_PICOCALC)
  // Static stack -- see picocalcStartAudioTask (audio_picocalc.cpp).
  picocalcStartAudioTask(zxAudioTask, "zxAud", 2);
#else
  xTaskCreatePinnedToCore(zxAudioTask, "zxAud", 4096, NULL, 2, NULL, 0);
#endif
  printLog("ZX audio: beeper on (I2S amp)");
#endif
}

#endif  // BOARD_HAS_ZX_CORE
