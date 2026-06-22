#include "../../emu.h"
#include "c64.h"
#include "driver/i2s.h"

// 3-voice SID (6581) approximation -> ESP32 internal DAC (GPIO26) via I2S DMA.
//   * Waveforms: triangle / sawtooth / pulse / noise (combined by AND when several are set).
//   * Per-voice ADSR envelope (rate tables approximated; decay/release 3x the attack rate).
//   * Master volume ($D418 low nibble) x the app `volume`, gated by the `sound` toggle.
//   * The analog filter ($D415-$D418 high bits) is NOT emulated.
//
// A core-0 task synthesises 16-bit samples and feeds the I2S DMA, which paces output at the
// sample rate (i2s_write blocks when the DMA is full, so the task self-throttles).

#define SID_FS      22050
#define SID_NVOICES 3

namespace {   // file-local SID state

uint8_t sidreg[0x20];

struct Voice {
  uint32_t acc;          // 24-bit phase accumulator
  uint32_t lfsr;         // 23-bit noise shift register
  uint32_t prevBit19;    // for clocking the noise LFSR
  float    env;          // 0..255 envelope level
  uint8_t  state;        // 0 off, 1 attack, 2 decay, 3 sustain, 4 release
  bool     prevGate;
};
Voice voice[SID_NVOICES];

const int voiceBase[SID_NVOICES] = {0x00, 0x07, 0x0e};

// ADSR attack time (ms) for a full 0..255 sweep, indexed by the 4-bit rate. Decay/release
// are 3x slower. Converted to per-sample increments in sidSetup().
const uint16_t adsrMs[16] = {2,8,16,24,38,56,68,80,100,250,500,800,1000,3000,5000,8000};
float atkInc[16], decInc[16];

// 24-bit accumulator step per sample for a frequency register (PAL PHI2 ~985248 Hz):
// step = freq * 985248 / 22050 ~= freq * 44.68  ->  (freq * 11438) >> 8.
inline uint32_t freqStep(uint16_t f) { return ((uint32_t)f * 11438u) >> 8; }

int genSample() {
  int mix = 0;
  for (int v = 0; v < SID_NVOICES; v++) {
    int b = voiceBase[v];
    uint16_t freq = sidreg[b + 0] | (sidreg[b + 1] << 8);
    uint16_t pw   = (sidreg[b + 2] | (sidreg[b + 3] << 8)) & 0x0fff;
    uint8_t  ctrl = sidreg[b + 4];
    uint8_t  ad   = sidreg[b + 5];
    uint8_t  sr   = sidreg[b + 6];
    Voice   &vo   = voice[v];

    bool gate = ctrl & 0x01;
    if (gate && !vo.prevGate)  vo.state = 1;       // gate rising  -> attack
    if (!gate && vo.prevGate)  vo.state = 4;       // gate falling -> release
    vo.prevGate = gate;

    if (ctrl & 0x08) vo.acc = 0;                   // TEST bit holds the oscillator at 0
    else             vo.acc = (vo.acc + freqStep(freq)) & 0xFFFFFF;

    uint32_t bit19 = vo.acc & (1u << 19);          // clock noise LFSR on bit19 rising
    if (bit19 && !vo.prevBit19) {
      uint32_t nb = ((vo.lfsr >> 22) ^ (vo.lfsr >> 17)) & 1;
      vo.lfsr = ((vo.lfsr << 1) | nb) & 0x7FFFFF;
    }
    vo.prevBit19 = bit19;

    uint16_t osc = 0x0FFF;   // waveforms AND together
    bool any = false;
    if (ctrl & 0x10) {       // triangle
      uint32_t t = (vo.acc & 0x800000) ? ~vo.acc : vo.acc;
      osc &= (t >> 11) & 0x0FFF; any = true;
    }
    if (ctrl & 0x20) { osc &= (vo.acc >> 12) & 0x0FFF; any = true; }                 // sawtooth
    if (ctrl & 0x40) { osc &= (((vo.acc >> 12) & 0x0FFF) < pw) ? 0x000 : 0x0FFF; any = true; }  // pulse
    if (ctrl & 0x80) {       // noise: assemble 12 bits from LFSR taps
      uint16_t n = (((vo.lfsr >> 20) & 1) << 11) | (((vo.lfsr >> 18) & 1) << 10) |
                   (((vo.lfsr >> 14) & 1) << 9)  | (((vo.lfsr >> 11) & 1) << 8)  |
                   (((vo.lfsr >> 9)  & 1) << 7)  | (((vo.lfsr >> 5)  & 1) << 6)  |
                   (((vo.lfsr >> 2)  & 1) << 5)  | (((vo.lfsr >> 0)  & 1) << 4);
      osc &= n; any = true;
    }
    if (!any) osc = 0x800;   // no waveform selected -> centered (no contribution)

    float sustain = (sr >> 4) * 17.0f;             // sustain level 0..255
    switch (vo.state) {
      case 1: vo.env += atkInc[ad >> 4];   if (vo.env >= 255)     { vo.env = 255; vo.state = 2; } break;
      case 2: vo.env -= decInc[ad & 0x0f]; if (vo.env <= sustain) { vo.env = sustain; vo.state = 3; } break;
      case 3: vo.env  = sustain; break;
      case 4: vo.env -= decInc[sr & 0x0f]; if (vo.env <= 0)       { vo.env = 0; vo.state = 0; } break;
      default: vo.env = 0; break;
    }

    mix += ((int)osc - 0x800) * (int)vo.env / 255; // signed, env-scaled
  }

  mix = mix * (sidreg[0x18] & 0x0f) / 15;          // SID master volume
  mix = mix * volume / 255;                         // app master volume
  if (!sound) mix = 0;                              // mute toggle
  int dac = 128 + (mix >> 6);                       // center on the 8-bit DAC midpoint
  if (dac < 0)   dac = 0;
  if (dac > 255) dac = 255;
  return dac;
}

void sidTask(void *) {
  static uint16_t buf[128];
  size_t wrote;
  while (running) {
    for (int i = 0; i < 128; i++) buf[i] = (uint16_t)(genSample() << 8);  // DAC uses high byte
#if BOARD_AUDIO_DAC
    i2s_write(I2S_NUM_0, buf, sizeof(buf), &wrote, portMAX_DELAY);
#else
    ampWriteDac8(buf, 128);                                              // S3: 8-bit DAC -> I2S amp
#endif
  }
  vTaskDelete(NULL);
}

} // anonymous namespace

// ---- public entry points (C linkage via proto.h) ----
void sidWrite(uint8_t reg, uint8_t val) { if (reg < 0x20) sidreg[reg] = val; }

unsigned char sidRead(uint8_t reg) {
  if (reg == 0x1b) return (voice[2].acc >> 16) & 0xff;   // OSC3 (random/waveform read)
  if (reg == 0x1c) return (uint8_t)voice[2].env;          // ENV3
  return 0;
}

void sidSetup() {
  memset(sidreg, 0, sizeof(sidreg));
  for (int v = 0; v < SID_NVOICES; v++) {
    voice[v] = Voice();
    voice[v].lfsr = 0x7FFFF8;                              // non-zero seed
  }
  for (int r = 0; r < 16; r++) {
    atkInc[r] = 255.0f / (adsrMs[r] * 0.001f * SID_FS);
    decInc[r] = 255.0f / (adsrMs[r] * 3 * 0.001f * SID_FS);
  }

#if BOARD_AUDIO_DAC
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  cfg.sample_rate = SID_FS;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 128;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
    printLog("SID: I2S install failed (no sound)");
    return;
  }
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);   // left channel = DAC2 = GPIO26 (the CYD speaker)
  i2s_zero_dma_buffer(I2S_NUM_0);
  xTaskCreatePinnedToCore(sidTask, "sidTask", 4096, NULL, 2, NULL, 0);  // core 0
  printLog("SID: 3-voice synth on (I2S DAC GPIO26)");
#else
  // ESP32-S3: external I2S amp (no internal DAC).
  ampBegin(SID_FS);
  xTaskCreatePinnedToCore(sidTask, "sidTask", 4096, NULL, 2, NULL, 0);  // core 0
  printLog("SID: 3-voice synth on (I2S amp)");
#endif
}
