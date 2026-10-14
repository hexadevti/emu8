// p4_i2c.h - shared I2C bus bring-up for the JC1060P470 (ESP32-P4).
//
// The GT911 capacitive touch (touchkeyboard.cpp) and the ES8311 audio codec (audio_amp.cpp) sit on
// the SAME I2C bus (SDA=GPIO7, SCL=GPIO8). p4WireBegin() initializes Wire once; both subsystems
// call it during setup() so whichever runs first brings the bus up.
#pragma once

#include "../../../board.h"

#if BOARD_TOUCH_GT911 || BOARD_AUDIO_CODEC
void p4WireBegin();   // idempotent: Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN) at 400 kHz, once
#endif
