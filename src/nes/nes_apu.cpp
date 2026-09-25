#include "../../emu.h"
#include "nes.h"
#include "driver/i2s.h"

// NES APU (2A03 audio) -> ESP32 internal DAC (GPIO26) via I2S DMA, mirroring the C64 SID path.
//   * 2 pulse (square, 4 duties, sweep, envelope, length), triangle (linear+length counter),
//     noise (LFSR, envelope, length). DMC (sample channel) is NOT emulated.
//   * The frame sequencer (envelope/linear at 240Hz, length/sweep at 120Hz) is clocked from
//     EMULATED time: the PPU bumps apuQuarterTicks four times per frame and the audio task
//     consumes those ticks. Driving it from the audio sample count instead (what this used to do)
//     runs the envelopes at a constant 240Hz while the interpreter renders at 40-50fps, so decays
//     finish early and notes end up clipped against their own channel.
//   * Channel timers come from the period registers; per-sample phase accumulators synthesise the
//     waveforms directly (register-based synthesis, like the SID — not cycle-accurate).
//   * Mixing uses the standard linear approximation of the 2A03's non-linear DAC followed by a
//     DC blocker, so the output is genuinely bipolar instead of the old "mix - 16" guess. Boards
//     with a real amp get full 16-bit samples; only the CYD's internal DAC quantises to 8 bits.
//   * Output gated by the app `sound` toggle and scaled by `volume`.
//
// I2S_NUM_0 is free on the NES path (the SID is C64-only), so nesApuSetup() owns it.

#define APU_FS 22050

namespace nes {

// ---- lookup tables ----
static const uint8_t lengthTable[32] = {
  10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,
  12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static const uint16_t noisePeriodTbl[16] = {
  4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068 };
static const uint8_t dutySeq[4][8] = {
  {0,1,0,0,0,0,0,0}, {0,1,1,0,0,0,0,0}, {0,1,1,1,1,0,0,0}, {1,0,0,1,1,1,1,1} };
static const uint8_t triSeq[32] = {
  15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };

// ---- channel state ----
struct Pulse {
  bool     enabled;
  uint16_t timer;            // 11-bit period
  uint8_t  duty;
  bool     constVol, lengthHalt;     // lengthHalt also = envelope loop
  uint8_t  volParam;
  bool     envStart; uint8_t envDivider, envDecay;
  bool     sweepEnable, sweepNegate, sweepReload;
  bool     sweepMute;        // sweep target out of range -> channel silenced (recomputed on writes)
  uint8_t  sweepPeriod, sweepShift, sweepDivider;
  uint8_t  lengthCounter;
  uint32_t phase, step;      // 32-bit cycle accumulator
};
struct Triangle {
  bool     enabled;
  uint16_t timer;
  bool     control;          // linear counter control / length halt
  uint8_t  linReload, linCounter; bool linReloadFlag;
  uint8_t  lengthCounter;
  uint32_t phase, step;
};
struct Noise {
  bool     enabled;
  bool     constVol, lengthHalt;
  uint8_t  volParam;
  bool     envStart; uint8_t envDivider, envDecay;
  bool     mode;
  uint16_t period; uint16_t lfsr;
  uint8_t  lengthCounter;
  uint32_t phase, step;      // 16.16 fixed-point LFSR-clock accumulator
};

static Pulse    pulse[2];
static Triangle tri;
static Noise    noise;
static uint8_t  frameMode;            // $4017 bit7 (4/5-step) — only 4-step behaviour modelled
static bool     frameIRQInhibit;
static uint32_t apuQuarterSeen;       // quarter-frame ticks already applied (chases apuQuarterTicks)
static bool     halfTick;
static int32_t  hpPrevIn, hpPrevOut;  // one-pole DC blocker state (see genSample16)

// ---- period -> phase step (computed on register writes, not per sample) ----
// pulse: freq = 1.789773MHz / (16*(P+1));  step = freq * 2^32 / Fs
static uint32_t pulseStep(uint16_t P) {
  if (P < 8) return 0;                                  // hardware mutes pulse below 8
  return (uint32_t)(((uint64_t)1789773ull << 32) / ((uint64_t)16 * (P + 1) * APU_FS));
}
static uint32_t triStep(uint16_t P) {                    // freq = clock / (32*(P+1))
  if (P < 2) return 0;
  return (uint32_t)(((uint64_t)1789773ull << 32) / ((uint64_t)32 * (P + 1) * APU_FS));
}
static uint32_t noiseStep(uint16_t per) {                // LFSR clocks at clock/per; 16.16 acc
  if (per == 0) return 0;
  return (uint32_t)(((uint64_t)1789773ull << 16) / ((uint64_t)per * APU_FS));
}

// ---- frame-sequencer clocks ----
static void clockEnvelope(uint8_t volParam, bool loop, bool &start, uint8_t &div, uint8_t &decay) {
  if (start) { start = false; decay = 15; div = volParam; }
  else if (div == 0) { div = volParam; if (decay > 0) decay--; else if (loop) decay = 15; }
  else div--;
}
static void clockLinear() {
  if (tri.linReloadFlag) tri.linCounter = tri.linReload;
  else if (tri.linCounter > 0) tri.linCounter--;
  if (!tri.control) tri.linReloadFlag = false;
}
static void clockLengths() {
  if (!pulse[0].lengthHalt && pulse[0].lengthCounter) pulse[0].lengthCounter--;
  if (!pulse[1].lengthHalt && pulse[1].lengthCounter) pulse[1].lengthCounter--;
  if (!tri.control       && tri.lengthCounter)        tri.lengthCounter--;
  if (!noise.lengthHalt  && noise.lengthCounter)      noise.lengthCounter--;
}
// The sweep unit silences its channel whenever the *target* period is out of range, even when the
// sweep is disabled and even when it never reloads the timer. Without this, parked channels leak a
// continuous ultrasonic/garbage tone into the mix, which is a large part of why it sounded muddy.
static void updateSweepMute(Pulse &p, int ch) {
  int change = p.timer >> p.sweepShift;
  if (p.sweepNegate) change = -change - (ch == 0 ? 1 : 0);
  int target = (int)p.timer + change;
  p.sweepMute = (p.timer < 8) || (!p.sweepNegate && target > 0x7FF);
}

static void clockSweep(Pulse &p, int ch) {
  if (p.sweepDivider == 0 && p.sweepEnable && p.sweepShift > 0) {
    int change = p.timer >> p.sweepShift;
    if (p.sweepNegate) change = -change - (ch == 0 ? 1 : 0);   // pulse1 ones', pulse2 twos' comp
    int target = (int)p.timer + change;
    if (target >= 8 && target <= 0x7FF) { p.timer = (uint16_t)target; p.step = pulseStep(p.timer); }
  }
  if (p.sweepDivider == 0 || p.sweepReload) { p.sweepDivider = p.sweepPeriod; p.sweepReload = false; }
  else p.sweepDivider--;
  updateSweepMute(p, ch);
}
static void quarterFrame() {                              // 240Hz: envelopes + triangle linear
  clockEnvelope(pulse[0].volParam, pulse[0].lengthHalt, pulse[0].envStart, pulse[0].envDivider, pulse[0].envDecay);
  clockEnvelope(pulse[1].volParam, pulse[1].lengthHalt, pulse[1].envStart, pulse[1].envDivider, pulse[1].envDecay);
  clockEnvelope(noise.volParam,    noise.lengthHalt,    noise.envStart,    noise.envDivider,    noise.envDecay);
  clockLinear();
  halfTick ^= 1;
  if (halfTick) {                                         // 120Hz: lengths + sweeps
    clockLengths();
    clockSweep(pulse[0], 0);
    clockSweep(pulse[1], 1);
  }
}

static inline void clockLFSR() {
  uint16_t fb = (noise.lfsr ^ (noise.lfsr >> (noise.mode ? 6 : 1))) & 1;
  noise.lfsr = (noise.lfsr >> 1) | (fb << 14);
}

// 65536/n for n = 1..20 -- the shortest noise period clocks the LFSR ~20 times per output sample,
// and the M0+ has no hardware divide, so the box-average below multiplies by a reciprocal instead.
static const uint16_t noiseRecip[21] = {
      0, 65535, 32768, 21845, 16384, 13107, 10923, 9362, 8192, 7282, 6554,
   5958,  5461,  5041,  4681,  4369,  4096,  3855, 3641, 3449, 3277 };

// Linear approximation of the 2A03's non-linear mixer (the usual blargg weights), scaled by
// 100000 so the whole mix stays integer:  pulse_out = 0.00752*(p1+p2),
// tnd_out = 0.00851*triangle + 0.00494*noise.  APU_MIX_MAX is every channel at maximum at once.
#define APU_W_PULSE  752
#define APU_W_TRI    851
#define APU_W_NOISE  494
#define APU_MIX_MAX  (30 * APU_W_PULSE + 15 * APU_W_TRI + 15 * APU_W_NOISE)   // 42735
// Normalise that to a 0..30000 range (leaves head-room for the DC blocker's overshoot).
#define APU_NORM     ((30000 << 16) / APU_MIX_MAX)
// Post-DC-block gain. One pulse channel alone swings about +-4000 before this, so 3 puts a single
// voice at ~36% of full scale and pushes a dense four-voice mix just into the clamp. Raising this
// further squares off loud passages -- turn up VOLUME in the options window instead.
#define APU_GAIN     3

// ---- one 16-bit signed output sample ----
static int16_t genSample16() {
  // Frame sequencer, clocked from emulated time rather than from the sample count (see the header
  // note). Cap the catch-up so a pause / ROM load cannot fire hundreds of envelope clocks at once.
  uint32_t q = apuQuarterTicks;
  int32_t pending = (int32_t)(q - apuQuarterSeen);
  if (pending < 0 || pending > 8) { apuQuarterSeen = q; pending = 0; }
  while (pending-- > 0) { apuQuarterSeen++; quarterFrame(); }

  int pmix = 0;
  for (int i = 0; i < 2; i++) {
    Pulse &p = pulse[i];
    p.phase += p.step;
    if (p.enabled && p.lengthCounter > 0 && p.step && !p.sweepMute) {
      int vol = p.constVol ? p.volParam : p.envDecay;
      if (dutySeq[p.duty][(p.phase >> 29) & 7]) pmix += vol;       // 0..15 per channel
    }
  }
  int tmix = 0;
  tri.phase += tri.step;
  if (tri.enabled && tri.lengthCounter > 0 && tri.linCounter > 0 && tri.step)
    tmix = triSeq[(tri.phase >> 27) & 31];                         // 0..15

  // Noise: box-average the LFSR output over the sample instead of point-sampling it. At short
  // periods the LFSR runs ~20x per sample, and taking a single bit aliases the hiss down into a
  // whistle -- the "not clear" part of percussion.
  noise.phase += noise.step;
  int nClk = 0, nHigh = 0;
  while (noise.phase >= 0x10000) {
    noise.phase -= 0x10000;
    clockLFSR();
    nClk++;
    if (!(noise.lfsr & 1)) nHigh++;
  }
  int nmix = 0;
  if (noise.enabled && noise.lengthCounter > 0) {
    int vol = noise.constVol ? noise.volParam : noise.envDecay;
    if (nClk == 0)       nmix = (noise.lfsr & 1) ? 0 : vol;        // period longer than a sample
    else if (nClk <= 20) nmix = (vol * nHigh * noiseRecip[nClk]) >> 16;
    else                 nmix = vol * nHigh / nClk;                // unreachable in practice
  }

  int32_t acc = pmix * APU_W_PULSE + tmix * APU_W_TRI + nmix * APU_W_NOISE;   // 0..APU_MIX_MAX
  int32_t in  = (acc * (int32_t)APU_NORM) >> 16;                              // 0..~30000

  // One-pole DC blocker, y = x - x[-1] + 0.999*y[-1] (~3.5Hz at 22050Hz). The NES mix is unipolar,
  // so without this the signal sits on a large, program-dependent DC offset -- which the old code
  // tried to cancel with a fixed "- 16" and could not, leaving the waveform lopsided and the
  // usable swing tiny. Removing it properly is most of the volume this channel was missing.
  int32_t y = in - hpPrevIn + ((hpPrevOut * 32735) >> 15);
  if (y >  60000) y =  60000;              // keep the feedback multiply inside int32
  if (y < -60000) y = -60000;
  hpPrevIn = in; hpPrevOut = y;

  if (!sound || OptionsWindow) return 0;   // filter state still advances, so no thump on unmute
  int32_t out = (y * APU_GAIN * (int32_t)volume) >> 8;
  if (out >  32767) out =  32767;
  if (out < -32768) out = -32768;
  return (int16_t)out;
}

// ---- CPU-side register access (called from nes_memory.cpp) ----
static void pulseWrite(int i, int sub, uint8_t val) {
  Pulse &p = pulse[i];
  switch (sub) {
    case 0: p.duty = val >> 6; p.lengthHalt = val & 0x20; p.constVol = val & 0x10; p.volParam = val & 0x0F; break;
    case 1: p.sweepEnable = val & 0x80; p.sweepPeriod = (val >> 4) & 7; p.sweepNegate = val & 8;
            p.sweepShift = val & 7; p.sweepReload = true; updateSweepMute(p, i); break;
    case 2: p.timer = (p.timer & 0x700) | val; p.step = pulseStep(p.timer);
            updateSweepMute(p, i); break;
    case 3: p.timer = (p.timer & 0x0FF) | ((uint16_t)(val & 7) << 8); p.step = pulseStep(p.timer);
            if (p.enabled) p.lengthCounter = lengthTable[val >> 3]; p.envStart = true;
            updateSweepMute(p, i); break;
  }
}

void apuWrite(uint8_t reg, uint8_t val) {
  switch (reg) {
    case 0x00: case 0x01: case 0x02: case 0x03: pulseWrite(0, reg - 0x00, val); break;
    case 0x04: case 0x05: case 0x06: case 0x07: pulseWrite(1, reg - 0x04, val); break;
    case 0x08: tri.control = val & 0x80; tri.linReload = val & 0x7F; break;
    case 0x0A: tri.timer = (tri.timer & 0x700) | val; tri.step = triStep(tri.timer); break;
    case 0x0B: tri.timer = (tri.timer & 0x0FF) | ((uint16_t)(val & 7) << 8); tri.step = triStep(tri.timer);
               if (tri.enabled) tri.lengthCounter = lengthTable[val >> 3]; tri.linReloadFlag = true; break;
    case 0x0C: noise.lengthHalt = val & 0x20; noise.constVol = val & 0x10; noise.volParam = val & 0x0F; break;
    case 0x0E: noise.mode = val & 0x80; noise.period = noisePeriodTbl[val & 0x0F]; noise.step = noiseStep(noise.period); break;
    case 0x0F: if (noise.enabled) noise.lengthCounter = lengthTable[val >> 3]; noise.envStart = true; break;
    case 0x15:
      pulse[0].enabled = val & 1; if (!(val & 1)) pulse[0].lengthCounter = 0;
      pulse[1].enabled = val & 2; if (!(val & 2)) pulse[1].lengthCounter = 0;
      tri.enabled      = val & 4; if (!(val & 4)) tri.lengthCounter = 0;
      noise.enabled    = val & 8; if (!(val & 8)) noise.lengthCounter = 0;
      break;
    case 0x17: frameMode = val & 0x80; frameIRQInhibit = val & 0x40; halfTick = false;
               apuQuarterSeen = apuQuarterTicks;
               if (frameMode) quarterFrame();           // 5-step mode clocks immediately
               break;
    default: break;
  }
}

uint8_t apuReadStatus() {                                // $4015: which length counters are active
  uint8_t s = 0;
  if (pulse[0].lengthCounter) s |= 1;
  if (pulse[1].lengthCounter) s |= 2;
  if (tri.lengthCounter)      s |= 4;
  if (noise.lengthCounter)    s |= 8;
  return s;
}

static void apuTask(void *) {
#if BOARD_AUDIO_DAC
  // CYD only: the ESP32's internal DAC is 8-bit, so quantise here and nowhere else.
  uint16_t buf[128];                 // on the task stack (not static BSS — dram0_0_seg is tight)
  size_t wrote;
  while (running) {
    for (int i = 0; i < 128; i++) {
      int dac = 128 + (genSample16() >> 8);
      buf[i] = (uint16_t)((dac < 0 ? 0 : dac > 255 ? 255 : dac) << 8);   // DAC uses the high byte
    }
    i2s_write(I2S_NUM_0, buf, sizeof(buf), &wrote, portMAX_DELAY);
  }
#else
  // Every other board has a real amp (I2S on the S3/P4, PWM on the PicoCalc) and takes signed
  // 16-bit directly. The old path squeezed the mix through 8 bits first, which on a quiet signal
  // threw away four bits and turned soft passages into audible steps.
  int16_t buf[128];
  while (running) {
    for (int i = 0; i < 128; i++) buf[i] = genSample16();
    ampWriteMono(buf, 128);
  }
#endif
  vTaskDelete(NULL);
}

static void apuSetup() {
  memset(&pulse, 0, sizeof(pulse));
  memset(&tri,   0, sizeof(tri));
  memset(&noise, 0, sizeof(noise));
  noise.lfsr = 1;                                        // must be non-zero
  halfTick = false;
  apuQuarterSeen = apuQuarterTicks;
  hpPrevIn = hpPrevOut = 0;
  updateSweepMute(pulse[0], 0);
  updateSweepMute(pulse[1], 1);

#if BOARD_AUDIO_DAC
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  cfg.sample_rate = APU_FS;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 128;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
    printLog("APU: I2S install failed (no sound)");
    return;
  }
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);            // left = DAC2 = GPIO26 (CYD speaker)
  i2s_zero_dma_buffer(I2S_NUM_0);
  xTaskCreatePinnedToCore(apuTask, "apuTask", 4096, NULL, 2, NULL, 0);   // core 0
  printLog("APU: pulse x2 + triangle + noise on (I2S DAC GPIO26)");
#else
  // ESP32-S3: external I2S amp (no internal DAC).
  ampBegin(APU_FS);
#if defined(BOARD_PICOCALC)
  // Static stack -- a 4KB task create that fails is FATAL on this board, and this is the
  // last thing the boot asks of the heap. See picocalcStartAudioTask (audio_picocalc.cpp).
  picocalcStartAudioTask(apuTask, "apuTask", 2);
#else
  xTaskCreatePinnedToCore(apuTask, "apuTask", 4096, NULL, 2, NULL, 0);   // core 0
#endif
  printLog("APU: pulse x2 + triangle + noise on (I2S amp)");
#endif
}

} // namespace nes

// C-linkage entry point (called from the platform dispatch, like sidSetup for the C64).
void nesApuSetup() { nes::apuSetup(); }
