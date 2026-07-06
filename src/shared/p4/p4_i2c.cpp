// p4_i2c.cpp - see p4_i2c.h. Single shared Wire init for the GT911 touch + ES8311 codec.
#include "p4_i2c.h"

#if BOARD_TOUCH_GT911 || BOARD_AUDIO_CODEC

#include <Arduino.h>
#include <Wire.h>

void p4WireBegin() {
  static bool done = false;
  if (done) return;
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);
  done = true;
}

#endif
