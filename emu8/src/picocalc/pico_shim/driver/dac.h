// driver/dac.h - PicoCalc stub. Only the BOARD_AUDIO_DAC=1 branch of speaker.cpp uses dac_*; the
// RP2350 has no DAC, so the shared !BOARD_AUDIO_DAC speaker path runs instead. Exists so the
// #include resolves.
#pragma once
