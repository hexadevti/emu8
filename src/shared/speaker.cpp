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
// Sampling rate. This MUST equal the rate the output stage consumes at, or the ring drifts and
// drops/holds samples forever -- on a 1-bit square wave that is plainly audible as a rasp.
//
// The ESP32 path derives its rate from a 40 MHz timer: 40e6/907 = 44101 Hz against a 44100 Hz
// I2S clock, a 0.002% mismatch that the ring absorbs. The PicoCalc has no such divider --
// add_repeating_timer_us() takes WHOLE microseconds and 44100 Hz is 22.68us, which is not
// representable. 22us is 45454.5 Hz, i.e. 3% FAST: the ring then sits permanently full and threw
// away roughly one sample in 33, continuously. So run the whole chain at the rate the timer can
// actually produce and let PWMAudio's fractional DMA pacer match it, instead of asking for a rate
// the hardware cannot make. (1000000/22 = 45454.54; PWMAudio's pacer resolves the fraction.)
#if defined(BOARD_PICOCALC)
#define SPK_US    22        // add_repeating_timer_us period -- the only knob that exists here
#define SPK_FS    (1000000 / SPK_US)   // 45454 Hz, and the output is told the SAME number
#else
#define SPK_FS    44100     // sampling == I2S output rate; higher rate = less aliasing on fast clicks
#endif
// Sample ring (power of 2). Was 2048 on the guess that it needed ~46ms of slack; instrumenting the
// consumer showed occupancy actually sits at 326..397 and never moved outside it, so 1024 (~22ms)
// keeps a 2.5x margin over the worst observed and hands 2K back to the heap -- which the Apple IIe
// map genuinely needs (see A2_IIE_RESERVE in src/apple2/memory.cpp).
#define SPK_RING  1024
static int16_t spkRing[SPK_RING];
static volatile uint32_t spkW = 0, spkR = 0;   // free-running write/read counts (mask to index)

// ---- the $C030 toggle timeline ------------------------------------------------------------
// The ISR below used to read the LIVE speaker_state once per sample. That is right for tones and
// wrong for everything else, and it is why noise and digitised speech came out as buzz:
//
//   * A tone toggles every few hundred us, so a 22us sampler sees every flip.
//   * A noise routine (California Games) toggles at irregular intervals, many of them ~10 CPU
//     cycles apart -- shorter than one sample. Any PAIR of flips landing between two sample
//     instants is invisible: the sampler reads the same level twice and the click is simply gone.
//     What survives is the subset that happens to straddle a sample boundary, which is correlated
//     with the sample rate, so the result aliases into tones instead of hiss.
//   * Digitised speech (Ghostbusters) is worse still: it is 1-bit PWM at tens of kHz, i.e. the
//     amplitude is carried ENTIRELY in the ratio of high to low time inside each period. Point
//     sampling throws that ratio away and keeps only a coin flip.
//
// So the CPU now records the micros() timestamp of every flip here, and the ISR replays the
// timeline and FILTERS IT IN CONTINUOUS TIME -- see the note on SPK_K below for why averaging the
// window was not enough. This is the same event-timeline fix src/desktop/audio_sdl.cpp already
// carries, with the reconstruction filter on top.
// Flip timestamps (power of 2). These are 16-BIT microsecond stamps, not 32-bit: the queue only
// ever spans the replay lag plus the servo error, and the resync threshold below bounds that to
// ~26ms, so the top half of a 32-bit stamp carried no information. The ISR reconstructs the
// absolute time against the window it is rendering. Same 4KB, twice the depth -- which the queue
// needed, having been measured at 847 of 1024 entries during a dense noise burst.
#define SPK_TOG_RING 2048
// How far behind the GUEST clock the replay runs -- the buffer that absorbs the host stalling.
// Measurement, not guesswork: instrumenting the instantaneous 6502 speed over 1ms windows gave
// jit=63..107, i.e. the emulator spends whole milliseconds at 63% of real time. A 1ms stall at
// that rate puts the guest 370us behind, so anything under about 400us guarantees the replay
// outruns the flips and reads a window whose edges have not been recorded yet. 250us did exactly
// that. 3ms was still not enough, for a reason the lag itself cannot fix: the servo needs room to
// be WRONG in, and its error was measured swinging to -3000us -- consuming the entire lag, so the
// replay ran level with live guest time and starved. 6ms gives the servo somewhere to go.
//
// This also sizes the flip queue, which is why it is not simply made huge: at 56k flips/s it costs
// 336 entries steady-state, and a burst pushes the instantaneous rate well above that average. The
// 16-bit stamps above buy the 2048 entries this needs without spending any more RAM, which matters
// because the Apple IIe map is only ~2.5KB from not fitting (A2_IIE_RESERVE, src/apple2/memory.cpp).
#define SPK_LAG_US   6000
// Sample period as 16.16 microseconds: 22.0us on the PicoCalc, 22.675us on the I2S boards. Kept as
// a fixed-point step rather than a divide so the timeline advances at exactly SPK_FS on both.
#define SPK_PERIOD_Q16 ((uint32_t)(((uint64_t)1000000 << 16) / SPK_FS))
static volatile uint16_t spkTogTs[SPK_TOG_RING];
static volatile uint32_t spkTogW = 0, spkTogR = 0;
static volatile bool     spkTogLost = false;   // producer had nowhere to put a flip; ISR resyncs

// ---- the reconstruction filter ------------------------------------------------------------
// Replaying the timeline and averaging each 22us window (what this used to do) is a box filter,
// and a box filter is a bad anti-alias filter. Measuring California Games settled it: at its
// loudest it puts 41195 of 56247 flips per second in the 8-16us bucket, i.e. a ~42kHz carrier,
// while the output runs at 45454Hz and can only represent 22.7kHz. A 22us box passes 42kHz at
// -21.8dB and the sampler then folds it down to 3.5kHz -- so the buzz was not detail being lost,
// it was an ultrasonic carrier being manufactured into an audible tone. Ghostbusters' speech sits
// lower and mostly escaped it, which is why one game seemed better and the other got worse.
//
// A real Apple II speaker never has this problem: the cone is a mechanical low-pass, so the
// carrier is gone before anything can alias. Model that instead. The filter is advanced ONCE PER
// EDGE rather than once per sample, so it is solving the RC response in continuous time at 1us
// resolution and its cutoff is completely independent of the output rate -- by the time the
// sampler sees the signal there is nothing above Nyquist left to fold. Two poles at ~7kHz: -0.2dB
// at 1kHz, -1.5dB at 3kHz, and -31dB at the 42kHz carrier that was the whole problem.
//
// SPK_K[dt] = exp(-dt / 22.7us) in Q15. dt is a gap between two edges CLIPPED TO ONE WINDOW, so it
// is never more than 23us and the table never needs more than 32 entries. Q15 keeps the whole
// thing in 32-bit integer arithmetic: this runs 45454 times a second on an FPU-less Cortex-M0+.
static const uint16_t SPK_K[32] = {
  32768, 31356, 30004, 28711, 27474, 26290, 25157, 24073,

  23035, 22043, 21093, 20184, 19314, 18481, 17685, 16923,

  16193, 15496, 14828, 14189, 13577, 12992, 12432, 11896,

  11384, 10893, 10424,  9974,  9545,  9133,  8740,  8363,
};
#define SPK_UNIT 1024                     // filter full scale; the volume multiply happens at the end

// Reading the clock is now on the 6502's hot path -- every $C030 access pays for it, and a tight
// noise loop hits $C030 about every 9 emulated cycles. micros() is an out-of-line call into flash;
// the raw timer register is one APB load, which is the difference between costing the emulated CPU
// a few percent of its (already thin) real-time budget and costing it almost nothing.
#if defined(BOARD_PICOCALC)
#include <hardware/timer.h>
#define SPK_NOW_US() (timer_hw->timerawl)
#else
#define SPK_NOW_US() ((uint32_t)micros())
#endif

// If this ever needs debugging again, the counters that found the last four bugs were: flips/s,
// timer fires/s, samples out/s, held samples/s, min/max ring occupancy, max flip-queue depth, the
// servo error, and queue overflows -- printed once a second from the consumer task alongside
// appleMeasuredMhz. The pairing that mattered was flips/s against MHz: it is what exposed the
// emulator running slower the more sound it was making. They are removed rather than left behind
// because two of them sat on the 6502's hot path.

#if defined(BOARD_PICOCALC)
#include <pico/time.h>
static repeating_timer_t spkTimer;   // pico-sdk alarm-pool timer (no hw_timer_t on RP2350)
#else
static hw_timer_t *spkTimer = nullptr;
#endif

// Single-producer (this ISR) / single-consumer (the task), both pinned to core 0 — the ISR simply
// preempts the task, so plain volatile indices are race-free (no cross-core ordering to worry about).
// Runs from IRAM at SPK_FS; only touches DRAM globals + a DRAM ring (no flash access). The ring
// holds the already-reconstructed level (+/- amp), band-limited by spkStep() below; the consumer
// only blocks DC and takes the last bit of top off. amp leaves headroom for the DC-blocker's edge
// overshoot.
// Advance both poles across `dt` microseconds held at `level`. Each pole relaxes toward its input
// by exp(-dt/tau), which is one table lookup and one multiply-shift -- so the cost is per EDGE, not
// per microsecond, and a window with no edges costs exactly one of these. dt is bounded by the
// window (23us), but clamp anyway: a corrupt timestamp must not index past the table, and
// saturating to SPK_K[31] only means "decayed a bit less than it should have".
//
// The state is at file scope rather than inside the ISR only so this can be a plain function; it is
// touched from nowhere else.
static bool    spkLevel = false;                  // replayed speaker level, lags the live state
static int32_t spkY1 = 0, spkY2 = 0;              // the two poles, in SPK_UNIT

static inline void IRAM_ATTR spkStep(uint32_t dt)
{
  int32_t k = (int32_t)SPK_K[dt < 32 ? dt : 31];
  int32_t L = spkLevel ? SPK_UNIT : -SPK_UNIT;
  spkY1 = L     + (((spkY1 - L)     * k) >> 15);
  spkY2 = spkY1 + (((spkY2 - spkY1) * k) >> 15);
}

static void IRAM_ATTR spkISR()
{
  // The sample is computed FIRST and only stored at the end, if the ring has room. It used to be
  // the other way round -- the whole body sat inside the ring-full test -- which quietly meant that
  // dropping an output sample ALSO stalled the replay clock below and stopped draining the flip
  // queue. The ring "tends to stay full" by design (see the note at the bottom of this function),
  // so that was not a rare case: the queue backed up until it overflowed, and an overflow is not a
  // harmless polarity flip -- losing one edge makes a window that should average to silence read
  // full scale instead. The denser the toggling the worse it got, which is precisely what a noise
  // loop does and why speech survived it while noise did not.
  int amp = sound ? ((int)volume << 4) : 0;            // volume 0..0xF0 -> 0..~3840 (kept low: a full
                                                      // square is loud; the slider scales from here)
  int16_t s;
  if (currentPlatform == PLATFORM_PCXT) {
    // PC-speaker: synthesize a square wave at the PIT-ch2 frequency (gated by port 0x61). A phase
    // accumulator advances by `freq` each 44100Hz sample; one full cycle per SPK_FS, high in the
    // first half. Silent when off (the DC blocker decays the held level -> no pop).
    static uint32_t acc = 0;
    int f = g_pcSpkFreq;
    if (g_pcSpkOn && f > 0) {
      acc += (uint32_t)f;
      if (acc >= (uint32_t)SPK_FS) acc -= (uint32_t)SPK_FS;
      s = (acc < (uint32_t)(SPK_FS / 2)) ? (int16_t)amp : (int16_t)(-amp);
    } else {
      s = 0;
    }
  } else {
    // Replay the flip timeline over THIS sample's window, advancing the reconstruction filter
    // across every segment between edges (see the note on SPK_K above).
    static uint32_t playUs = 0, playFrac = 0;
    static bool anchored = false;
    static int32_t  rateQ16 = (int32_t)SPK_PERIOD_Q16;  // measured guest rate, Q16 us per sample
    static uint32_t gPrev = 0;                          // guest clock at the previous sample
    if (spkTogLost) {
      // The producer could not record a flip. Nothing here can reconstruct the edge it lost, so
      // take one glitch now instead of an inverted, wrongly-scaled signal from here on: drop the
      // backlog and resync the replayed level to the live state.
      spkTogLost = false;
      spkTogR = spkTogW;
      spkLevel = speaker_state;
      anchored = false;
    }
    // Track the GUEST clock, not real time. The flips are stamped in guest microseconds now, so
    // the window that replays them has to advance in guest microseconds too -- and the guest runs
    // at whatever rate the host manages (0.92MHz here), which is neither 1.02MHz nor constant.
    //
    // A plain re-anchor cannot do this: it would snap the clock whenever the two rates diverged,
    // and a snap drops or repeats a slab of the waveform. So the SAMPLE STEP is adjusted instead
    // and the replay tracks the guest smoothly -- see the feed-forward note below for how the rate
    // is arrived at, and for the two ways of getting it wrong that came first.
    uint32_t gnow = appleGuestUs;
    if (!gnow) gnow = SPK_NOW_US();      // no Apple II pacing (other cores): old behaviour
    int32_t err = (int32_t)(gnow - SPK_LAG_US - playUs);
    if (!anchored || err > 20000 || err < -20000) {
      // Started, or the guest stopped for 20ms (a disk seek, a menu, a platform switch) and the
      // servo cannot walk that back at its designed rate. Nothing to track yet, so snap and accept
      // one glitch. NOT a re-anchor of the pacer: appleGuestUs free-runs and never jumps, which is
      // precisely why it is safe to servo to. This threshold also BOUNDS THE QUEUE SPAN, which the
      // 16-bit flip stamps depend on: at most SPK_LAG_US + 20000 = 26ms, comfortably inside the
      // +/-32.7ms a signed 16-bit microsecond difference can express.
      playUs = gnow - SPK_LAG_US; playFrac = 0; err = 0;
      gPrev = gnow;
      spkTogR = spkTogW;
      anchored = true;
    }
    const int32_t nomQ16 = (int32_t)SPK_PERIOD_Q16;
    const int32_t loQ16 = nomQ16 - nomQ16 / 4, hiQ16 = nomQ16 + nomQ16 / 4;   // +/-25% of rate
    // FEED-FORWARD, not an integrator. Two integrators have now failed here, and for the same
    // underlying reason: both tried to INFER the guest's rate from the accumulated error, and that
    // is a slow way to learn a number the guest states outright. The bang-bang version could not
    // settle at all. The true integral settled, but had to wind its accumulator to ~118M to express
    // an 8% deficit, which at realistic error sizes took a full second -- and for that second the
    // error sat at -3000us, the whole of SPK_LAG_US, so the replay ran level with live guest time
    // and rendered windows whose edges the 6502 had not recorded yet. That is heard as hiss.
    //
    // How far the guest clock moved since the last sample IS the rate. Measuring it has no
    // acquisition ramp; all it needs is smoothing. One pole at ~90ms keeps the per-sample jitter
    // (which is the distortion, and must not reach the audio) out while still following the
    // emulator speeding up and slowing down over a frame.
    uint32_t dG = gnow - gPrev;
    gPrev = gnow;
    if (dG > 200) dG = 200;                             // a stall; the resync above handles those
    rateQ16 += ((int32_t)(dG << 16) - rateQ16) >> 12;
    // The trim. With the rate fed forward there is no standing rate error left for an integral to
    // null, so plain proportional suffices -- it only pulls the lag back to SPK_LAG_US after the
    // feed-forward has lagged a change in speed. First order, so it cannot limit-cycle.
    int32_t step = rateQ16 + err * 16;                  // proportional on top
    if (step < loQ16) step = loQ16; else if (step > hiQ16) step = hiQ16;
    uint32_t t0 = playUs;
    playFrac += (uint32_t)step;
    playUs   += playFrac >> 16;
    playFrac &= 0xFFFF;
    uint32_t t1 = playUs, cur = t0;
    while (spkTogR != spkTogW) {
      // The stamps are 16-bit, so recover the absolute time against this window's end. Valid
      // because the resync above bounds the queue to ~26ms of timeline (see SPK_TOG_RING).
      int32_t rel = (int16_t)(spkTogTs[spkTogR & (SPK_TOG_RING - 1)] - (uint16_t)t1);
      if (rel >= 0) break;                            // belongs to a later sample; leave it queued
      uint32_t ts = t1 + (uint32_t)rel;
      // A flip recorded late (timestamp already behind us) collapses to a zero-length segment
      // rather than being dropped -- cur never moves backwards, so dt is never negative.
      if ((int32_t)(ts - cur) > 0) { spkStep(ts - cur); cur = ts; }
      spkLevel = !spkLevel;
      spkTogR++;
    }
    spkStep(t1 - cur);                                // the rest of the window at the current level
    s = (int16_t)((spkY2 * amp) >> 10);               // SPK_UNIT -> -amp .. +amp
  }
  uint32_t w = spkW;
  if ((uint32_t)(w - spkR) < SPK_RING) {              // ring not full
    spkRing[w & (SPK_RING - 1)] = s;
    spkW = w + 1;
  }
  // else: consumer briefly behind -> drop this sample (the timer runs a hair faster than the I2S,
  // so the ring tends to stay full and we drop rare excess samples rather than ever underrunning).
}

#if defined(BOARD_PICOCALC)
// pico-sdk repeating-timer callbacks take the timer and return "keep running".
static bool IRAM_ATTR spkTimerCb(repeating_timer_t *) { spkISR(); return true; }
#endif

static void speakerTask(void *)
{
  // Arm the sampling timer from THIS task so its interrupt is allocated on this task's core (core 0),
  // keeping the ISR off core 1 where the USB host lives. 40MHz tick / 907 = ~44101Hz, a near exact
  // match to the I2S rate so the ring barely drifts.
#if defined(BOARD_PICOCALC)
  // RP2350: the ESP32 timer API does not exist. The pico-sdk default alarm pool gives the same
  // thing -- a hardware-paced periodic callback, independent of the FreeRTOS scheduler. A NEGATIVE
  // period means "every N us from the START of the previous callback", i.e. a fixed rate that does
  // not drift with callback duration. The period is SPK_US and the OUTPUT is opened at the matching
  // SPK_FS (see the note at the top), so producer and consumer run at the same rate and the ring
  // neither overruns nor underruns.
  add_repeating_timer_us(-SPK_US, spkTimerCb, NULL, &spkTimer);
#elif ESP_ARDUINO_VERSION_MAJOR >= 3
  // Arduino-ESP32 core 3.x (ESP32-P4): the timer API is frequency-based and folds
  // write+enable into timerAlarm(). 40 MHz tick / (40e6/SPK_FS) alarm == ~44100 Hz, autoreload.
  spkTimer = timerBegin(40000000);                     // 40 MHz tick
  timerAttachInterrupt(spkTimer, &spkISR);
  timerAlarm(spkTimer, 40000000UL / SPK_FS, true, 0);  // ~44100Hz, autoreload
#else
  spkTimer = timerBegin(0, 2, true);                   // 80MHz / 2 = 40MHz tick
  timerAttachInterrupt(spkTimer, &spkISR, true);
  timerAlarmWrite(spkTimer, 40000000UL / SPK_FS, true);// ~44100Hz
  timerAlarmEnable(spkTimer);
#endif

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

#define SPK_STACK_WORDS 512   // 2KB, what the dynamic create below asks for in bytes

void speakerSetup()
{
  ampBegin(SPK_FS);
  // Core 0: the task only blocks on the I2S DMA (no spin), so it won't starve the render loop the
  // way the old tight feed loop did; the timer ISR (armed inside the task) also lands on core 0.
#if defined(BOARD_PICOCALC)
  // No heap here, which the IIe cannot spare (A2_IIE_RESERVE, src/apple2/memory.cpp). The stack
  // is the idle end of sharedBigBuf: the Apple II map stops at 0xF000 (48K of main RAM plus three
  // 4K IIe banks) and the buffer runs to 64064, which leaves 2624 bytes nothing touches while an
  // Apple II runs -- the only machine that calls this on this board. Not the static sound-core
  // stack: a IIe borrows that for one of its banks (picocalcLendAudioStack).
  static_assert(sizeof(sharedBigBuf) >= 0xF000 + 8 + SPK_STACK_WORDS * sizeof(StackType_t),
                "the speaker stack must fit past the Apple II map in sharedBigBuf");
  static StaticTask_t spkTCB;
  StackType_t *stk = (StackType_t *)(((uintptr_t)(sharedBigBuf + 0xF000) + 7) & ~(uintptr_t)7);
  xTaskCreateStaticPinnedToCore(speakerTask, "speaker", SPK_STACK_WORDS, NULL, 2, stk, &spkTCB, 0);
#else
  xTaskCreatePinnedToCore(speakerTask, "speaker", 2048, NULL, 2, NULL, 0);
#endif
}
// Called from the 6502 on every $C030 access, from the other core. Records WHEN
// the flip happened so the sampler can reconstruct it; the timestamp is stored before the index is
// published, so the ISR never reads a slot that is not written yet.
//
// An overflow is NOT harmless and is not ignored. Losing one edge leaves the replayed level
// inverted, and inverting a 1-bit speaker is inaudible only while the level is a plain square --
// once the integrator is averaging dense toggling, a window that should come out near silence
// comes out at full scale instead. So say so and let the ISR resync.
// EVERY INSTRUCTION SPENT HERE COMES OFF THE 6502's CLOCK, and not evenly -- only while sound is
// playing. Measured on hardware: the emulator ran at 1.00MHz at 200 flips/s and 0.89MHz at 60000,
// a near-linear 11% loss, because the jitter instrumentation that used to live here read the
// hardware timer and did a 64-bit divide on a path the 6502 takes every ~9 emulated cycles.
//
// That is worse than it sounds, because it is a feedback loop rather than a constant tax: the
// replay clock follows the guest, so denser sound slowed the emulator, which lowered the replay
// rate, which sagged the pitch IN TIME WITH THE SOUND. It was heard as the speed and tone wobbling.
// So this function stays minimal -- an SRAM read, a bounds check and two stores -- and is kept out
// of flash so no call pays an XIP miss. Measure from the ISR or the consumer task, never from here.
//
// The other half of that fix is in the caller: read8/write8 in src/apple2/memory.cpp test for
// $C030 inline and call straight here, because reaching this through readSoftSwitches ->
// processSoftSwitches -> a ~40-case jump table meant three flash fetches per speaker access.
// Keep the two together -- lightening this function alone only recovered a third of the loss.
void IRAM_ATTR speakerToggle() {
  speaker_state = !speaker_state;
  // Guest time, not real time -- this is the whole fix. Falls back to the hardware timer for any
  // core that does not publish a guest clock.
  uint32_t now = appleGuestUs;
  if (!now) now = SPK_NOW_US();
  uint32_t w = spkTogW;
  if ((uint32_t)(w - spkTogR) < SPK_TOG_RING) {
    spkTogTs[w & (SPK_TOG_RING - 1)] = (uint16_t)now;
    spkTogW = w + 1;
  } else {
    spkTogLost = true;
  }
}
#endif
