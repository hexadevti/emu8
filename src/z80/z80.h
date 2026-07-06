// z80.h - Zilog Z80 CPU core for the MSX1 port (M1).
//
// Memory-agnostic, in the same spirit as src/iigs/cpu65816.h: the host supplies callbacks, so the
// SAME core compiles into the desktop harness (host/msx_host.cpp, runs ZEXDOC/ZEXALL) and into the
// MSX1 machine on the board (src/msx/msx_machine.cpp). Unlike the 6502/65816 cores, the Z80 has a
// SEPARATE 256-port I/O space, so there are four hooks: rd/wr (memory) and in/out (I/O ports).
//
// Flag register (F) bit layout:  S Z Y H X P/V N C  (bit7..bit0)
//   S (0x80) sign        Z (0x40) zero          Y (0x20) undoc. copy of result bit5
//   H (0x10) half-carry  X (0x08) undoc. copy of result bit3
//   P/V(0x04) parity/overflow   N (0x02) add/subtract   C (0x01) carry
//
// Targets full documented + undocumented behaviour (X/Y flags, IXH/IXL, the MEMPTR/WZ register used
// by BIT n,(HL)) so ZEXALL passes, not just ZEXDOC.

#pragma once
#include <stdint.h>

// status flags
#define Z80_CF 0x01
#define Z80_NF 0x02
#define Z80_PF 0x04   // parity (logical) / overflow (arithmetic)
#define Z80_VF 0x04
#define Z80_XF 0x08   // undocumented: copy of result bit 3
#define Z80_HF 0x10
#define Z80_YF 0x20   // undocumented: copy of result bit 5
#define Z80_ZF 0x40
#define Z80_SF 0x80

struct Z80 {
  // main register file (kept as bytes so 16-bit pair access is host-endian-independent)
  uint8_t A, F, B, C, D, E, H, L;
  // alternate set (EX AF,AF' / EXX)
  uint8_t A_, F_, B_, C_, D_, E_, H_, L_;
  // index registers (exposed as IXH/IXL/IYH/IYL bytes for the undocumented opcodes)
  uint8_t IXH, IXL, IYH, IYL;
  uint8_t I;            // interrupt vector base
  uint8_t R;            // memory-refresh counter (only the low 7 bits increment)
  uint8_t R7;           // preserved bit 7 of R (LD R,A sets it; refresh never touches it)
  uint16_t SP, PC;
  uint16_t MEMPTR;      // internal WZ register (drives X/Y flags of BIT n,(HL)/(IX+d))

  bool IFF1, IFF2;      // interrupt enable flip-flops
  uint8_t IM;           // interrupt mode 0/1/2
  bool halted;          // set by HALT; cleared by an accepted interrupt or reset
  int  eiPending;       // (reserved) EI bookkeeping
  bool _eiBlock;        // true for exactly one instruction after EI: blocks irq() acceptance

  uint64_t cycles;      // running T-state count

  // memory + I/O hooks supplied by the host
  uint8_t (*rd)(uint16_t addr);
  void    (*wr)(uint16_t addr, uint8_t val);
  uint8_t (*in)(uint16_t port);
  void    (*out)(uint16_t port, uint8_t val);

  void reset();         // power-on / RESET: PC=0, I=R=0, IFF=0, IM 0, SP=FFFF
  int  step();          // execute one instruction; returns the T-states it took
  bool irq(uint8_t bus);// raise a maskable INT (bus = byte placed on the data bus for IM0/IM2);
                        //   returns true if accepted (IFF1 was set). MSX uses IM 1 (bus ignored).
  void nmi();           // raise a non-maskable interrupt (unused on MSX1, provided for completeness)

  // 16-bit pair accessors
  inline uint16_t BC() const { return (uint16_t(B) << 8) | C; }
  inline uint16_t DE() const { return (uint16_t(D) << 8) | E; }
  inline uint16_t HL() const { return (uint16_t(H) << 8) | L; }
  inline uint16_t AF() const { return (uint16_t(A) << 8) | F; }
  inline uint16_t IX() const { return (uint16_t(IXH) << 8) | IXL; }
  inline uint16_t IY() const { return (uint16_t(IYH) << 8) | IYL; }
  inline void setBC(uint16_t v) { B = v >> 8; C = (uint8_t)v; }
  inline void setDE(uint16_t v) { D = v >> 8; E = (uint8_t)v; }
  inline void setHL(uint16_t v) { H = v >> 8; L = (uint8_t)v; }
  inline void setAF(uint16_t v) { A = v >> 8; F = (uint8_t)v; }
  inline void setIX(uint16_t v) { IXH = v >> 8; IXL = (uint8_t)v; }
  inline void setIY(uint16_t v) { IYH = v >> 8; IYL = (uint8_t)v; }
};
