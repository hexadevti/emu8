#include "../../emu.h"
#include "nes.h"
#include "driver/i2s.h"

// NES APU (2A03 audio) -> ESP32 internal DAC (GPIO26) via I2S DMA, mirroring the C64 SID path.
//   * 2 pulse (square, 4 duties, sweep, envelope, length), triangle (linear+length counter),
//     noise (LFSR, envelope, length). DMC (sample channel) is NOT emulated.
//   * The frame sequencer (envelope/linear at 240Hz, length/sweep at 120Hz) is clocked inside the
//     audio task from the sample count, so the CPU only writes registers (like the SID).
//   * Channel timers come from the period registers; per-sample phase accumulators synthesise the
//     waveforms directly (register-based synthesis, like the SID — not cycle-accurate).
//   * Output gated by the app `sound` toggle and scaled by `volume`. Tune by ear.
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
static uint32_t frameAcc;             // accumulates toward 240Hz quarter frames
static bool     halfTick;

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
static void clockSweep(Pulse &p, int ch) {
  if (p.sweepDivider == 0 && p.sweepEnable && p.sweepShift > 0) {
    int change = p.timer >> p.sweepShift;
    if (p.sweepNegate) change = -change - (ch == 0 ? 1 : 0);   // pulse1 ones', pulse2 twos' comp
    int target = (int)p.timer + change;
    if (target >= 8 && target <= 0x7FF) { p.timer = (uint16_t)target; p.step = pulseStep(p.timer); }
  }
  if (p.sweepDivider == 0 || p.sweepReload) { p.sweepDivider = p.sweepPeriod; p.sweepReload = false; }
  else p.sweepDivider--;
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

// ---- one 8-bit DAC sample (centered on 128, like the SID) ----
static int genSample() {
  frameAcc += 240;
  if (frameAcc >= APU_FS) { frameAcc -= APU_FS; quarterFrame(); }

  int mix = 0;
  for (int i = 0; i < 2; i++) {
    Pulse &p = pulse[i];
    p.phase += p.step;
    if (p.enabled && p.lengthCounter > 0 && p.step) {
      int vol = p.constVol ? p.volParam : p.envDecay;
      if (dutySeq[p.duty][(p.phase >> 29) & 7]) mix += vol;     // 0..15
    }
  }
  tri.phase += tri.step;
  if (tri.enabled && tri.lengthCounter > 0 && tri.linCounter > 0 && tri.step)
    mix += triSeq[(tri.phase >> 27) & 31];                       // 0..15
  noise.phase += noise.step;
  while (noise.phase >= 0x10000) { noise.phase -= 0x10000; clockLFSR(); }
  if (noise.enabled && noise.lengthCounter > 0) {
    int vol = noise.constVol ? noise.volParam : noise.envDecay;
    if (!(noise.lfsr & 1)) mix += vol;                           // 0..15
  }

  int s = (mix - 16) * (int)volume / 255;     // rough center; app master volume
  if (!sound || OptionsWindow) s = 0;         // mute toggle, and silence while the menu is open
  int dac = 128 + s * 3;                       // scale to the 8-bit DAC range
  if (dac < 0)   dac = 0;
  if (dac > 255) dac = 255;
  return dac;
}

// ---- CPU-side register access (called from nes_memory.cpp) ----
static void pulseWrite(int i, int sub, uint8_t val) {
  Pulse &p = pulse[i];
  switch (sub) {
    case 0: p.duty = val >> 6; p.lengthHalt = val & 0x20; p.constVol = val & 0x10; p.volParam = val & 0x0F; break;
    case 1: p.sweepEnable = val & 0x80; p.sweepPeriod = (val >> 4) & 7; p.sweepNegate = val & 8;
            p.sweepShift = val & 7; p.sweepReload = true; break;
    case 2: p.timer = (p.timer & 0x700) | val; p.step = pulseStep(p.timer); break;
    case 3: p.timer = (p.timer & 0x0FF) | ((uint16_t)(val & 7) << 8); p.step = pulseStep(p.timer);
            if (p.enabled) p.lengthCounter = lengthTable[val >> 3]; p.envStart = true; break;
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
    case 0x17: frameMode = val & 0x80; frameIRQInhibit = val & 0x40; frameAcc = 0; halfTick = false;
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
  uint16_t buf[128];                 // on the task stack (not static BSS — dram0_0_seg is tight)
  size_t wrote;
  while (running) {
    for (int i = 0; i < 128; i++) buf[i] = (uint16_t)(genSample() << 8);   // DAC uses high byte
#if BOARD_AUDIO_DAC
    i2s_write(I2S_NUM_0, buf, sizeof(buf), &wrote, portMAX_DELAY);
#else
    ampWriteDac8(buf, 128);                                                // S3: 8-bit DAC -> I2S amp
#endif
  }
  vTaskDelete(NULL);
}

static void apuSetup() {
  memset(&pulse, 0, sizeof(pulse));
  memset(&tri,   0, sizeof(tri));
  memset(&noise, 0, sizeof(noise));
  noise.lfsr = 1;                                        // must be non-zero
  frameAcc = 0; halfTick = false;

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
  xTaskCreatePinnedToCore(apuTask, "apuTask", 4096, NULL, 2, NULL, 0);   // core 0
  printLog("APU: pulse x2 + triangle + noise on (I2S amp)");
#endif
}

} // namespace nes

// C-linkage entry point (called from the platform dispatch, like sidSetup for the C64).
void nesApuSetup() { nes::apuSetup(); }
