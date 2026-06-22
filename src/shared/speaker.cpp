#include "../../emu.h"

#if BOARD_AUDIO_DAC
#include "driver/dac.h"

// The Apple II speaker is a 1-bit toggle at $C030. We output it through the ESP32
// DAC on GPIO26 (DAC channel 2) instead of a plain digitalWrite, so the square-wave
// amplitude scales with `volume` (0x00..0xF0) and `sound` can mute it. This is what
// makes the Settings volume slider / mute actually do something.
// NOTE: SPEAKER_PIN must be GPIO26 (DAC_CHANNEL_2) for this path.
void speakerSetup() {
    dac_output_enable(DAC_CHANNEL_2);
    dac_output_voltage(DAC_CHANNEL_2, 0);
}

void speakerToggle() {
  speaker_state = !speaker_state;
  // High half of the wave outputs `volume`; low half outputs 0. Muted -> always 0.
  dac_output_voltage(DAC_CHANNEL_2, (sound && speaker_state) ? volume : 0);
}

#else
// ESP32-S3 (no internal DAC): drive the Apple 1-bit speaker through the I2S amp. The CPU toggles
// speaker_state at $C030; we turn that into an audio square wave.
//
// Earlier this sampled speaker_state inside a tight i2s_write loop. That couples the SAMPLING
// instant to task scheduling: on core 0 the loop starved the render task (display froze); on
// core 1 it fought the emulator CPU + USB host, so the sampling jittered and the tone came out
// distorted. Fix: a HARDWARE TIMER samples speaker_state at a rock-steady rate (no scheduler
// jitter -> clean edges), and a lightweight task just shovels the filled buffers into the I2S
// DMA (which may be jittery without affecting the captured waveform).
#define SPK_FS    44100     // sampling == I2S output rate; higher rate = less aliasing on fast clicks
#define SPK_RING  2048      // sample ring (power of 2), ~46ms of slack to absorb feed jitter/drift
static int16_t spkRing[SPK_RING];
static volatile uint32_t spkW = 0, spkR = 0;   // free-running write/read counts (mask to index)
static hw_timer_t *spkTimer = nullptr;

// Single-producer (this ISR) / single-consumer (the task), both pinned to core 0 — the ISR simply
// preempts the task, so plain volatile indices are race-free (no cross-core ordering to worry about).
// Runs from IRAM at SPK_FS; only touches DRAM globals + a DRAM ring (no flash access). The ring holds
// the RAW 1-bit level (+/- amp); the consumer shapes it. amp is <<6 (not <<7) to leave headroom for
// the DC-blocker's edge overshoot below.
static void IRAM_ATTR spkISR()
{
  uint32_t w = spkW;
  if ((uint32_t)(w - spkR) < SPK_RING) {               // ring not full
    int amp = sound ? ((int)volume << 4) : 0;          // volume 0..0xF0 -> 0..~3840 (kept low: a full
                                                       // square is loud; the slider scales from here)
    spkRing[w & (SPK_RING - 1)] = speaker_state ? (int16_t)amp : (int16_t)(-amp);
    spkW = w + 1;
  }
  // else: consumer briefly behind -> drop this sample (the timer runs a hair faster than the I2S,
  // so the ring tends to stay full and we drop rare excess samples rather than ever underrunning).
}

static void speakerTask(void *)
{
  // Arm the sampling timer from THIS task so its interrupt is allocated on this task's core (core 0),
  // keeping the ISR off core 1 where the USB host lives. 40MHz tick / 907 = ~44101Hz, a near exact
  // match to the I2S rate so the ring barely drifts.
  spkTimer = timerBegin(0, 2, true);                   // 80MHz / 2 = 40MHz tick
  timerAttachInterrupt(spkTimer, &spkISR, true);
  timerAlarmWrite(spkTimer, 40000000UL / SPK_FS, true);// ~44100Hz
  timerAlarmEnable(spkTimer);

  // Shape the raw 1-bit square into something closer to a physical speaker:
  //  * DC blocker (one-pole high-pass, ~10Hz): the cone can't hold a DC offset, so a held level
  //    decays back to centre instead of clicking/popping between sounds.
  //  * gentle low-pass (~5kHz): rounds off the harshest square-wave harmonics (the buzz).
  float dcInPrev = 0.0f, dcOutPrev = 0.0f, lp = 0.0f;
  int16_t lastOut = 0;
  int16_t chunk[128];
  while (running) {
    for (int k = 0; k < 128; k++) {                    // ALWAYS fill a full block -> never starve DMA
      if (spkR != spkW) {                              // sample available
        float x  = (float)spkRing[spkR & (SPK_RING - 1)];
        float hp = x - dcInPrev + 0.999f * dcOutPrev;  // DC blocker
        dcInPrev = x; dcOutPrev = hp;
        lp += 0.5f * (hp - lp);                        // one-pole low-pass
        int v = (int)lp;
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        lastOut = (int16_t)v;
        spkR++;
      }
      // Ring momentarily empty (the I2S clock drifts a hair from the sampling timer): HOLD the last
      // value instead of stalling. The DMA stays fed -> no underrun "pop"; held briefly it's silent
      // anyway (the DC blocker has already decayed an idle level toward 0).
      chunk[k] = lastOut;
    }
    ampWriteMono(chunk, 128);                          // blocks on I2S DMA -> paces the consumer
  }
  vTaskDelete(NULL);
}

void speakerSetup()
{
  ampBegin(SPK_FS);
  // Core 0: the task only blocks on the I2S DMA (no spin), so it won't starve the render loop the
  // way the old tight feed loop did; the timer ISR (armed inside the task) also lands on core 0.
  xTaskCreatePinnedToCore(speakerTask, "speaker", 2048, NULL, 2, NULL, 0);
}
void speakerToggle() { speaker_state = !speaker_state; }
#endif
