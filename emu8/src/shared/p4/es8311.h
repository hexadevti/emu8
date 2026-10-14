// es8311.h - ES8311 audio codec bring-up for the JC1060P470 (ESP32-P4).
//
// The P4 has no internal DAC. Audio goes I2S -> ES8311 codec -> NS4150B class-D amp -> speaker.
// The ES8311 is configured once over I2C (shared bus, GPIO7/8) for 16-bit I2S playback with an
// external MCLK from the ESP32 (256 x Fs). audio_amp.cpp calls es8311Init() from ampBegin() and
// then streams PCM over I2S exactly like the S3's dumb amp.
#pragma once

#include "../../../board.h"

#if BOARD_AUDIO_CODEC

#include <stdint.h>

// Configure the ES8311 for DAC playback at `sampleRate` (MCLK assumed = 256 * sampleRate, which is
// what the I2S driver emits on I2S_MCLK_PIN by default). Returns false if the codec does not ACK.
bool es8311Init(uint32_t sampleRate);

// Set DAC volume (0..100). Optional; es8311Init() leaves it near 0 dB.
void es8311SetVolume(uint8_t pct);

#endif // BOARD_AUDIO_CODEC
