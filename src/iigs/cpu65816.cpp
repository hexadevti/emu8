// cpu65816.cpp - WDC 65C816 CPU core (M1). See cpu65816.h for the model and status-bit layout.
//
// Correctness-first interpreter (not yet cycle-exact). Covers the full 256-opcode set: the 6502 /
// 65C02 base (what the IIGS runs in emulation mode) plus the native-mode 8/16-bit width machinery,
// long/stack-relative/indirect-long addressing, block moves, and XCE/REP/SEP mode control.

#include "cpu65816.h"

// ----------------------------------------------------------------- low-level memory + fetch
static inline uint8_t  rd8 (CPU65816& c, uint32_t a)            { return c.rd(a & 0xFFFFFF); }
static inline void     wr8 (CPU65816& c, uint32_t a, uint8_t v) { c.wr(a & 0xFFFFFF, v); }
// 16-bit access that wraps within the address's bank (the 65816 keeps the bank fixed on +1)
static inline uint16_t rd16b(CPU65816& c, uint32_t a) {
  uint32_t bk = a & 0xFF0000;
  return c.rd(a & 0xFFFFFF) | ((uint16_t)c.rd(bk | ((a + 1) & 0xFFFF)) << 8);
}
static inline void wr16b(CPU65816& c, uint32_t a, uint16_t v) {
  uint32_t bk = a & 0xFF0000;
  c.wr(a & 0xFFFFFF, v & 0xFF);
  c.wr(bk | ((a + 1) & 0xFFFF), v >> 8);
}
// bank-0 pointer reads (direct page / stack), 16-bit wrap inside bank 0
static inline uint16_t rd16_0(CPU65816& c, uint16_t a) { return c.rd(a) | ((uint16_t)c.rd((uint16_t)(a + 1)) << 8); }
static inline uint32_t rd24_0(CPU65816& c, uint16_t a) {
  return c.rd(a) | ((uint32_t)c.rd((uint16_t)(a + 1)) << 8) | ((uint32_t)c.rd((uint16_t)(a + 2)) << 16);
}

static inline uint8_t  fetch8 (CPU65816& c) { uint8_t v = c.rd(((uint32_t)c.PBR << 16) | c.PC); c.PC = (c.PC + 1) & 0xFFFF; return v; }
static inline uint16_t fetch16(CPU65816& c) { uint8_t lo = fetch8(c), hi = fetch8(c); return lo | (hi << 8); }
static inline uint32_t fetch24(CPU65816& c) { uint8_t lo = fetch8(c), hi = fetch8(c), bk = fetch8(c); return lo | (hi << 8) | ((uint32_t)bk << 16); }
static inline uint16_t immM(CPU65816& c) { return c.m8() ? fetch8(c) : fetch16(c); }
static inline uint16_t immX(CPU65816& c) { return c.x8() ? fetch8(c) : fetch16(c); }

// ----------------------------------------------------------------- stack
static inline void push8(CPU65816& c, uint8_t v) { c.wr(c.S, v); c.S = c.E ? (0x0100 | ((c.S - 1) & 0xFF)) : ((c.S - 1) & 0xFFFF); }
static inline uint8_t pull8(CPU65816& c) { c.S = c.E ? (0x0100 | ((c.S + 1) & 0xFF)) : ((c.S + 1) & 0xFFFF); return c.rd(c.S); }
static inline void push16(CPU65816& c, uint16_t v) { push8(c, v >> 8); push8(c, v & 0xFF); }
static inline uint16_t pull16(CPU65816& c) { uint8_t lo = pull8(c); uint8_t hi = pull8(c); return lo | (hi << 8); }

// ----------------------------------------------------------------- flags + width normalize
static inline void setZN8 (CPU65816& c, uint8_t v)  { c.P &= ~(P65_Z | P65_N); if (!v) c.P |= P65_Z; if (v & 0x80)   c.P |= P65_N; }
static inline void setZN16(CPU65816& c, uint16_t v) { c.P &= ~(P65_Z | P65_N); if (!v) c.P |= P65_Z; if (v & 0x8000) c.P |= P65_N; }
static inline void setZNm (CPU65816& c, uint16_t v) { if (c.m8()) setZN8(c, v); else setZN16(c, v); }
static inline void setZNx (CPU65816& c, uint16_t v) { if (c.x8()) setZN8(c, v); else setZN16(c, v); }

// Apply mode-dependent register constraints (call after any change to E or M/X).
static inline void normalize(CPU65816& c) {
  if (c.E) { c.P |= (P65_M | P65_X); c.S = 0x0100 | (c.S & 0xFF); }
  if (c.x8()) { c.X &= 0xFF; c.Y &= 0xFF; }
}

// ----------------------------------------------------------------- direct page + effective addresses
static inline uint16_t dpAddr(CPU65816& c, uint8_t off) { return (uint16_t)(c.D + off); }
static inline uint16_t dpIdx (CPU65816& c, uint8_t off, uint16_t idx) {
  if (c.E && (c.D & 0xFF) == 0) return (c.D & 0xFF00) | ((off + idx) & 0xFF);   // emulation page-wrap
  return (uint16_t)(c.D + off + idx);
}
static inline uint32_t eaAbs       (CPU65816& c) { return ((uint32_t)c.DBR << 16) | fetch16(c); }
static inline uint32_t eaAbsX      (CPU65816& c) { return ((((uint32_t)c.DBR << 16) | fetch16(c)) + c.X) & 0xFFFFFF; }
static inline uint32_t eaAbsY      (CPU65816& c) { return ((((uint32_t)c.DBR << 16) | fetch16(c)) + c.Y) & 0xFFFFFF; }
static inline uint32_t eaLong      (CPU65816& c) { return fetch24(c); }
static inline uint32_t eaLongX     (CPU65816& c) { return (fetch24(c) + c.X) & 0xFFFFFF; }
static inline uint32_t eaDP        (CPU65816& c) { return dpAddr(c, fetch8(c)); }
static inline uint32_t eaDPX       (CPU65816& c) { return dpIdx(c, fetch8(c), c.X); }
static inline uint32_t eaDPY       (CPU65816& c) { return dpIdx(c, fetch8(c), c.Y); }
static inline uint32_t eaIndDP     (CPU65816& c) { uint16_t p = dpAddr(c, fetch8(c)); return ((uint32_t)c.DBR << 16) | rd16_0(c, p); }
static inline uint32_t eaIndDPX    (CPU65816& c) { uint16_t p = dpIdx(c, fetch8(c), c.X); return ((uint32_t)c.DBR << 16) | rd16_0(c, p); }
static inline uint32_t eaIndDPY    (CPU65816& c) { uint16_t p = dpAddr(c, fetch8(c)); uint32_t b = ((uint32_t)c.DBR << 16) | rd16_0(c, p); return (b + c.Y) & 0xFFFFFF; }
static inline uint32_t eaIndLongDP (CPU65816& c) { uint16_t p = dpAddr(c, fetch8(c)); return rd24_0(c, p); }
static inline uint32_t eaIndLongDPY(CPU65816& c) { uint16_t p = dpAddr(c, fetch8(c)); return (rd24_0(c, p) + c.Y) & 0xFFFFFF; }
static inline uint32_t eaStackRel  (CPU65816& c) { return (uint16_t)(c.S + fetch8(c)); }
static inline uint32_t eaStackRelY (CPU65816& c) { uint16_t p = (uint16_t)(c.S + fetch8(c)); uint32_t b = ((uint32_t)c.DBR << 16) | rd16_0(c, p); return (b + c.Y) & 0xFFFFFF; }

static inline uint16_t loadM (CPU65816& c, uint32_t ea) { return c.m8() ? rd8(c, ea) : rd16b(c, ea); }
static inline void     storeM(CPU65816& c, uint32_t ea, uint16_t v) { if (c.m8()) wr8(c, ea, v & 0xFF); else wr16b(c, ea, v); }

// ----------------------------------------------------------------- ALU (width-aware)
static void opLDA(CPU65816& c, uint16_t v) { if (c.m8()) { c.A = (c.A & 0xFF00) | (v & 0xFF); setZN8(c, v); } else { c.A = v; setZN16(c, v); } }
static void opORA(CPU65816& c, uint16_t v) { if (c.m8()) { uint8_t r = (c.A | v) & 0xFF; c.A = (c.A & 0xFF00) | r; setZN8(c, r); } else { c.A |= v; setZN16(c, c.A); } }
static void opAND(CPU65816& c, uint16_t v) { if (c.m8()) { uint8_t r = (c.A & v) & 0xFF; c.A = (c.A & 0xFF00) | r; setZN8(c, r); } else { c.A &= v; setZN16(c, c.A); } }
static void opEOR(CPU65816& c, uint16_t v) { if (c.m8()) { uint8_t r = (c.A ^ v) & 0xFF; c.A = (c.A & 0xFF00) | r; setZN8(c, r); } else { c.A ^= v; setZN16(c, c.A); } }

static void opADC(CPU65816& c, uint16_t m) {
  bool carry = c.P & P65_C;
  if (c.P & P65_D) {                                     // decimal
    if (c.m8()) {
      uint16_t a = c.A & 0xFF;
      int lo = (a & 0xF) + (m & 0xF) + (carry ? 1 : 0); if (lo > 9) lo += 6;
      int hi = (a >> 4) + ((m >> 4) & 0xF) + (lo > 0xF ? 1 : 0);
      bool v = (~(a ^ m) & (a ^ (hi << 4)) & 0x80);
      if (hi > 9) hi += 6;
      uint8_t res = ((hi << 4) | (lo & 0xF)) & 0xFF;
      c.P &= ~(P65_C | P65_V); if (hi > 0xF) c.P |= P65_C; if (v) c.P |= P65_V;
      c.A = (c.A & 0xFF00) | res; setZN8(c, res);
    } else {
      uint32_t a = c.A;
      int r0 = (a & 0xF) + (m & 0xF) + (carry ? 1 : 0); if (r0 > 9) r0 += 6;
      int r1 = ((a >> 4) & 0xF) + ((m >> 4) & 0xF) + (r0 > 0xF ? 1 : 0); if (r1 > 9) r1 += 6;
      int r2 = ((a >> 8) & 0xF) + ((m >> 8) & 0xF) + (r1 > 0xF ? 1 : 0); if (r2 > 9) r2 += 6;
      int r3 = ((a >> 12) & 0xF) + ((m >> 12) & 0xF) + (r2 > 0xF ? 1 : 0);
      bool v = (~(a ^ m) & (a ^ ((uint32_t)r3 << 12)) & 0x8000);
      if (r3 > 9) r3 += 6;
      uint16_t res = ((r3 << 12) | ((r2 & 0xF) << 8) | ((r1 & 0xF) << 4) | (r0 & 0xF));
      c.P &= ~(P65_C | P65_V); if (r3 > 0xF) c.P |= P65_C; if (v) c.P |= P65_V;
      c.A = res; setZN16(c, res);
    }
  } else {                                              // binary
    if (c.m8()) {
      uint16_t a = c.A & 0xFF, s = a + (m & 0xFF) + (carry ? 1 : 0);
      c.P &= ~(P65_C | P65_V); if (s > 0xFF) c.P |= P65_C; if (~(a ^ m) & (a ^ s) & 0x80) c.P |= P65_V;
      uint8_t res = s & 0xFF; c.A = (c.A & 0xFF00) | res; setZN8(c, res);
    } else {
      uint32_t a = c.A, s = a + m + (carry ? 1 : 0);
      c.P &= ~(P65_C | P65_V); if (s > 0xFFFF) c.P |= P65_C; if (~(a ^ m) & (a ^ s) & 0x8000) c.P |= P65_V;
      uint16_t res = s & 0xFFFF; c.A = res; setZN16(c, res);
    }
  }
}

static void opSBC(CPU65816& c, uint16_t m) {
  bool carry = c.P & P65_C;
  if (c.m8()) {
    uint16_t a = c.A & 0xFF, b = m & 0xFF, bin = a + ((~b) & 0xFF) + (carry ? 1 : 0);
    c.P &= ~(P65_C | P65_V); if (bin > 0xFF) c.P |= P65_C; if ((a ^ b) & (a ^ bin) & 0x80) c.P |= P65_V;
    uint8_t res = bin & 0xFF;
    if (c.P & P65_D) {
      int lo = (a & 0xF) - (b & 0xF) - (carry ? 0 : 1), hi = (a >> 4) - (b >> 4);
      if (lo < 0) { lo += 10; hi--; } if (hi < 0) hi += 10;
      res = ((hi << 4) | (lo & 0xF)) & 0xFF;
    }
    c.A = (c.A & 0xFF00) | res; setZN8(c, res);
  } else {
    uint32_t a = c.A, b = m, bin = a + ((~b) & 0xFFFF) + (carry ? 1 : 0);
    c.P &= ~(P65_C | P65_V); if (bin > 0xFFFF) c.P |= P65_C; if ((a ^ b) & (a ^ bin) & 0x8000) c.P |= P65_V;
    uint16_t res = bin & 0xFFFF;
    if (c.P & P65_D) {
      int n0 = (a & 0xF) - (b & 0xF) - (carry ? 0 : 1), bw = 0; if (n0 < 0) { n0 += 10; bw = 1; }
      int n1 = ((a >> 4) & 0xF) - ((b >> 4) & 0xF) - bw; bw = 0; if (n1 < 0) { n1 += 10; bw = 1; }
      int n2 = ((a >> 8) & 0xF) - ((b >> 8) & 0xF) - bw; bw = 0; if (n2 < 0) { n2 += 10; bw = 1; }
      int n3 = ((a >> 12) & 0xF) - ((b >> 12) & 0xF) - bw; if (n3 < 0) n3 += 10;
      res = ((n3 << 12) | ((n2 & 0xF) << 8) | ((n1 & 0xF) << 4) | (n0 & 0xF));
    }
    c.A = res; setZN16(c, res);
  }
}

static void opCMPv(CPU65816& c, uint16_t reg, uint16_t m, bool wide) {
  if (!wide) { uint16_t r = (reg & 0xFF) - (m & 0xFF); c.P &= ~P65_C; if ((reg & 0xFF) >= (m & 0xFF)) c.P |= P65_C; setZN8(c, r & 0xFF); }
  else { uint32_t r = (uint32_t)reg - (uint32_t)m; c.P &= ~P65_C; if (reg >= m) c.P |= P65_C; setZN16(c, r & 0xFFFF); }
}
static void opBIT(CPU65816& c, uint16_t m, bool imm) {
  uint16_t a = c.m8() ? (c.A & 0xFF) : c.A, r = a & m;
  c.P &= ~P65_Z; if (!(c.m8() ? (r & 0xFF) : r)) c.P |= P65_Z;
  if (!imm) {
    if (c.m8()) { c.P &= ~(P65_N | P65_V); if (m & 0x80)   c.P |= P65_N; if (m & 0x40)   c.P |= P65_V; }
    else        { c.P &= ~(P65_N | P65_V); if (m & 0x8000) c.P |= P65_N; if (m & 0x4000) c.P |= P65_V; }
  }
}

static uint16_t opASL(CPU65816& c, uint16_t v) { if (c.m8()) { c.P &= ~P65_C; if (v & 0x80) c.P |= P65_C; uint8_t r = (v << 1) & 0xFF; setZN8(c, r); return r; } else { c.P &= ~P65_C; if (v & 0x8000) c.P |= P65_C; uint16_t r = (v << 1) & 0xFFFF; setZN16(c, r); return r; } }
static uint16_t opLSR(CPU65816& c, uint16_t v) { c.P &= ~P65_C; if (v & 1) c.P |= P65_C; if (c.m8()) { uint8_t r = (v & 0xFF) >> 1; setZN8(c, r); return r; } else { uint16_t r = v >> 1; setZN16(c, r); return r; } }
static uint16_t opROL(CPU65816& c, uint16_t v) { uint16_t cin = (c.P & P65_C) ? 1 : 0; if (c.m8()) { c.P &= ~P65_C; if (v & 0x80) c.P |= P65_C; uint8_t r = ((v << 1) | cin) & 0xFF; setZN8(c, r); return r; } else { c.P &= ~P65_C; if (v & 0x8000) c.P |= P65_C; uint16_t r = ((v << 1) | cin) & 0xFFFF; setZN16(c, r); return r; } }
static uint16_t opROR(CPU65816& c, uint16_t v) { uint16_t cin = (c.P & P65_C) ? 1 : 0; c.P &= ~P65_C; if (v & 1) c.P |= P65_C; if (c.m8()) { uint8_t r = ((v & 0xFF) >> 1) | (cin << 7); setZN8(c, r); return r; } else { uint16_t r = (v >> 1) | (cin << 15); setZN16(c, r); return r; } }
static uint16_t opINC(CPU65816& c, uint16_t v) { if (c.m8()) { uint8_t r = (v + 1) & 0xFF; setZN8(c, r); return r; } else { uint16_t r = (v + 1) & 0xFFFF; setZN16(c, r); return r; } }
static uint16_t opDEC(CPU65816& c, uint16_t v) { if (c.m8()) { uint8_t r = (v - 1) & 0xFF; setZN8(c, r); return r; } else { uint16_t r = (v - 1) & 0xFFFF; setZN16(c, r); return r; } }
static uint16_t opTSB(CPU65816& c, uint16_t m) { uint16_t a = c.m8() ? (c.A & 0xFF) : c.A; c.P &= ~P65_Z; if (!(c.m8() ? ((a & m) & 0xFF) : (a & m))) c.P |= P65_Z; return m | a; }
static uint16_t opTRB(CPU65816& c, uint16_t m) { uint16_t a = c.m8() ? (c.A & 0xFF) : c.A; c.P &= ~P65_Z; if (!(c.m8() ? ((a & m) & 0xFF) : (a & m))) c.P |= P65_Z; return m & ~a; }

// accumulator-target RMW helper
#define RMW_A(FN) do { if (c.m8()) c.A = (c.A & 0xFF00) | (FN(c, c.A & 0xFF) & 0xFF); else c.A = FN(c, c.A); } while (0)
#define RMW_M(FN, EAEXPR) do { uint32_t ea = (EAEXPR); uint16_t v = loadM(c, ea); storeM(c, ea, FN(c, v)); } while (0)

static inline void branch(CPU65816& c, bool take) { int8_t r = (int8_t)fetch8(c); if (take) c.PC = (uint16_t)(c.PC + r); }

// ----------------------------------------------------------------- reset + step
void CPU65816::reset() {
  E = true; P = P65_M | P65_X | P65_I; D = 0; DBR = 0; PBR = 0; S = 0x01FF;
  A = X = Y = 0; stopped = waiting = false; cycles = 0;
  PC = rd(0x00FFFC) | ((uint16_t)rd(0x00FFFD) << 8);
}

int CPU65816::step() {
  if (stopped) return 1;
  CPU65816& c = *this;
  uint8_t op = fetch8(c);
  switch (op) {
    // ---- ORA ----
    case 0x09: opORA(c, immM(c)); break;
    case 0x05: opORA(c, loadM(c, eaDP(c))); break;
    case 0x15: opORA(c, loadM(c, eaDPX(c))); break;
    case 0x0D: opORA(c, loadM(c, eaAbs(c))); break;
    case 0x1D: opORA(c, loadM(c, eaAbsX(c))); break;
    case 0x19: opORA(c, loadM(c, eaAbsY(c))); break;
    case 0x01: opORA(c, loadM(c, eaIndDPX(c))); break;
    case 0x11: opORA(c, loadM(c, eaIndDPY(c))); break;
    case 0x12: opORA(c, loadM(c, eaIndDP(c))); break;
    case 0x07: opORA(c, loadM(c, eaIndLongDP(c))); break;
    case 0x17: opORA(c, loadM(c, eaIndLongDPY(c))); break;
    case 0x03: opORA(c, loadM(c, eaStackRel(c))); break;
    case 0x13: opORA(c, loadM(c, eaStackRelY(c))); break;
    case 0x0F: opORA(c, loadM(c, eaLong(c))); break;
    case 0x1F: opORA(c, loadM(c, eaLongX(c))); break;
    // ---- AND ----
    case 0x29: opAND(c, immM(c)); break;
    case 0x25: opAND(c, loadM(c, eaDP(c))); break;
    case 0x35: opAND(c, loadM(c, eaDPX(c))); break;
    case 0x2D: opAND(c, loadM(c, eaAbs(c))); break;
    case 0x3D: opAND(c, loadM(c, eaAbsX(c))); break;
    case 0x39: opAND(c, loadM(c, eaAbsY(c))); break;
    case 0x21: opAND(c, loadM(c, eaIndDPX(c))); break;
    case 0x31: opAND(c, loadM(c, eaIndDPY(c))); break;
    case 0x32: opAND(c, loadM(c, eaIndDP(c))); break;
    case 0x27: opAND(c, loadM(c, eaIndLongDP(c))); break;
    case 0x37: opAND(c, loadM(c, eaIndLongDPY(c))); break;
    case 0x23: opAND(c, loadM(c, eaStackRel(c))); break;
    case 0x33: opAND(c, loadM(c, eaStackRelY(c))); break;
    case 0x2F: opAND(c, loadM(c, eaLong(c))); break;
    case 0x3F: opAND(c, loadM(c, eaLongX(c))); break;
    // ---- EOR ----
    case 0x49: opEOR(c, immM(c)); break;
    case 0x45: opEOR(c, loadM(c, eaDP(c))); break;
    case 0x55: opEOR(c, loadM(c, eaDPX(c))); break;
    case 0x4D: opEOR(c, loadM(c, eaAbs(c))); break;
    case 0x5D: opEOR(c, loadM(c, eaAbsX(c))); break;
    case 0x59: opEOR(c, loadM(c, eaAbsY(c))); break;
    case 0x41: opEOR(c, loadM(c, eaIndDPX(c))); break;
    case 0x51: opEOR(c, loadM(c, eaIndDPY(c))); break;
    case 0x52: opEOR(c, loadM(c, eaIndDP(c))); break;
    case 0x47: opEOR(c, loadM(c, eaIndLongDP(c))); break;
    case 0x57: opEOR(c, loadM(c, eaIndLongDPY(c))); break;
    case 0x43: opEOR(c, loadM(c, eaStackRel(c))); break;
    case 0x53: opEOR(c, loadM(c, eaStackRelY(c))); break;
    case 0x4F: opEOR(c, loadM(c, eaLong(c))); break;
    case 0x5F: opEOR(c, loadM(c, eaLongX(c))); break;
    // ---- ADC ----
    case 0x69: opADC(c, immM(c)); break;
    case 0x65: opADC(c, loadM(c, eaDP(c))); break;
    case 0x75: opADC(c, loadM(c, eaDPX(c))); break;
    case 0x6D: opADC(c, loadM(c, eaAbs(c))); break;
    case 0x7D: opADC(c, loadM(c, eaAbsX(c))); break;
    case 0x79: opADC(c, loadM(c, eaAbsY(c))); break;
    case 0x61: opADC(c, loadM(c, eaIndDPX(c))); break;
    case 0x71: opADC(c, loadM(c, eaIndDPY(c))); break;
    case 0x72: opADC(c, loadM(c, eaIndDP(c))); break;
    case 0x67: opADC(c, loadM(c, eaIndLongDP(c))); break;
    case 0x77: opADC(c, loadM(c, eaIndLongDPY(c))); break;
    case 0x63: opADC(c, loadM(c, eaStackRel(c))); break;
    case 0x73: opADC(c, loadM(c, eaStackRelY(c))); break;
    case 0x6F: opADC(c, loadM(c, eaLong(c))); break;
    case 0x7F: opADC(c, loadM(c, eaLongX(c))); break;
    // ---- SBC ----
    case 0xE9: opSBC(c, immM(c)); break;
    case 0xE5: opSBC(c, loadM(c, eaDP(c))); break;
    case 0xF5: opSBC(c, loadM(c, eaDPX(c))); break;
    case 0xED: opSBC(c, loadM(c, eaAbs(c))); break;
    case 0xFD: opSBC(c, loadM(c, eaAbsX(c))); break;
    case 0xF9: opSBC(c, loadM(c, eaAbsY(c))); break;
    case 0xE1: opSBC(c, loadM(c, eaIndDPX(c))); break;
    case 0xF1: opSBC(c, loadM(c, eaIndDPY(c))); break;
    case 0xF2: opSBC(c, loadM(c, eaIndDP(c))); break;
    case 0xE7: opSBC(c, loadM(c, eaIndLongDP(c))); break;
    case 0xF7: opSBC(c, loadM(c, eaIndLongDPY(c))); break;
    case 0xE3: opSBC(c, loadM(c, eaStackRel(c))); break;
    case 0xF3: opSBC(c, loadM(c, eaStackRelY(c))); break;
    case 0xEF: opSBC(c, loadM(c, eaLong(c))); break;
    case 0xFF: opSBC(c, loadM(c, eaLongX(c))); break;
    // ---- CMP ----
    case 0xC9: opCMPv(c, c.A, immM(c), !c.m8()); break;
    case 0xC5: opCMPv(c, c.A, loadM(c, eaDP(c)), !c.m8()); break;
    case 0xD5: opCMPv(c, c.A, loadM(c, eaDPX(c)), !c.m8()); break;
    case 0xCD: opCMPv(c, c.A, loadM(c, eaAbs(c)), !c.m8()); break;
    case 0xDD: opCMPv(c, c.A, loadM(c, eaAbsX(c)), !c.m8()); break;
    case 0xD9: opCMPv(c, c.A, loadM(c, eaAbsY(c)), !c.m8()); break;
    case 0xC1: opCMPv(c, c.A, loadM(c, eaIndDPX(c)), !c.m8()); break;
    case 0xD1: opCMPv(c, c.A, loadM(c, eaIndDPY(c)), !c.m8()); break;
    case 0xD2: opCMPv(c, c.A, loadM(c, eaIndDP(c)), !c.m8()); break;
    case 0xC7: opCMPv(c, c.A, loadM(c, eaIndLongDP(c)), !c.m8()); break;
    case 0xD7: opCMPv(c, c.A, loadM(c, eaIndLongDPY(c)), !c.m8()); break;
    case 0xC3: opCMPv(c, c.A, loadM(c, eaStackRel(c)), !c.m8()); break;
    case 0xD3: opCMPv(c, c.A, loadM(c, eaStackRelY(c)), !c.m8()); break;
    case 0xCF: opCMPv(c, c.A, loadM(c, eaLong(c)), !c.m8()); break;
    case 0xDF: opCMPv(c, c.A, loadM(c, eaLongX(c)), !c.m8()); break;
    // ---- CPX / CPY ----
    case 0xE0: opCMPv(c, c.X, immX(c), !c.x8()); break;
    case 0xE4: opCMPv(c, c.X, loadM(c, eaDP(c)), !c.x8()); break;   // note: index-width load
    case 0xEC: opCMPv(c, c.X, loadM(c, eaAbs(c)), !c.x8()); break;
    case 0xC0: opCMPv(c, c.Y, immX(c), !c.x8()); break;
    case 0xC4: opCMPv(c, c.Y, loadM(c, eaDP(c)), !c.x8()); break;
    case 0xCC: opCMPv(c, c.Y, loadM(c, eaAbs(c)), !c.x8()); break;
    // ---- LDA ----
    case 0xA9: opLDA(c, immM(c)); break;
    case 0xA5: opLDA(c, loadM(c, eaDP(c))); break;
    case 0xB5: opLDA(c, loadM(c, eaDPX(c))); break;
    case 0xAD: opLDA(c, loadM(c, eaAbs(c))); break;
    case 0xBD: opLDA(c, loadM(c, eaAbsX(c))); break;
    case 0xB9: opLDA(c, loadM(c, eaAbsY(c))); break;
    case 0xA1: opLDA(c, loadM(c, eaIndDPX(c))); break;
    case 0xB1: opLDA(c, loadM(c, eaIndDPY(c))); break;
    case 0xB2: opLDA(c, loadM(c, eaIndDP(c))); break;
    case 0xA7: opLDA(c, loadM(c, eaIndLongDP(c))); break;
    case 0xB7: opLDA(c, loadM(c, eaIndLongDPY(c))); break;
    case 0xA3: opLDA(c, loadM(c, eaStackRel(c))); break;
    case 0xB3: opLDA(c, loadM(c, eaStackRelY(c))); break;
    case 0xAF: opLDA(c, loadM(c, eaLong(c))); break;
    case 0xBF: opLDA(c, loadM(c, eaLongX(c))); break;
    // ---- LDX / LDY (index width) ----
    case 0xA2: { uint16_t v = immX(c); c.X = c.x8() ? (v & 0xFF) : v; setZNx(c, c.X); } break;
    case 0xA6: { uint16_t v = loadM(c, eaDP(c)); c.X = c.x8() ? (v & 0xFF) : v; setZNx(c, c.X); } break;
    case 0xB6: { uint16_t v = loadM(c, eaDPY(c)); c.X = c.x8() ? (v & 0xFF) : v; setZNx(c, c.X); } break;
    case 0xAE: { uint16_t v = loadM(c, eaAbs(c)); c.X = c.x8() ? (v & 0xFF) : v; setZNx(c, c.X); } break;
    case 0xBE: { uint16_t v = loadM(c, eaAbsY(c)); c.X = c.x8() ? (v & 0xFF) : v; setZNx(c, c.X); } break;
    case 0xA0: { uint16_t v = immX(c); c.Y = c.x8() ? (v & 0xFF) : v; setZNx(c, c.Y); } break;
    case 0xA4: { uint16_t v = loadM(c, eaDP(c)); c.Y = c.x8() ? (v & 0xFF) : v; setZNx(c, c.Y); } break;
    case 0xB4: { uint16_t v = loadM(c, eaDPX(c)); c.Y = c.x8() ? (v & 0xFF) : v; setZNx(c, c.Y); } break;
    case 0xAC: { uint16_t v = loadM(c, eaAbs(c)); c.Y = c.x8() ? (v & 0xFF) : v; setZNx(c, c.Y); } break;
    case 0xBC: { uint16_t v = loadM(c, eaAbsX(c)); c.Y = c.x8() ? (v & 0xFF) : v; setZNx(c, c.Y); } break;
    // ---- STA ----
    case 0x85: storeM(c, eaDP(c), c.A); break;
    case 0x95: storeM(c, eaDPX(c), c.A); break;
    case 0x8D: storeM(c, eaAbs(c), c.A); break;
    case 0x9D: storeM(c, eaAbsX(c), c.A); break;
    case 0x99: storeM(c, eaAbsY(c), c.A); break;
    case 0x81: storeM(c, eaIndDPX(c), c.A); break;
    case 0x91: storeM(c, eaIndDPY(c), c.A); break;
    case 0x92: storeM(c, eaIndDP(c), c.A); break;
    case 0x87: storeM(c, eaIndLongDP(c), c.A); break;
    case 0x97: storeM(c, eaIndLongDPY(c), c.A); break;
    case 0x83: storeM(c, eaStackRel(c), c.A); break;
    case 0x93: storeM(c, eaStackRelY(c), c.A); break;
    case 0x8F: storeM(c, eaLong(c), c.A); break;
    case 0x9F: storeM(c, eaLongX(c), c.A); break;
    // ---- STX / STY (index width) ----
    case 0x86: if (c.x8()) wr8(c, eaDP(c), c.X); else wr16b(c, eaDP(c), c.X); break;
    case 0x96: if (c.x8()) wr8(c, eaDPY(c), c.X); else wr16b(c, eaDPY(c), c.X); break;
    case 0x8E: if (c.x8()) wr8(c, eaAbs(c), c.X); else wr16b(c, eaAbs(c), c.X); break;
    case 0x84: if (c.x8()) wr8(c, eaDP(c), c.Y); else wr16b(c, eaDP(c), c.Y); break;
    case 0x94: if (c.x8()) wr8(c, eaDPX(c), c.Y); else wr16b(c, eaDPX(c), c.Y); break;
    case 0x8C: if (c.x8()) wr8(c, eaAbs(c), c.Y); else wr16b(c, eaAbs(c), c.Y); break;
    // ---- STZ ----
    case 0x64: storeM(c, eaDP(c), 0); break;
    case 0x74: storeM(c, eaDPX(c), 0); break;
    case 0x9C: storeM(c, eaAbs(c), 0); break;
    case 0x9E: storeM(c, eaAbsX(c), 0); break;
    // ---- INC / DEC accumulator + memory ----
    case 0x1A: RMW_A(opINC); break;
    case 0x3A: RMW_A(opDEC); break;
    case 0xE6: RMW_M(opINC, eaDP(c)); break;
    case 0xF6: RMW_M(opINC, eaDPX(c)); break;
    case 0xEE: RMW_M(opINC, eaAbs(c)); break;
    case 0xFE: RMW_M(opINC, eaAbsX(c)); break;
    case 0xC6: RMW_M(opDEC, eaDP(c)); break;
    case 0xD6: RMW_M(opDEC, eaDPX(c)); break;
    case 0xCE: RMW_M(opDEC, eaAbs(c)); break;
    case 0xDE: RMW_M(opDEC, eaAbsX(c)); break;
    // ---- ASL / LSR / ROL / ROR ----
    case 0x0A: RMW_A(opASL); break;
    case 0x06: RMW_M(opASL, eaDP(c)); break;
    case 0x16: RMW_M(opASL, eaDPX(c)); break;
    case 0x0E: RMW_M(opASL, eaAbs(c)); break;
    case 0x1E: RMW_M(opASL, eaAbsX(c)); break;
    case 0x4A: RMW_A(opLSR); break;
    case 0x46: RMW_M(opLSR, eaDP(c)); break;
    case 0x56: RMW_M(opLSR, eaDPX(c)); break;
    case 0x4E: RMW_M(opLSR, eaAbs(c)); break;
    case 0x5E: RMW_M(opLSR, eaAbsX(c)); break;
    case 0x2A: RMW_A(opROL); break;
    case 0x26: RMW_M(opROL, eaDP(c)); break;
    case 0x36: RMW_M(opROL, eaDPX(c)); break;
    case 0x2E: RMW_M(opROL, eaAbs(c)); break;
    case 0x3E: RMW_M(opROL, eaAbsX(c)); break;
    case 0x6A: RMW_A(opROR); break;
    case 0x66: RMW_M(opROR, eaDP(c)); break;
    case 0x76: RMW_M(opROR, eaDPX(c)); break;
    case 0x6E: RMW_M(opROR, eaAbs(c)); break;
    case 0x7E: RMW_M(opROR, eaAbsX(c)); break;
    // ---- TSB / TRB ----
    case 0x04: RMW_M(opTSB, eaDP(c)); break;
    case 0x0C: RMW_M(opTSB, eaAbs(c)); break;
    case 0x14: RMW_M(opTRB, eaDP(c)); break;
    case 0x1C: RMW_M(opTRB, eaAbs(c)); break;
    // ---- BIT ----
    case 0x89: opBIT(c, immM(c), true); break;
    case 0x24: opBIT(c, loadM(c, eaDP(c)), false); break;
    case 0x34: opBIT(c, loadM(c, eaDPX(c)), false); break;
    case 0x2C: opBIT(c, loadM(c, eaAbs(c)), false); break;
    case 0x3C: opBIT(c, loadM(c, eaAbsX(c)), false); break;
    // ---- branches ----
    case 0x10: branch(c, !(c.P & P65_N)); break;   // BPL
    case 0x30: branch(c,  (c.P & P65_N)); break;   // BMI
    case 0x50: branch(c, !(c.P & P65_V)); break;   // BVC
    case 0x70: branch(c,  (c.P & P65_V)); break;   // BVS
    case 0x90: branch(c, !(c.P & P65_C)); break;   // BCC
    case 0xB0: branch(c,  (c.P & P65_C)); break;   // BCS
    case 0xD0: branch(c, !(c.P & P65_Z)); break;   // BNE
    case 0xF0: branch(c,  (c.P & P65_Z)); break;   // BEQ
    case 0x80: branch(c, true); break;             // BRA
    case 0x82: { int16_t r = (int16_t)fetch16(c); c.PC = (uint16_t)(c.PC + r); } break; // BRL
    // ---- jumps / calls ----
    case 0x4C: c.PC = fetch16(c); break;                                         // JMP abs
    case 0x6C: { uint16_t p = fetch16(c); c.PC = rd16_0(c, p); } break;          // JMP (abs)
    case 0x7C: { uint16_t p = fetch16(c) + c.X; c.PC = rd16b(c, ((uint32_t)c.PBR << 16) | p); } break; // JMP (abs,X)
    case 0x5C: { uint32_t a = fetch24(c); c.PBR = a >> 16; c.PC = a & 0xFFFF; } break;                 // JML long
    case 0xDC: { uint16_t p = fetch16(c); uint32_t a = rd24_0(c, p); c.PBR = a >> 16; c.PC = a & 0xFFFF; } break; // JML [abs]
    case 0x20: { uint16_t a = fetch16(c); push16(c, (c.PC - 1) & 0xFFFF); c.PC = a; } break;           // JSR abs
    case 0xFC: { uint16_t p = fetch16(c); push16(c, (c.PC - 1) & 0xFFFF); c.PC = rd16b(c, ((uint32_t)c.PBR << 16) | (uint16_t)(p + c.X)); } break; // JSR (abs,X)
    case 0x22: { uint32_t a = fetch24(c); push8(c, c.PBR); push16(c, (c.PC - 1) & 0xFFFF); c.PBR = a >> 16; c.PC = a & 0xFFFF; } break;            // JSL long
    case 0x60: c.PC = (pull16(c) + 1) & 0xFFFF; break;                                                  // RTS
    case 0x6B: { uint16_t pc = pull16(c); c.PBR = pull8(c); c.PC = (pc + 1) & 0xFFFF; } break;          // RTL
    case 0x40: { c.P = pull8(c); if (!c.E) { c.PC = pull16(c); c.PBR = pull8(c); } else { c.PC = pull16(c); } normalize(c); } break; // RTI
    // ---- stack ----
    case 0x48: if (c.m8()) push8(c, c.A & 0xFF); else push16(c, c.A); break;        // PHA
    case 0x68: if (c.m8()) { uint8_t v = pull8(c); c.A = (c.A & 0xFF00) | v; setZN8(c, v); } else { uint16_t v = pull16(c); c.A = v; setZN16(c, v); } break; // PLA
    case 0xDA: if (c.x8()) push8(c, c.X & 0xFF); else push16(c, c.X); break;        // PHX
    case 0xFA: if (c.x8()) { uint8_t v = pull8(c); c.X = v; setZN8(c, v); } else { uint16_t v = pull16(c); c.X = v; setZN16(c, v); } break; // PLX
    case 0x5A: if (c.x8()) push8(c, c.Y & 0xFF); else push16(c, c.Y); break;        // PHY
    case 0x7A: if (c.x8()) { uint8_t v = pull8(c); c.Y = v; setZN8(c, v); } else { uint16_t v = pull16(c); c.Y = v; setZN16(c, v); } break; // PLY
    case 0x08: push8(c, c.E ? (c.P | P65_X | 0x10) : c.P); break;                    // PHP
    case 0x28: c.P = pull8(c); normalize(c); break;                                 // PLP
    case 0x8B: push8(c, c.DBR); break;                                              // PHB
    case 0xAB: { uint8_t v = pull8(c); c.DBR = v; setZN8(c, v); } break;            // PLB
    case 0x4B: push8(c, c.PBR); break;                                              // PHK
    case 0x0B: push16(c, c.D); break;                                              // PHD
    case 0x2B: { c.D = pull16(c); setZN16(c, c.D); } break;                         // PLD
    case 0xF4: push16(c, fetch16(c)); break;                                        // PEA
    case 0xD4: { uint16_t p = dpAddr(c, fetch8(c)); push16(c, rd16_0(c, p)); } break; // PEI
    case 0x62: { int16_t r = (int16_t)fetch16(c); push16(c, (uint16_t)(c.PC + r)); } break; // PER
    // ---- transfers ----
    case 0xAA: c.X = c.x8() ? (c.A & 0xFF) : c.A; setZNx(c, c.X); break;            // TAX
    case 0xA8: c.Y = c.x8() ? (c.A & 0xFF) : c.A; setZNx(c, c.Y); break;            // TAY
    case 0x8A: if (c.m8()) { c.A = (c.A & 0xFF00) | (c.X & 0xFF); setZN8(c, c.A & 0xFF); } else { c.A = c.X; setZN16(c, c.A); } break; // TXA
    case 0x98: if (c.m8()) { c.A = (c.A & 0xFF00) | (c.Y & 0xFF); setZN8(c, c.A & 0xFF); } else { c.A = c.Y; setZN16(c, c.A); } break; // TYA
    case 0xBA: c.X = c.x8() ? (c.S & 0xFF) : c.S; setZNx(c, c.X); break;            // TSX
    case 0x9A: c.S = c.E ? (0x0100 | (c.X & 0xFF)) : c.X; break;                    // TXS
    case 0x9B: c.Y = c.x8() ? (c.X & 0xFF) : c.X; setZNx(c, c.Y); break;            // TXY
    case 0xBB: c.X = c.x8() ? (c.Y & 0xFF) : c.Y; setZNx(c, c.X); break;            // TYX
    case 0x5B: c.D = c.A; setZN16(c, c.D); break;                                   // TCD
    case 0x7B: c.A = c.D; setZN16(c, c.A); break;                                   // TDC
    case 0x1B: c.S = c.E ? (0x0100 | (c.A & 0xFF)) : c.A; break;                    // TCS
    case 0x3B: c.A = c.S; setZN16(c, c.A); break;                                   // TSC
    case 0xEB: { uint8_t lo = c.A & 0xFF, hi = c.A >> 8; c.A = ((uint16_t)lo << 8) | hi; setZN8(c, hi); } break; // XBA
    // ---- register inc/dec ----
    case 0xE8: c.X = c.x8() ? ((c.X + 1) & 0xFF) : ((c.X + 1) & 0xFFFF); setZNx(c, c.X); break; // INX
    case 0xCA: c.X = c.x8() ? ((c.X - 1) & 0xFF) : ((c.X - 1) & 0xFFFF); setZNx(c, c.X); break; // DEX
    case 0xC8: c.Y = c.x8() ? ((c.Y + 1) & 0xFF) : ((c.Y + 1) & 0xFFFF); setZNx(c, c.Y); break; // INY
    case 0x88: c.Y = c.x8() ? ((c.Y - 1) & 0xFF) : ((c.Y - 1) & 0xFFFF); setZNx(c, c.Y); break; // DEY
    // ---- flag ops ----
    case 0x18: c.P &= ~P65_C; break;   // CLC
    case 0x38: c.P |=  P65_C; break;   // SEC
    case 0x58: c.P &= ~P65_I; break;   // CLI
    case 0x78: c.P |=  P65_I; break;   // SEI
    case 0xB8: c.P &= ~P65_V; break;   // CLV
    case 0xD8: c.P &= ~P65_D; break;   // CLD
    case 0xF8: c.P |=  P65_D; break;   // SED
    case 0xC2: c.P &= ~fetch8(c); normalize(c); break;   // REP
    case 0xE2: c.P |=  fetch8(c); normalize(c); break;   // SEP
    case 0xFB: { bool oldC = (c.P & P65_C), oldE = c.E;   // XCE: swap C <-> E
                 c.E = oldC; if (oldE) c.P |= P65_C; else c.P &= ~P65_C; normalize(c); } break;
    // ---- block moves ----
    case 0x54: { uint8_t db = fetch8(c), sb = fetch8(c); c.DBR = db;     // MVN
                 uint8_t v = c.rd(((uint32_t)sb << 16) | c.X); c.wr(((uint32_t)db << 16) | c.Y, v);
                 c.X = c.x8() ? ((c.X + 1) & 0xFF) : ((c.X + 1) & 0xFFFF);
                 c.Y = c.x8() ? ((c.Y + 1) & 0xFF) : ((c.Y + 1) & 0xFFFF);
                 c.A = (c.A - 1) & 0xFFFF; if (c.A != 0xFFFF) c.PC = (c.PC - 3) & 0xFFFF; } break;
    case 0x44: { uint8_t db = fetch8(c), sb = fetch8(c); c.DBR = db;     // MVP
                 uint8_t v = c.rd(((uint32_t)sb << 16) | c.X); c.wr(((uint32_t)db << 16) | c.Y, v);
                 c.X = c.x8() ? ((c.X - 1) & 0xFF) : ((c.X - 1) & 0xFFFF);
                 c.Y = c.x8() ? ((c.Y - 1) & 0xFF) : ((c.Y - 1) & 0xFFFF);
                 c.A = (c.A - 1) & 0xFFFF; if (c.A != 0xFFFF) c.PC = (c.PC - 3) & 0xFFFF; } break;
    // ---- interrupts / misc ----
    case 0x00: { fetch8(c);                                              // BRK (+ signature byte)
                 if (!c.E) { push8(c, c.PBR); push16(c, c.PC); push8(c, c.P); c.PBR = 0; c.P |= P65_I; c.P &= ~P65_D; c.PC = rd16_0(c, 0xFFE6); }
                 else { push16(c, c.PC); push8(c, c.P | 0x10); c.PBR = 0; c.P |= P65_I; c.P &= ~P65_D; c.PC = rd16_0(c, 0xFFFE); } } break;
    case 0x02: { fetch8(c);                                              // COP (+ signature byte)
                 if (!c.E) { push8(c, c.PBR); push16(c, c.PC); push8(c, c.P); c.PBR = 0; c.P |= P65_I; c.P &= ~P65_D; c.PC = rd16_0(c, 0xFFE4); }
                 else { push16(c, c.PC); push8(c, c.P); c.PBR = 0; c.P |= P65_I; c.P &= ~P65_D; c.PC = rd16_0(c, 0xFFF4); } } break;
    case 0xDB: c.stopped = true; break;     // STP
    case 0xCB: c.waiting = true; break;     // WAI (treated as a benign nop here)
    case 0x42: fetch8(c); break;            // WDM (2-byte nop)
    case 0xEA: break;                       // NOP
    default: break;                         // (every 65816 opcode is defined; nothing illegal)
  }
  cycles += 2;
  return 2;
}
