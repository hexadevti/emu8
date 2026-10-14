// es8311.cpp - see es8311.h. Minimal ES8311 DAC-playback init over I2C.
//
// Register sequence is the canonical Espressif/esp-adf ES8311 setup for a codec slaved to the ESP32
// I2S master with an external MCLK = 256*Fs, 16-bit I2S frames, DAC path only (no ADC/mic). The
// clock-coefficient registers (0x02/0x03/0x04..0x08/0x16) here target the 256*Fs ratio used for the
// emulator's 44.1 kHz / 48 kHz audio. If playback is silent/distorted on hardware, verify the MCLK
// ratio and tune these against the ES8311 datasheet or the esp_codec_dev / esp-adf es8311 driver.

#include "es8311.h"

#if BOARD_AUDIO_CODEC

#include <Arduino.h>
#include <Wire.h>
#include "p4_i2c.h"

static bool es8311Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static uint8_t es8311Read(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  Wire.requestFrom((int)ES8311_ADDR, 1);
  return Wire.available() ? Wire.read() : 0;
}

bool es8311Init(uint32_t sampleRate) {
  (void)sampleRate;   // coefficients below assume MCLK = 256*Fs (the I2S default)
  p4WireBegin();

  // Probe: ES8311 chip-ID registers 0xFD/0xFE read 0x83/0x11.
  uint8_t id1 = es8311Read(0xFD), id2 = es8311Read(0xFE);
  Serial.printf("ES8311 id: 0x%02X 0x%02X\n", id1, id2);

  // --- reset ---
  es8311Write(0x00, 0x1F); delay(20);
  es8311Write(0x00, 0x00);
  es8311Write(0x45, 0x00);

  // --- clock manager: use external MCLK, enable BCLK/clocks (256*Fs ratio) ---
  es8311Write(0x01, 0x30);   // MCLK on, clkdac/clkadc on
  es8311Write(0x02, 0x00);   // pre-div=1, mult=1 (256*Fs)
  es8311Write(0x03, 0x10);   // FS mode / ADC OSR
  es8311Write(0x16, 0x24);   // DAC OSR
  es8311Write(0x04, 0x10);   // DAC OSR
  es8311Write(0x05, 0x00);   // divider for ADC/DAC clocks
  es8311Write(0x06, 0x03);   // BCLK divider (256*Fs / 16-bit*2ch)
  es8311Write(0x07, 0x00);
  es8311Write(0x08, 0xFF);

  // --- power up, slave mode ---
  es8311Write(0x00, 0x80);   // CSM power up, slave

  // --- serial data ports: 16-bit I2S, in and out ---
  es8311Write(0x09, 0x0C);   // SDP IN : I2S, 16-bit
  es8311Write(0x0A, 0x0C);   // SDP OUT: I2S, 16-bit

  // --- system / analog power ---
  es8311Write(0x0B, 0x00);
  es8311Write(0x0C, 0x00);
  es8311Write(0x10, 0x1F);   // analog power
  es8311Write(0x11, 0x7F);   // analog power
  es8311Write(0x00, 0x80);

  // --- DAC path ---
  es8311Write(0x0D, 0x01);   // power up DAC
  es8311Write(0x0E, 0x02);
  es8311Write(0x12, 0x00);   // DAC not muted
  es8311Write(0x13, 0x10);
  es8311Write(0x14, 0x1A);   // (ADC select; harmless for DAC-only use)
  es8311Write(0x32, 0x90);   // DAC volume: cut well below 0 dB (0xBF) — the NS4150B 3W amp + full-scale
                             // digital was way too loud. Raise/lower this (0x00 mute .. 0xFF max) to taste.
  es8311Write(0x37, 0x08);
  es8311Write(0x44, 0x08);   // route DAC to output
  es8311Write(0x01, 0x3F);   // clocks fully on

  return (id1 == 0x83);      // true when the codec was detected on the bus
}

void es8311SetVolume(uint8_t pct) {
  if (pct > 100) pct = 100;
  es8311Write(0x32, (uint8_t)(pct * 255 / 100));   // 0x32: 0x00 mute .. 0xFF max
}

#endif // BOARD_AUDIO_CODEC
