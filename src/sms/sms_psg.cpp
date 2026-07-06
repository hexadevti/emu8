// sms_psg.cpp - Texas Instruments SN76489 PSG for the SMS core. Unlike the MSX AY-3-8910 there is a
// SINGLE write port and no envelope generator: 3 square-tone channels + 1 noise channel, each with a
// 4-bit attenuation. Writes use a latch/data protocol:
//   byte b7=1  LATCH/DATA: b6-5 = channel(0-2 tone, 3 noise), b4 = type(0 tone/noise, 1 volume),
//              b3-0 = data (tone period low 4 bits, or volume, or 3-bit noise control)
//   byte b7=0  DATA: b5-0 = tone period high 6 bits (or b3-0 = volume / noise control)
// Synthesis (psgGenSample) is Arduino-free so it links into the harness; the I2S task is sms_audio.cpp.

#include "sms.h"

namespace sms {

static const double PSG_CLK = 3579545.0 / 16.0;   // SN76489 internal tone clock (master/16)
static const int    PSG_FS  = 22050;

static uint16_t tonePeriod[3];   // 10-bit
static uint8_t  vol[4];          // 4-bit attenuation (0 = loud, 15 = silent) for tones 0-2 + noise
static uint8_t  noiseCtrl;       // b2 = white(1)/periodic(0), b1-0 = shift rate
static int      latchCh, latchType;

// synthesis state
static uint32_t tonePhase[3], toneStep[3];
static uint32_t noisePhase, noiseStep;
static uint16_t lfsr;
static int      toneOut[3];      // current square level (+1/-1 toggling)

// SN76489 attenuation -> linear amplitude (2 dB/step), scaled so 4 channels fit an 8-bit DAC swing.
static const int SNVOL[16] = {32, 25, 20, 16, 13, 10, 8, 6, 5, 4, 3, 3, 2, 2, 1, 0};

static void recalcTone(int i) {
  int p = tonePeriod[i] ? tonePeriod[i] : 1;
  toneStep[i] = (uint32_t)((PSG_CLK / (double)p) * 4294967296.0 / (2.0 * PSG_FS));
}
static void recalcNoise() {
  int rate = noiseCtrl & 3;
  int p = (rate == 3) ? (tonePeriod[2] ? tonePeriod[2] : 1) : (0x10 << rate);  // 0x10/0x20/0x40 or tone2
  noiseStep = (uint32_t)((PSG_CLK / (double)p) * 4294967296.0 / (2.0 * PSG_FS));
}

void psgReset() {
  for (int i = 0; i < 3; i++) { tonePeriod[i] = 0; tonePhase[i] = 0; toneOut[i] = 1; }
  for (int i = 0; i < 4; i++) vol[i] = 15;          // all channels silent at power-on
  noiseCtrl = 0; latchCh = 0; latchType = 0;
  noisePhase = 0; lfsr = 0x8000;
  for (int i = 0; i < 3; i++) recalcTone(i);
  recalcNoise();
}

void psgWrite(uint8_t v) {
  if (v & 0x80) {                                   // LATCH/DATA byte
    latchCh = (v >> 5) & 3;
    latchType = (v >> 4) & 1;
    if (latchType) { vol[latchCh] = v & 0x0F; }
    else if (latchCh == 3) { noiseCtrl = v & 0x07; lfsr = 0x8000; recalcNoise(); }
    else { tonePeriod[latchCh] = (uint16_t)((tonePeriod[latchCh] & 0x3F0) | (v & 0x0F)); recalcTone(latchCh); if ((noiseCtrl & 3) == 3) recalcNoise(); }
  } else {                                          // DATA byte (high bits of the latched item)
    if (latchType) { vol[latchCh] = v & 0x0F; }
    else if (latchCh == 3) { noiseCtrl = v & 0x07; lfsr = 0x8000; recalcNoise(); }
    else { tonePeriod[latchCh] = (uint16_t)((tonePeriod[latchCh] & 0x00F) | ((v & 0x3F) << 4)); recalcTone(latchCh); if ((noiseCtrl & 3) == 3) recalcNoise(); }
  }
}

// One output sample (0..255, 128 = silence). masterVol = app volume (0..255); mute silences it.
int psgGenSample(int masterVol, bool mute) {
  int acc = 0;
  for (int i = 0; i < 3; i++) {
    uint32_t o = tonePhase[i]; tonePhase[i] += toneStep[i];
    if (tonePhase[i] < o) toneOut[i] = -toneOut[i];   // half-period wrap toggles the square
    int level = (tonePeriod[i] <= 1) ? 1 : toneOut[i]; // period 0/1 -> DC (ultrasonic)
    acc += level * SNVOL[vol[i]];
  }
  uint32_t on = noisePhase; noisePhase += noiseStep;
  if (noisePhase < on) {                              // noise shift clock
    uint16_t taps = (noiseCtrl & 0x04) ? (lfsr & 0x0009) : (lfsr & 0x0001);  // white XORs bits 0,3
    int fb = 0; for (uint16_t t = taps; t; t >>= 1) fb ^= (t & 1);           // parity of the taps
    lfsr = (uint16_t)((lfsr >> 1) | (fb << 15));
  }
  acc += ((lfsr & 1) ? 1 : -1) * SNVOL[vol[3]];

  int s = acc * masterVol / 255;
  if (mute) s = 0;
  int dac = 128 + s;
  return dac < 0 ? 0 : (dac > 255 ? 255 : dac);
}

} // namespace sms
