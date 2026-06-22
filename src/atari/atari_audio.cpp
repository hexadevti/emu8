#include "../../emu.h"
#include "atari.h"
#include "driver/i2s.h"
#include "driver/adc.h"   // adc_power_acquire(): keep the SAR ADC powered (I2S DAC powers it down)

// TIA audio (2 channels) -> ESP32 internal DAC (GPIO26) via I2S DMA, mirroring the NES APU path.
// Each channel has AUDC (waveform/mode 0-15), AUDF (frequency divider 0-31) and AUDV (volume 0-15).
// This is register synthesis tuned by ear (not the exact polynomial-counter hardware): pure-tone
// modes become square waves at the right pitch, poly/noise modes become an LFSR of the matching
// length. Good enough for recognisable game sound; the NES APU takes the same pragmatic approach.
//
// I2S_NUM_0 is free on the Atari path (the SID/NES APU are other-platform only), so we own it.

#define AUD_FS   22050
#define TIA_BASE 31400.0     // NTSC TIA audio clock (3.579545MHz / 114), Hz

namespace atari {

struct Voice {
  uint8_t  audc = 0, audf = 0, audv = 0;
  bool     noise = false;
  uint8_t  lfsrLen = 9;
  uint16_t lfsr = 1;
  uint32_t phase = 0, step = 0;   // tone: 32-bit square accumulator; noise: 16.16 LFSR-clock acc
};
static Voice *v = nullptr;      // [2], malloc'd in audioSetup (not static BSS — DRAM budget is full)

static void setTone(Voice &q, double freq) {
  q.noise = false;
  q.step  = (uint32_t)(freq * 4294967296.0 / AUD_FS);
}
static void setNoise(Voice &q, int len, double clk) {
  q.noise = true; q.lfsrLen = len;
  q.step  = (uint32_t)(clk * 65536.0 / AUD_FS);     // 16.16 fixed-point clock rate
}

// Recompute a channel's waveform whenever AUDC/AUDF changes.
static void updateVoice(int i) {
  Voice &q = v[i];
  double divider = q.audf + 1;
  switch (q.audc & 0x0F) {
    case 4: case 5:           setTone(q, TIA_BASE / (2.0 * divider));        break;  // pure tone /2
    case 12: case 13:         setTone(q, TIA_BASE / (6.0 * divider));        break;  // pure tone /6
    case 6: case 10: case 14: setTone(q, TIA_BASE / (2.0 * 31.0 * divider)); break;  // low pure tone
    case 8:                   setNoise(q, 9, TIA_BASE / divider);            break;  // white noise
    case 7: case 9:           setNoise(q, 5, TIA_BASE / divider);            break;  // 5-bit poly
    case 1: case 2: case 3:   setNoise(q, 4, TIA_BASE / divider);            break;  // 4-bit poly buzz
    case 15:                  setNoise(q, 5, TIA_BASE / (6.0 * divider));    break;  // 5-bit poly /6
    default:                  q.step = 0; q.noise = false;                   break;  // 0/11: silent
  }
}

static inline void clockLfsr(Voice &q) {
  uint16_t b;
  switch (q.lfsrLen) {
    case 4: b = (q.lfsr ^ (q.lfsr >> 1)) & 1; q.lfsr = ((q.lfsr >> 1) | (b << 3)) & 0x0F; break;
    case 5: b = (q.lfsr ^ (q.lfsr >> 2)) & 1; q.lfsr = ((q.lfsr >> 1) | (b << 4)) & 0x1F; break;
    default: b = (q.lfsr ^ (q.lfsr >> 4)) & 1; q.lfsr = ((q.lfsr >> 1) | (b << 8)) & 0x1FF; break;
  }
  if (q.lfsr == 0) q.lfsr = 1;
}

void audioWrite(uint8_t reg, uint8_t val) {
  if (!v) return;
  switch (reg) {
    case 0x15: v[0].audc = val & 0x0F; updateVoice(0); break;
    case 0x16: v[1].audc = val & 0x0F; updateVoice(1); break;
    case 0x17: v[0].audf = val & 0x1F; updateVoice(0); break;
    case 0x18: v[1].audf = val & 0x1F; updateVoice(1); break;
    case 0x19: v[0].audv = val & 0x0F; break;
    case 0x1A: v[1].audv = val & 0x0F; break;
    default: break;
  }
}

static int genSample() {
  if (!v) return 128;
  int mix = 0;                                 // 0..30 (two channels x 0..15)
  for (int i = 0; i < 2; i++) {
    Voice &q = v[i];
    if (q.audv == 0 || q.step == 0) continue;
    if (q.noise) {
      q.phase += q.step;
      while (q.phase >= 0x10000) { q.phase -= 0x10000; clockLfsr(q); }
      if (q.lfsr & 1) mix += q.audv;
    } else {
      q.phase += q.step;
      if (q.phase & 0x80000000u) mix += q.audv;    // top bit = square wave
    }
  }
  int s = (mix - 15) * (int)volume / 255;      // center on 0, apply app master volume
  if (!sound || OptionsWindow) s = 0;          // mute toggle / silence while the menu is open
  int dac = 128 + s * 4;
  if (dac < 0)   dac = 0;
  if (dac > 255) dac = 255;
  return dac;
}

static void audioTask(void *) {
  uint16_t out[128];                           // on the task stack (dram0_0_seg is tight)
  size_t wrote;
  while (running) {
    for (int i = 0; i < 128; i++) out[i] = (uint16_t)(genSample() << 8);   // DAC uses the high byte
#if BOARD_AUDIO_DAC
    i2s_write(I2S_NUM_0, out, sizeof(out), &wrote, portMAX_DELAY);
#else
    ampWriteDac8(out, 128);                                                // S3: 8-bit DAC -> I2S amp
#endif
  }
  vTaskDelete(NULL);
}

static void audioSetup() {
  if (!v) v = (Voice *)malloc(2 * sizeof(Voice));
  if (!v) { printLog("Atari audio: voice malloc failed (no sound)"); return; }
  for (int i = 0; i < 2; i++) { v[i] = Voice(); v[i].lfsr = 1; }

#if BOARD_AUDIO_DAC
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  cfg.sample_rate = AUD_FS;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 128;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
    printLog("Atari audio: I2S install failed (no sound)");
    return;
  }
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);   // left = DAC2 = GPIO26 (CYD speaker)
  i2s_zero_dma_buffer(I2S_NUM_0);
  // Initialising the I2S built-in DAC powers DOWN the SAR ADC, which freezes analogRead() — and
  // the joystick/buttons are read via analogRead. Re-acquire ADC power so input keeps working.
  adc_power_acquire();
  xTaskCreatePinnedToCore(audioTask, "atariAud", 4096, NULL, 2, NULL, 0);   // core 0
  printLog("Atari audio: 2 TIA voices on (I2S DAC GPIO26)");
#else
  // ESP32-S3: external I2S amp (no internal DAC).
  ampBegin(AUD_FS);
  xTaskCreatePinnedToCore(audioTask, "atariAud", 4096, NULL, 2, NULL, 0);   // core 0
  printLog("Atari audio: 2 TIA voices on (I2S amp)");
#endif
}

} // namespace atari

// C-linkage entry point (called from the platform dispatch, like nesApuSetup / sidSetup).
void atariAudioSetup() { atari::audioSetup(); }
