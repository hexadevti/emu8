// audio_picocalc_p4.cpp -- PicoCalc speakers driven by an ESP32-P4 module (BOARD_PICOCALC_P4).
//
// The PicoCalc mainboard expects two PWM-ish logic signals on its speaker nets (an RC filter and
// the amp sit behind them); the RP2350 build feeds them with PWMAudio (audio_picocalc.cpp). The P4
// has a better fit for that: I2S0 in PDM TX mode, DAC line mode, where the hardware PCM-to-PDM
// filter turns 16-bit samples into a sigma-delta bitstream on up to two data pins. Two-line DAC
// mode puts the left slot on dout and the right slot on dout2, so the stereo frames written here
// land on AUDIO_L_PIN / AUDIO_R_PIN. No clock pin is needed in DAC mode.
//
// The PDM side always runs at PDM_RATE, whatever the core asks for. Every core but the Apple II
// makes 22050 Hz, and the DAC-mode PDM at 22050 sounded bad (C64) where the Apple's 44100 was clean,
// so the lower rates are brought up here instead: linear interpolation, which for the usual 2:1
// also knocks the 22 kHz images down instead of leaving them all to the hardware up-sampler.
//
// Replaces src/shared/audio_amp.cpp (compiled out by BOARD_AUDIO_PDM, which also keeps the legacy
// driver/i2s.h out of the link: IDF aborts at boot if the legacy and the new I2S driver are both
// present). Same three entry points, same contract: the writes block on the DMA, which paces the
// audio task.
#include "../../emu.h"

#if BOARD_AUDIO_PDM

#include "driver/i2s_pdm.h"

#define PDM_RATE 44100

static i2s_chan_handle_t s_tx = nullptr;

// Resampler state: input-to-output rate ratio in 16.16, the position between the previous input
// sample and the next, and that previous sample. Output frames collect in s_out (stereo).
static uint32_t s_step = 1u << 16;
static uint32_t s_phase = 0;
static int32_t  s_prev = 0;
static int16_t  s_out[256];
static int      s_outN = 0;

void ampBegin(int sampleRate) {
  static bool done = false;
  if (done) return;
  done = true;                                     // one attempt: a failure is logged, not retried
  s_step = (uint32_t)(((uint64_t)sampleRate << 16) / PDM_RATE);

  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num  = 8;
  cc.dma_frame_num = 128;
  cc.auto_clear    = true;                         // underrun plays silence, not the last buffer
  esp_err_t e = i2s_new_channel(&cc, &s_tx, nullptr);

  i2s_pdm_tx_config_t pc = {
    .clk_cfg  = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(PDM_RATE),
    .slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .clk  = I2S_GPIO_UNUSED,
      .dout = (gpio_num_t)AUDIO_L_PIN,
      .dout2 = (gpio_num_t)AUDIO_R_PIN,
      .invert_flags = { .clk_inv = 0 },
    },
  };
  if (e == ESP_OK) e = i2s_channel_init_pdm_tx_mode(s_tx, &pc);
  if (e == ESP_OK) e = i2s_channel_enable(s_tx);
  if (e != ESP_OK) {
    char m[64];
    snprintf(m, sizeof(m), "amp: PDM TX init failed (%s)", esp_err_to_name(e));
    printLog(m);
    if (s_tx) { i2s_del_channel(s_tx); s_tx = nullptr; }
    return;
  }
  char m[64];
  snprintf(m, sizeof(m), "amp: PDM DAC output on GPIO%d/%d at %d Hz (from %d)", AUDIO_L_PIN, AUDIO_R_PIN,
           PDM_RATE, sampleRate);
  printLog(m);
}

static void flushOut() {
  size_t wrote;
  if (s_tx && s_outN) i2s_channel_write(s_tx, s_out, (size_t)s_outN * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
  s_outN = 0;
}

// One input sample: emit the output frames that fall between the previous sample and this one.
static inline void put(int32_t x) {
  while (s_phase < (1u << 16)) {
    const int16_t y = (int16_t)(s_prev + (((x - s_prev) * (int32_t)(s_phase >> 1)) >> 15));   // fits int32
    s_out[s_outN * 2] = y; s_out[s_outN * 2 + 1] = y;
    if (++s_outN == 128) flushOut();
    s_phase += s_step;
  }
  s_phase -= 1u << 16;
  s_prev = x;
}

// `n` samples in the cores' DAC format (8-bit value in the HIGH byte, unsigned, centred on 0x8000),
// to signed 16-bit on both channels.
void ampWriteDac8(const uint16_t *dacBuf, int n) {
  for (int i = 0; i < n; i++) put((int32_t)dacBuf[i] - 32768);
  flushOut();
}

// `n` mono signed 16-bit samples (the Apple speaker), to both channels.
void ampWriteMono(const int16_t *mono, int n) {
  for (int i = 0; i < n; i++) put(mono[i]);
  flushOut();
}

#endif // BOARD_AUDIO_PDM
