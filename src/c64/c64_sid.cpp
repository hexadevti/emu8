#include "../../emu.h"
#include "c64.h"
#include "driver/i2s.h"

// 3-voice SID (6581) approximation -> ESP32 internal DAC (GPIO26) via I2S DMA, an I2S amp on the
// S3, or PWM on the PicoCalc.
//   * Waveforms: triangle / sawtooth / pulse / noise (combined by AND when several are set), with
//     hard sync and ring modulation (both taken from the previous voice, as on the chip).
//   * Per-voice ADSR envelope. Decay/release step at the attack rate but follow the SID's
//     piecewise-exponential curve (the envelope counter slows down as the level falls), which is
//     where the datasheet's "3x longer" decay/release times come from.
//   * The filter ($D415-$D418): cutoff, resonance, per-voice routing and LP/BP/HP modes, as a
//     2-pole state-variable filter. "Voice 3 off" ($D418 bit 7) is honoured.
//   * Master volume ($D418 low nibble) x the app `volume`, gated by the `sound` toggle.
//
// All of it is integer maths: the PicoCalc's RP2040 has no FPU, and this runs 22050 times a second
// on the core that also drives the panel. Floats appear only where a register change forces a new
// filter coefficient (at most a few times per frame).
//
// A task synthesises 16-bit samples and feeds the DMA, which paces output at the sample rate (the
// write blocks when the ring is full, so the task self-throttles). It works in small blocks so the
// registers the 6510 writes are picked up within ~1 ms, not once per 6 ms burst.

#define SID_FS      22050
#define SID_NVOICES 3
#define SID_BLOCK   16

namespace {   // file-local SID state

uint8_t sidreg[0x20];

#define ENV_MAX (255u << 16)     // envelope level, 16.16 fixed point

struct Voice {
  uint32_t acc;          // 24-bit phase accumulator
  uint32_t lfsr;         // 23-bit noise shift register
  uint32_t prevBit19;    // for clocking the noise LFSR
  uint32_t env;          // 0..ENV_MAX envelope level
  uint16_t out;          // last 12-bit waveform output (OSC3 reads voice 3's)
  uint8_t  state;        // 0 off, 1 attack, 2 decay, 3 sustain, 4 release
  bool     prevGate;
};
Voice voice[SID_NVOICES];

// Gate edges, latched when the 6510 writes the control register (1 = rise, 2 = fall; the last one
// wins). The synth runs on the other core and only looks at the registers when it makes a sample, so
// the usual note start -- gate off, then straight back on, a few microseconds to a few ms apart --
// was almost never seen: the voice stayed at its (often low) sustain level instead of re-attacking,
// and most notes came out quiet. Only the ones with a long enough gap between them sounded right.
volatile uint8_t gateEdge[SID_NVOICES];

const int voiceBase[SID_NVOICES] = {0x00, 0x07, 0x0e};

// ADSR attack time (ms) for a full 0..255 sweep, indexed by the 4-bit rate. Decay/release use the
// same step; expDiv() stretches a full 255..0 sweep to ~3x (the datasheet's 6 ms .. 24 s).
// Converted to per-sample 16.16 increments in sidSetup().
const uint16_t adsrMs[16] = {2,8,16,24,38,56,68,80,100,250,500,800,1000,3000,5000,8000};
uint32_t atkInc[16], decInc[16];

// The SID's decay/release counter is divided down as the level drops (thresholds 93/54/26/14/6),
// which is what gives it an exponential-sounding tail instead of a linear fade.
inline uint32_t expDiv(uint32_t env) {
  uint32_t l = env >> 16;
  return l >= 93 ? 1 : l >= 54 ? 2 : l >= 26 ? 4 : l >= 14 ? 8 : l >= 6 ? 16 : 30;
}

// 24-bit accumulator step per sample for a frequency register (PAL PHI2 ~985248 Hz):
// step = freq * 985248 / 22050 ~= freq * 44.68  ->  (freq * 11438) >> 8.
inline uint32_t freqStep(uint16_t f) { return ((uint32_t)f * 11438u) >> 8; }

// ---- filter: trapezoidal (Simper) state-variable filter, Q14 coefficients ----
// Unconditionally stable at any cutoff/resonance, unlike the textbook Chamberlin form, which blows
// up at high cutoff with low resonance -- exactly where SID tunes sweep it.
int32_t fA1 = 16384, fA2 = 0, fA3 = 0, fK = 23170;   // 0 Hz, Q = 0.707
int32_t fIc1 = 0, fIc2 = 0;
uint32_t fKey = 0xFFFFFFFF;                          // cutoff+resonance the coefficients are for

void filterCoeffs(uint16_t cutoff, uint8_t res) {
  // 11-bit cutoff -> the 6581's curve: ~220 Hz at 0, ~1 kHz a quarter of the way up, ~4 kHz at
  // half, kept below Nyquist. Nearly all C64 music was written for the 6581; a linear (8580-like)
  // 30 Hz..12 kHz map puts low register values far too low and muffles the bass lines tunes route
  // through the low-pass.
  const float c = cutoff * (1.0f / 2047.0f);
  float fc = 220.0f + 17800.0f * powf(c, 2.2f);
  if (fc > SID_FS * 0.45f) fc = SID_FS * 0.45f;
  float g = tanf(3.14159265f * fc / SID_FS);
  float k = 1.0f / (0.707f + res * 0.13f);            // resonance 0..15 -> Q 0.7 .. 2.7
  float a1 = 1.0f / (1.0f + g * (g + k));
  fA1 = (int32_t)(a1 * 16384.0f);
  fA2 = (int32_t)(g * a1 * 16384.0f);
  fA3 = (int32_t)(g * g * a1 * 16384.0f);
  fK  = (int32_t)(k * 16384.0f);
}

int genSample() {
  int direct = 0, filtIn = 0;
  const uint8_t route = sidreg[0x17];
  const uint8_t modeVol = sidreg[0x18];

  // Oscillators first: sync and ring modulation read the other voices' accumulators.
  uint32_t prevAcc[SID_NVOICES];
  for (int v = 0; v < SID_NVOICES; v++) {
    const int b = voiceBase[v];
    Voice &vo = voice[v];
    prevAcc[v] = vo.acc;
    if (sidreg[b + 4] & 0x08) vo.acc = 0;           // TEST bit holds the oscillator at 0
    else vo.acc = (vo.acc + freqStep(sidreg[b] | (sidreg[b + 1] << 8))) & 0xFFFFFF;
  }
  for (int v = 0; v < SID_NVOICES; v++) {           // hard sync: reset when the source's MSB rises
    const int src = (v + 2) % SID_NVOICES;          // voice 1 <- 3, 2 <- 1, 3 <- 2
    if ((sidreg[voiceBase[v] + 4] & 0x02) && !(prevAcc[src] & 0x800000) && (voice[src].acc & 0x800000))
      voice[v].acc = 0;
  }

  for (int v = 0; v < SID_NVOICES; v++) {
    const int b = voiceBase[v];
    const uint16_t pw = (sidreg[b + 2] | (sidreg[b + 3] << 8)) & 0x0fff;
    const uint8_t ctrl = sidreg[b + 4];
    const uint8_t ad   = sidreg[b + 5];
    const uint8_t sr   = sidreg[b + 6];
    Voice &vo = voice[v];

    const bool gate = ctrl & 0x01;
    const uint8_t edge = gateEdge[v];
    if (edge) {                                    // an edge written since the last sample
      gateEdge[v] = 0;
      vo.state = (edge == 1) ? 1 : 4;              // rise -> attack, fall -> release
    } else {
      if (gate && !vo.prevGate)  vo.state = 1;     // gate rising  -> attack
      if (!gate && vo.prevGate)  vo.state = 4;     // gate falling -> release
    }
    vo.prevGate = gate;

    const uint32_t bit19 = vo.acc & (1u << 19);    // clock noise LFSR on bit19 rising
    if (bit19 && !vo.prevBit19) {
      uint32_t nb = ((vo.lfsr >> 22) ^ (vo.lfsr >> 17)) & 1;
      vo.lfsr = ((vo.lfsr << 1) | nb) & 0x7FFFFF;
    }
    vo.prevBit19 = bit19;

    uint16_t osc = 0x0FFF;   // waveforms AND together
    bool any = false;
    if (ctrl & 0x10) {       // triangle (ring mod: MSB XORed with the source voice's)
      uint32_t msb = vo.acc & 0x800000;
      if (ctrl & 0x04) msb ^= voice[(v + 2) % SID_NVOICES].acc & 0x800000;
      uint32_t t = msb ? ~vo.acc : vo.acc;
      osc &= (t >> 11) & 0x0FFF; any = true;
    }
    if (ctrl & 0x20) { osc &= (vo.acc >> 12) & 0x0FFF; any = true; }                           // sawtooth
    if (ctrl & 0x40) { osc &= (((vo.acc >> 12) & 0x0FFF) >= pw) ? 0x0FFF : 0x000; any = true; }  // pulse
    if (ctrl & 0x80) {       // noise: assemble 12 bits from LFSR taps
      uint16_t n = (((vo.lfsr >> 20) & 1) << 11) | (((vo.lfsr >> 18) & 1) << 10) |
                   (((vo.lfsr >> 14) & 1) << 9)  | (((vo.lfsr >> 11) & 1) << 8)  |
                   (((vo.lfsr >> 9)  & 1) << 7)  | (((vo.lfsr >> 5)  & 1) << 6)  |
                   (((vo.lfsr >> 2)  & 1) << 5)  | (((vo.lfsr >> 0)  & 1) << 4);
      osc &= n; any = true;
    }
    if (!any) osc = 0x800;   // no waveform selected -> centered (no contribution)
    vo.out = osc;

    const uint32_t sustain = (uint32_t)((sr >> 4) * 17) << 16;
    switch (vo.state) {
      case 1: vo.env += atkInc[ad >> 4];
              if (vo.env >= ENV_MAX) { vo.env = ENV_MAX; vo.state = 2; } break;
      case 2: { uint32_t d = decInc[ad & 0x0f] / expDiv(vo.env);
              if (vo.env <= sustain + d) { vo.env = sustain; vo.state = 3; } else vo.env -= d; } break;
      case 3: vo.env = sustain; break;
      case 4: { uint32_t d = decInc[sr & 0x0f] / expDiv(vo.env);
              if (vo.env <= d) { vo.env = 0; vo.state = 0; } else vo.env -= d; } break;
      default: vo.env = 0; break;
    }

    const int out = ((int)osc - 0x800) * (int)(vo.env >> 16) >> 8;   // about +-2048
    if (route & (1 << v))                     filtIn += out;
    else if (v != 2 || !(modeVol & 0x80))     direct += out;           // $D418 bit 7: voice 3 off
  }

  // Filter (only when something is routed through it or a mode is selected).
  if ((route & 0x07) || (modeVol & 0x70)) {
    const uint16_t cutoff = (sidreg[0x15] & 0x07) | (sidreg[0x16] << 3);
    const uint32_t key = ((uint32_t)cutoff << 4) | (route >> 4);
    if (key != fKey) { fKey = key; filterCoeffs(cutoff, route >> 4); }
    const int32_t v3 = filtIn - fIc2;
    const int32_t v1 = (fA1 * fIc1 + fA2 * v3) >> 14;                  // band-pass
    const int32_t v2 = fIc2 + ((fA2 * fIc1 + fA3 * v3) >> 14);         // low-pass
    fIc1 = 2 * v1 - fIc1;
    fIc2 = 2 * v2 - fIc2;
    const int32_t hp = filtIn - ((fK * v1) >> 14) - v2;                 // high-pass
    int32_t f = 0;
    if (modeVol & 0x10) f += v2;
    if (modeVol & 0x20) f += v1;
    if (modeVol & 0x40) f += hp;
    direct += f;
  }

  int mix = direct * (modeVol & 0x0f) / 15;         // SID master volume
  mix = mix * volume / 255;                         // app master volume
  if (!sound) mix = 0;                              // mute toggle
  // Gain for the PicoCalc's small speaker: one voice at full envelope is ~+-16000. Three together
  // would clip, so peaks above the knee are compressed smoothly toward full scale instead of being
  // squared off (hard clipping is what sounds harsh).
  mix *= 8;
  if (mix > 150000) mix = 150000; else if (mix < -150000) mix = -150000;   // keeps e * room in 32 bits
  const int knee = 20000, room = 32767 - knee;
  if (mix > knee)       { int e = mix - knee;  mix =   knee + e * room / (e + room); }
  else if (mix < -knee) { int e = -mix - knee; mix = -(knee + e * room / (e + room)); }
  return 32768 + mix;                               // unsigned, centred on 0x8000
}

void sidTask(void *) {
  static uint16_t buf[SID_BLOCK];
  size_t wrote;
  while (running) {
    for (int i = 0; i < SID_BLOCK; i++) buf[i] = (uint16_t)genSample();   // 16-bit, 0x8000 centre
#if defined(BOARD_DESKTOP)
    // EMU_SID_LOG=<file>: every 20 ms, the 25 registers + the 3 envelope levels (offline analysis),
    // and EMU_SID_WAV=<file>: the raw 16-bit output.
    static FILE *slog = []{ const char *p = getenv("EMU_SID_LOG"); return p ? fopen(p, "wb") : nullptr; }();
    static FILE *swav = []{ const char *p = getenv("EMU_SID_WAV"); return p ? fopen(p, "wb") : nullptr; }();
    static int slogN = 0;
    if (swav) { fwrite(buf, 2, SID_BLOCK, swav); fflush(swav); }
    if (slog && (slogN += SID_BLOCK) >= SID_FS / 50) {
      slogN = 0;
      uint8_t rec[28];
      memcpy(rec, sidreg, 25);
      for (int v = 0; v < 3; v++) rec[25 + v] = (uint8_t)(voice[v].env >> 16);
      fwrite(rec, 1, 28, slog); fflush(slog);
    }
#endif
#if BOARD_AUDIO_DAC
    i2s_write(I2S_NUM_0, buf, sizeof(buf), &wrote, portMAX_DELAY);          // DAC uses the high byte
#else
    ampWriteDac8(buf, SID_BLOCK);
#endif
  }
  vTaskDelete(NULL);
}

} // anonymous namespace

// ---- public entry points (C linkage via proto.h) ----
void sidWrite(uint8_t reg, uint8_t val) {
  if (reg >= 0x20) return;
  if (reg == 0x04 || reg == 0x0b || reg == 0x12) {   // voice control: latch gate edges
    const uint8_t g = val & 1;
    if (g != (sidreg[reg] & 1)) gateEdge[reg / 7] = g ? 1 : 2;
  }
  sidreg[reg] = val;
}

unsigned char sidRead(uint8_t reg) {
  if (reg == 0x1b) return (uint8_t)(voice[2].out >> 4);     // OSC3: top 8 bits of voice 3's waveform
  if (reg == 0x1c) return (uint8_t)(voice[2].env >> 16);    // ENV3
  return 0;
}

#if defined(BOARD_DESKTOP)
// Desktop debug facade: snapshot the SID register file ($D400-$D418 shadow) and the live per-voice
// envelope level + ADSR state, for the SID control window (src/desktop/ui_imgui.cpp). Register edits
// from the window go through the normal sidWrite() so the synth reflects them immediately.
int sidDebugRegs(uint8_t *out, int max) {
  int n = (max < 0x19) ? max : 0x19;          // 25 registers $D400-$D418
  for (int i = 0; i < n; i++) out[i] = sidreg[i];
  return n;
}
void sidDebugVoice(int v, float *env, uint8_t *state) {
  if (v < 0 || v >= SID_NVOICES) { if (env) *env = 0; if (state) *state = 0; return; }
  if (env)   *env   = (float)(voice[v].env >> 16);
  if (state) *state = voice[v].state;
}
#endif

void sidSetup() {
  memset(sidreg, 0, sizeof(sidreg));
  for (int v = 0; v < SID_NVOICES; v++) gateEdge[v] = 0;
  for (int v = 0; v < SID_NVOICES; v++) {
    voice[v] = Voice();
    voice[v].lfsr = 0x7FFFF8;                              // non-zero seed
  }
  for (int r = 0; r < 16; r++) {
    atkInc[r] = (uint32_t)((float)ENV_MAX / (adsrMs[r] * 0.001f * SID_FS));
    decInc[r] = atkInc[r];
  }
  fIc1 = fIc2 = 0; fKey = 0xFFFFFFFF;

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
#if defined(BOARD_PICOCALC)
  // Static stack -- a 4KB task create that fails is FATAL on this board, and this is the
  // last thing the boot asks of the heap. See picocalcStartAudioTask (audio_picocalc.cpp).
  picocalcStartAudioTask(sidTask, "sidTask", 2);
#else
  xTaskCreatePinnedToCore(sidTask, "sidTask", 4096, NULL, 2, NULL, 0);  // core 0
#endif
  printLog("SID: 3-voice synth on (I2S amp)");
#endif
}
