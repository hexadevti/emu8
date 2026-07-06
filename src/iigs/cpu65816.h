// cpu65816.h - WDC 65C816 CPU core for the Apple IIGS port (M1).
//
// Memory-agnostic: the host supplies 24-bit read/write callbacks (rd/wr), so the same core
// runs against the on-device self-test RAM (src/iigs/cpu65816_test.cpp) and, later, the real
// IIGS banked memory (read24/write24 + bankPtr). M1 targets EMULATION mode (E=1, behaves like a
// 65C02 - what the IIGS boots into) plus the full native-mode 8/16-bit width machinery so M2
// (native mode) is a small step.
//
// Status (P) bits:  N V M X D I Z C   (bit7..bit0)
//   M (0x20) = accumulator/memory width: 1 = 8-bit.  Forced 1 in emulation mode.
//   X (0x10) = index width:              1 = 8-bit.  Forced 1 in emulation mode (B flag slot).
//   E (separate) = emulation flag; swapped with carry by XCE.

#pragma once
#include <stdint.h>

// status flags
#define P65_C 0x01
#define P65_Z 0x02
#define P65_I 0x04
#define P65_D 0x08
#define P65_X 0x10   // index width (native); break-flag slot (emulation)
#define P65_M 0x20   // accumulator/memory width (native)
#define P65_V 0x40
#define P65_N 0x80

struct CPU65816 {
  // registers (A/X/Y are full 16 bits; only the low byte is used in 8-bit width)
  uint16_t A, X, Y;
  uint16_t D;          // direct-page register
  uint16_t S;          // stack pointer (high byte forced 0x01 in emulation mode)
  uint16_t PC;         // program counter within the program bank
  uint8_t  PBR, DBR;   // program-bank / data-bank registers
  uint8_t  P;          // status
  bool     E;          // emulation flag
  bool     stopped;    // set by STP; cleared only by reset
  bool     waiting;    // set by WAI; cleared by an interrupt (modelled as a no-op step here)

  uint64_t cycles;     // running cycle count (approximate; not cycle-exact yet)

  // 24-bit memory hooks supplied by the host
  uint8_t (*rd)(uint32_t addr24);
  void    (*wr)(uint32_t addr24, uint8_t val);

  void reset();        // emulation-mode reset; loads PC from the 00:FFFC vector
  int  step();         // execute one instruction; returns approximate cycle count

  // width helpers (8-bit when emulation, or when the corresponding P bit is set)
  inline bool m8() const { return E || (P & P65_M); }   // accumulator/memory is 8-bit
  inline bool x8() const { return E || (P & P65_X); }   // index registers are 8-bit
};
