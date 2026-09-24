// audio_picocalc.cpp - ClockworkPi PicoCalc audio output (PWM, not I2S).
//
// Replaces src/shared/audio_amp.cpp (which is compiled out here by BOARD_AUDIO_PWM) and exports
// the exact same three entry points, so every sound core -- SID, APU, TIA, PSG and the Apple
// 1-bit speaker -- keeps calling ampBegin()/ampWriteDac8()/ampWriteMono() unchanged.
//
// The PicoCalc drives a small speaker/headphone amp from two PWM pins on the SAME slice
// (GPIO26 = PWM5A, GPIO27 = PWM5B), which is exactly what arduino-pico's PWMAudio stereo mode
// expects: it takes the first pin and muxes pin+1 as the second channel.
//
// Why PWMAudio and not analogWrite(): the cores' write functions are expected to BLOCK until the
// hardware has room, because that back-pressure is what paces the emulated sound chip (see the
// i2s_write(..., portMAX_DELAY) calls this file replaces). PWMAudio is DMA-fed from a multi-buffer
// ring, i.e. the structural twin of the ESP32 I2S DMA. analogWrite() has no pacing at all and
// would free-run. Its stock write() waits by SPINNING though, which does not work here -- see
// dacPutMono() below.
//
// All the emulated machines are mono, so each sample is written twice (L then R).

#include "../../emu.h"

#if defined(BOARD_PICOCALC)

#include <PWMAudio.h>

// Stereo: PWMAudio uses AUDIO_PWM_L_PIN and AUDIO_PWM_L_PIN+1, which is AUDIO_PWM_R_PIN here.
static PWMAudio dac(AUDIO_PWM_L_PIN, /*stereo=*/true);
static bool     dacStarted = false;

// 8 buffers x 128 words: the same depth as the I2S path's dma_buf_count/dma_buf_len, so the
// amount of slack a core sees before write() blocks is unchanged (~23 ms at 44.1 kHz).
void ampBegin(int sampleRate)
{
  if (dacStarted) return;                    // idempotent: only one core makes sound at a time
  dac.setBuffers(8, 128);
  if (!dac.begin((long)sampleRate)) { printLog("audio: PWMAudio begin failed"); return; }
  dacStarted = true;
  sprintf(buf, "audio: PWM stereo on GPIO%d/%d @ %dHz", AUDIO_PWM_L_PIN, AUDIO_PWM_R_PIN, sampleRate);
  printLog(buf);
}

// Push one mono sample to both channels, sleeping -- not spinning -- when the DMA ring is full.
//
// This is the whole reason this helper exists. PWMAudio::write(sync=true) ends up in
// AudioBufferManager::write, whose "wait for a free buffer" is a bare `while (!*p) {}` busy
// loop. The speaker task is priority 2 and the render task priority 1, and on this board BOTH
// live on the same physical core (the 6502 has the other one to itself). So the first time the
// speaker caught up with the DMA it spun forever and the render task, one priority level below,
// never ran again -- a live emulator behind a frozen screen.
//
// So: write with sync=false and vTaskDelay() on refusal, which blocks the task properly and
// hands the core to the renderer. The pacing back-pressure the cores rely on is preserved --
// we still do not return until the sample is in the ring.
//
// Stereo ordering note: in stereo mode the first write only latches the sample into PWMAudio's
// hold word and always succeeds; the second is the one that needs a free buffer, and on refusal
// it leaves the hold word intact, so retrying it is safe.
static inline void dacPutMono(int16_t s)
{
  // The give-up path is checked BEFORE the frame is started, never between the two channels.
  // Bailing out after channel 0 used to leave PWMAudio's hold word latched, so the next call's
  // "first" write became the second half of the abandoned frame -- L and R swapped permanently,
  // and every later sample one position out. If the ring is wedged, skip the whole frame.
  int naps = 0;
  while (!dac.write(s, /*sync=*/false)) {    // channel 0: latches into the hold word
    vTaskDelay(1);                           // 1 tick = 1ms; the ring holds ~23ms, so no underrun
    if (++naps > 100) return;                // DMA stopped dead: drop the frame rather than hang
  }
  while (!dac.write(s, /*sync=*/false))      // channel 1: this is the one that needs a free buffer
    vTaskDelay(1);                           // no bail-out here -- the frame is already committed
}

// `n` samples in the cores' DAC format: an 8-bit level in the HIGH byte, i.e. unsigned and
// centred on 0x8000. Convert to 16-bit signed and duplicate to L+R.
void ampWriteDac8(const uint16_t *dacBuf, int n)
{
  if (!dacStarted) return;
  for (int i = 0; i < n; i++)
    dacPutMono((int16_t)((int)dacBuf[i] - 32768));
}

// `n` mono 16-bit-signed samples (duplicated L+R). Used by the Apple II speaker square wave.
void ampWriteMono(const int16_t *mono, int n)
{
  if (!dacStarted) return;
  for (int i = 0; i < n; i++)
    dacPutMono(mono[i]);
}

#endif // BOARD_PICOCALC
