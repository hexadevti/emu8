// driver/i2s.h - PicoCalc stub. Every per-platform *_audio.cpp #includes this unconditionally, but
// each i2s_* reference sits inside a `#if BOARD_AUDIO_DAC` branch, which is 0 here -- audio routes
// through ampBegin/ampWriteMono/ampWriteDac8, implemented for this board by
// src/picocalc/audio_picocalc.cpp on top of PWMAudio. So this only has to make the #include resolve.
#pragma once
