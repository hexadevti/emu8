// driver/adc.h - PicoCalc stub. *_audio.cpp and joystick.cpp #include this for adc_power_acquire()
// / adc1_config_*, which are only called under BOARD_AUDIO_DAC=1 and BOARD_INPUT_ANALOG=1 -- both 0
// here (GPIO26/27 are the PWM audio pins on this board, not a joystick). Exists so the #include resolves.
#pragma once
