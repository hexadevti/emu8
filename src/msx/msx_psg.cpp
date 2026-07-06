// msx_psg.cpp - AY-3-8910 PSG for the MSX1 core (ports $A0 addr / $A1 data-write / $A2 data-read).
// 3 square-tone channels + a shared 17-bit noise LFSR + a 16-step envelope generator. The synthesis
// (psgGenSample) is Arduino-free so it links into the host harness too; the I2S output task lives in
// msx_audio.cpp (device only). Register 14 is the joystick input (PSG port A). Sample rate 22050 Hz.

#include "msx.h"
#include <string.h>

namespace msx {

static const double   AY_CLK = 1789772.5;     // MSX PSG clock = 3.579545 MHz / 2
static const int      PSG_FS = 22050;

static uint8_t psgReg[16];
static uint8_t psgAddr;
static uint8_t g_joy = 0xFF;                   // active-low joystick lines (PSG port A / R14)

// synthesis state (advanced once per output sample by psgGenSample)
static uint32_t tonePhase[3], toneStep[3];
static uint32_t noisePhase, noiseStep, noiseLfsr = 1;
static uint32_t envPhase, envStep;
static int      envLevel, envDir;
static bool     envHold_, envAlt_, envCont_, envAtt_, envHolding;

// AY logarithmic volume, scaled so three channels sum within an 8-bit DAC swing (~85 each)
static const uint8_t AYVOL[16] = {0,1,2,4,6,9,13,18,24,31,39,48,58,69,80,85};

static void recalcSteps() {
  for (int i = 0; i < 3; i++) {
    int tp = psgReg[i * 2] | ((psgReg[i * 2 + 1] & 0x0F) << 8);
    if (tp == 0) tp = 1;
    toneStep[i] = (uint32_t)((AY_CLK / (16.0 * tp)) * 4294967296.0 / PSG_FS);
  }
  int np = psgReg[6] & 0x1F; if (np == 0) np = 1;
  noiseStep = (uint32_t)((AY_CLK / (16.0 * np)) * 4294967296.0 / PSG_FS);
  int ep = psgReg[11] | (psgReg[12] << 8); if (ep == 0) ep = 1;
  envStep = (uint32_t)((AY_CLK / (256.0 * ep)) * 4294967296.0 / PSG_FS);
}
static void envReset() {
  uint8_t s = psgReg[13] & 0x0F;
  envCont_ = s & 8; envAtt_ = s & 4; envAlt_ = s & 2; envHold_ = s & 1;
  envDir = envAtt_ ? +1 : -1;
  envLevel = envAtt_ ? 0 : 15;
  envHolding = false; envPhase = 0;
}
static void clockEnv() {
  if (envHolding) return;
  envLevel += envDir;
  if (envLevel < 0 || envLevel > 15) {
    if (!envCont_) { envLevel = 0; envHolding = true; return; }
    if (envHold_)  { envLevel = (envLevel < 0) ? 0 : 15; envHolding = true; return; }
    if (envAlt_)   { envDir = -envDir; envLevel += 2 * envDir; }   // reverse back into range
    else           { envLevel = (envLevel < 0) ? 15 : 0; }         // repeat the ramp
    if (envLevel < 0) envLevel = 0; if (envLevel > 15) envLevel = 15;
  }
}

void psgReset() {
  memset(psgReg, 0, sizeof(psgReg)); psgAddr = 0; psgReg[14] = 0xFF; psgReg[15] = 0xFF;
  for (int i = 0; i < 3; i++) { tonePhase[i] = 0; }
  noisePhase = 0; noiseLfsr = 1; envPhase = 0; envLevel = 0; envDir = -1; envHolding = true;
  recalcSteps();
}
void psgWriteAddr(uint8_t a) { psgAddr = a & 0x0F; }
void psgWriteData(uint8_t v) {
  if (psgAddr == 14) return;                   // port A is input on MSX (joystick); ignore writes
  psgReg[psgAddr] = v;
  if (psgAddr <= 12) recalcSteps();
  else if (psgAddr == 13) envReset();
}
uint8_t psgReadData() {
  if (psgAddr == 14) return g_joy;
  return psgReg[psgAddr];
}
void setJoystick(uint8_t mask) { g_joy = mask; }

// One output sample (0..255, 128 = silence). masterVol = app volume (0..255); mute silences it.
int psgGenSample(int masterVol, bool mute) {
  uint32_t oe = envPhase; envPhase += envStep; if (envPhase < oe) clockEnv();
  uint32_t on = noisePhase; noisePhase += noiseStep;
  if (noisePhase < on) { uint32_t b = (noiseLfsr ^ (noiseLfsr >> 3)) & 1; noiseLfsr = (noiseLfsr >> 1) | (b << 16); }
  int noiseOut = noiseLfsr & 1;
  uint8_t mix = psgReg[7];
  int acc = 0;
  for (int i = 0; i < 3; i++) {
    tonePhase[i] += toneStep[i];
    int toneBit = (mix & (1 << i))       ? 1 : (int)(tonePhase[i] >> 31);   // tone disabled -> 1
    int nBit    = (mix & (1 << (i + 3))) ? 1 : noiseOut;                     // noise disabled -> 1
    int ampReg = psgReg[8 + i];
    int amp = (ampReg & 0x10) ? envLevel : (ampReg & 0x0F);
    int half = AYVOL[amp] >> 1;
    acc += (toneBit & nBit) ? half : -half;     // AC swing around 0
  }
  int s = acc * masterVol / 255;
  if (mute) s = 0;
  int dac = 128 + s;
  return dac < 0 ? 0 : (dac > 255 ? 255 : dac);
}

} // namespace msx
