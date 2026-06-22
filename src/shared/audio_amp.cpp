// audio_amp.cpp - external I2S Class-D amp output for the JC4827W543 (ESP32-S3, no internal DAC).
//
// The ESP32-S3 has no DAC, so the C64 SID / NES APU / Atari TIA / Apple speaker can't use the
// internal-DAC I2S path the CYD uses. Instead they feed the board's onboard I2S amp
// (NS4168 / MAX98357-class) over standard Philips I2S (16-bit), pins from board.h. Only one
// platform's audio runs at a time, so a single I2S_NUM_0 driver is shared.
//
// Compiled only on the no-DAC board(s); on the CYD the cores keep their I2S-DAC path.

#include "../../emu.h"

#if !BOARD_AUDIO_DAC

#include "driver/i2s.h"

// Install the I2S driver (once) and route it to the amp pins. Idempotent across cores.
void ampBegin(int sampleRate) {
  static bool done = false;
  if (done) return;
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = sampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;   // stereo frames (amp is mono; we duplicate)
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 128;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) { printLog("amp: I2S install failed"); return; }
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_BCLK_PIN;
  pins.ws_io_num = I2S_LRCLK_PIN;
  pins.data_out_num = I2S_DOUT_PIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
  done = true;
  printLog("amp: I2S external amp output on");
}

// Write `n` samples that are in the cores' DAC format (8-bit value in the HIGH byte, i.e.
// (dac<<8), so unsigned and centered on 0x8000). Converts to 16-bit signed and duplicates
// to L+R for the mono amp. Blocks on the I2S DMA (which paces playback).
void ampWriteDac8(const uint16_t *dacBuf, int n) {
  int16_t st[256];   // up to 128 frames x2 (L,R)
  int i = 0;
  while (i < n) {
    int chunk = (n - i > 128) ? 128 : (n - i);
    for (int k = 0; k < chunk; k++) {
      int16_t s = (int16_t)((int)dacBuf[i + k] - 32768);   // unsigned 0x8000-center -> signed 0
      st[k * 2] = s; st[k * 2 + 1] = s;
    }
    size_t wrote;
    i2s_write(I2S_NUM_0, st, chunk * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
    i += chunk;
  }
}

// Write `n` mono 16-bit-signed samples (duplicated L+R). Used by the Apple speaker square wave.
void ampWriteMono(const int16_t *mono, int n) {
  int16_t st[256];
  int i = 0;
  while (i < n) {
    int chunk = (n - i > 128) ? 128 : (n - i);
    for (int k = 0; k < chunk; k++) { st[k * 2] = mono[i + k]; st[k * 2 + 1] = mono[i + k]; }
    size_t wrote;
    i2s_write(I2S_NUM_0, st, chunk * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
    i += chunk;
  }
}

#endif // !BOARD_AUDIO_DAC
