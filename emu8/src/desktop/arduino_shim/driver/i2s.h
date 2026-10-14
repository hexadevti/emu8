// driver/i2s.h — desktop stub. The per-platform *_audio.cpp #include this unconditionally, but under
// BOARD_AUDIO_DAC=0 (desktop) every i2s_* reference is compiled out (audio routes through
// ampWriteDac8/ampWriteMono, implemented by src/desktop/audio_sdl.cpp). Real audio_amp.cpp is NOT
// compiled on desktop. So this only needs to exist for the #include to resolve.
#pragma once
