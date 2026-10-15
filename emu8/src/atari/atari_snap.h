#pragma once
// Atari 2600 snapshot plumbing (atari_snapshot.cpp drives the format). The core keeps its state
// file-static in each translation unit, so every unit exposes ONE symmetric xxxSnap(SnapIO&) that
// moves its own fields in a fixed order -- the same call saves and loads (io.loading).
#include "../shared/snapshot.h"
#include "atari.h"

namespace atari {

// int / bool fields go through fixed-width values so desktop x64 and RP2040 files match.
static inline void snapI32(SnapIO &io, int &v) { int32_t t = (int32_t)v; io.val(t); if (io.loading) v = (int)t; }
static inline void snapU8v(SnapIO &io, volatile uint8_t &v) { uint8_t t = v; io.val(t); if (io.loading) v = t; }

bool cartSnapCheck(SnapIO &io);   // atari_cart.cpp: cartridge identity (reject on mismatch)
void cartSnap(SnapIO &io);        // atari_cart.cpp: bank number + Superchip RAM
void cpuSnap(SnapIO &io);         // atari_cpu.cpp:  6507 registers
void riotSnap(SnapIO &io);        // atari_riot.cpp: RAM, timer, ports, input latches
void tiaSnap(SnapIO &io);         // atari_tia.cpp:  registers, beam, objects, collisions, field so far
void audioSnap(SnapIO &io);       // atari_audio.cpp: both voices

} // namespace atari
