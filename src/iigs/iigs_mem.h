// iigs_mem.h - Apple IIGS 24-bit banked memory (M2 skeleton).
//
// Implements the layered-memory design validated by the M0/M0.5 benchmarks: hot banks in internal
// SRAM, expansion RAM in Octal PSRAM, dispatched through a `bankPtr[256]` table + read24/write24
// (the shape the CPU core's rd/wr callbacks plug into). M2 maps banks $00/$01 (SRAM) + $02.. (PSRAM)
// and stubs the $C000-$CFFF I/O window; the $E0/$E1 video/shadow banks and real softswitch/shadow
// handling arrive in M3. See the IIGS feasibility memory for the GO verdict and the layout rationale.

#pragma once
#include <stdint.h>

bool    iigsMemInit();                 // allocate banks; false on out-of-memory
uint8_t iigsRead24(uint32_t addr24);   // CPU rd callback
void    iigsWrite24(uint32_t addr24, uint8_t val);  // CPU wr callback

extern uint8_t* iigsBankPtr[256];      // per-bank 64KB base pointer, or nullptr (I/O / unmapped)
