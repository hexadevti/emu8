// z80.cpp - Zilog Z80 CPU core (see z80.h). Instruction-stepped, full documented + undocumented
// behaviour (X/Y flags, IXH/IXL, SLL, MEMPTR/WZ for BIT n,(HL)/(IX+d)), all four prefix tables
// (CB / ED / DD / FD / DDCB / FDCB), and IM 0/1/2 + NMI.
//
// Cycle counts are APPROXIMATE: good enough for the MSX1 batch-paced
// loop, not cycle-exact. The MEMPTR register and undocumented flags ARE exact so ZEXALL passes.
//
// The decoder uses the classic index-substitution trick for DD/FD: when a prefix is active, HL maps
// to IX/IY, the H/L register operands map to IXH/IXL (UNLESS the instruction also touches (HL), in
// which case (HL) becomes (IX+d) and the other register operand stays the real H/L).

#include "z80_core.h"

// ---- precomputed flag tables -------------------------------------------------------------------
// Built at compile time so they sit in flash with the rest of the core rather than as 768 bytes of
// .bss -- which every board pays on every boot, Z80 machine or not, and which on the PicoCalc's
// RP2040 comes straight out of the heap the Apple IIe map needs. The interpreter itself executes
// from flash there too, so reading its tables from flash costs nothing extra. Declared in
// z80_core.h and shared by every Z80Core<Bus> instantiation.
//
// C++11-compatible on purpose: Arduino-ESP32 2.0.x (CYD / JC4827W543) compiles with -std=gnu++11,
// where a constexpr function cannot contain a loop. So each entry is a single-expression constexpr
// and the 256-entry arrays are expanded from a hand-rolled index pack (std::index_sequence is C++14).
static constexpr uint8_t z80Parity(int i) {
  return ((i ^ (i >> 1) ^ (i >> 2) ^ (i >> 3) ^ (i >> 4) ^ (i >> 5) ^ (i >> 6) ^ (i >> 7)) & 1) ? 0 : Z80_PF;
}
static constexpr uint8_t z80Sz53(int i) {
  return (uint8_t)((i & (Z80_SF | Z80_YF | Z80_XF)) | (i == 0 ? Z80_ZF : 0));
}
template <int... I> struct Z80Seq {};
template <int N, int... I> struct Z80MakeSeq : Z80MakeSeq<N - 1, N - 1, I...> {};
template <int... I> struct Z80MakeSeq<0, I...> { typedef Z80Seq<I...> type; };

template <int... I> static constexpr Z80FlagTables makeTables(Z80Seq<I...>) {
  return Z80FlagTables{ { z80Sz53(I)... },
                        { (uint8_t)(z80Sz53(I) | z80Parity(I))... },
                        { z80Parity(I)... } };
}
constexpr Z80FlagTables z80Flags = makeTables(Z80MakeSeq<256>::type());

// ---- pointer-bus instantiation ------------------------------------------------------------------
// Z80::step()/irq()/nmi() go through the rd/wr/in/out hooks. The machines use their own inline bus
// (Z80Core<TheirBus>, see z80_core.h); this generic path serves the host harnesses.
struct Z80PtrBus {
  static inline uint8_t rd (Z80& c, uint16_t a)            { return c.rd(a); }
  static inline void    wr (Z80& c, uint16_t a, uint8_t v) { c.wr(a, v); }
  static inline uint8_t in (Z80& c, uint16_t p)            { return c.in(p); }
  static inline void    out(Z80& c, uint16_t p, uint8_t v) { c.out(p, v); }
};

void Z80::reset() {
  A = F = B = C = D = E = H = L = 0;
  A_ = F_ = B_ = C_ = D_ = E_ = H_ = L_ = 0;
  IXH = IXL = IYH = IYL = 0;
  I = R = R7 = 0;
  SP = 0xFFFF; PC = 0x0000; MEMPTR = 0;
  IFF1 = IFF2 = false; IM = 0; halted = false; eiPending = 0;
  cycles = 0;
  _eiBlock = false;
}

int  Z80::step()           { return Z80Core<Z80PtrBus>::step(*this); }
bool Z80::irq(uint8_t bus) { return Z80Core<Z80PtrBus>::irq(*this, bus); }
void Z80::nmi()            { Z80Core<Z80PtrBus>::nmi(*this); }
