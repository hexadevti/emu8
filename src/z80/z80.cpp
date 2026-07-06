// z80.cpp - Zilog Z80 CPU core (see z80.h). Instruction-stepped, full documented + undocumented
// behaviour (X/Y flags, IXH/IXL, SLL, MEMPTR/WZ for BIT n,(HL)/(IX+d)), all four prefix tables
// (CB / ED / DD / FD / DDCB / FDCB), and IM 0/1/2 + NMI.
//
// Cycle counts are APPROXIMATE (like src/iigs/cpu65816.cpp): good enough for the MSX1 batch-paced
// loop, not cycle-exact. The MEMPTR register and undocumented flags ARE exact so ZEXALL passes.
//
// The decoder uses the classic index-substitution trick for DD/FD: when a prefix is active, HL maps
// to IX/IY, the H/L register operands map to IXH/IXL (UNLESS the instruction also touches (HL), in
// which case (HL) becomes (IX+d) and the other register operand stays the real H/L).

#include "z80.h"

// ---- precomputed flag tables -------------------------------------------------------------------
static uint8_t sz53[256];    // S, Z, Y(bit5), X(bit3) from a byte value
static uint8_t sz53p[256];   // sz53 + parity
static uint8_t parityT[256]; // PF set when the byte has even parity
static bool    tablesReady = false;

static void initTables() {
  for (int i = 0; i < 256; i++) {
    uint8_t p = (uint8_t)i;
    p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    parityT[i] = (p & 1) ? 0 : Z80_PF;
    uint8_t sz = (uint8_t)i & (Z80_SF | Z80_YF | Z80_XF);
    if (i == 0) sz |= Z80_ZF;
    sz53[i]  = sz;
    sz53p[i] = sz | parityT[i];
  }
  tablesReady = true;
}

// ---- low-level fetch / stack -------------------------------------------------------------------
static inline void incR(Z80& c) { c.R = (c.R & 0x80) | ((c.R + 1) & 0x7F); }
static inline uint8_t fetchOp(Z80& c) { uint8_t v = c.rd(c.PC++); incR(c); return v; }   // M1 fetch
static inline uint8_t imm(Z80& c)     { return c.rd(c.PC++); }                            // operand byte
static inline uint16_t imm16(Z80& c)  { uint8_t lo = imm(c); uint8_t hi = imm(c); return lo | (hi << 8); }
static inline void push16(Z80& c, uint16_t v) { c.wr(--c.SP, v >> 8); c.wr(--c.SP, (uint8_t)v); }
static inline uint16_t pop16(Z80& c) { uint8_t lo = c.rd(c.SP++); uint8_t hi = c.rd(c.SP++); return lo | (hi << 8); }

// ---- 8-bit register access (honours DD/FD index substitution for codes 4/5 = H/L) --------------
// code: 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A.  idx: 0=HL 1=IX 2=IY.  Never call with code 6.
static uint8_t& reg8(Z80& c, int code, int idx) {
  switch (code) {
    case 0: return c.B; case 1: return c.C; case 2: return c.D; case 3: return c.E;
    case 4: return (idx == 1) ? c.IXH : (idx == 2) ? c.IYH : c.H;
    case 5: return (idx == 1) ? c.IXL : (idx == 2) ? c.IYL : c.L;
    case 7: return c.A;
  }
  return c.A;
}

// 16-bit pair (rp) for LD rp,nn / INC/DEC rp / ADD HL,rp.  p: 0=BC 1=DE 2=HL/idx 3=SP
static uint16_t rpGet(Z80& c, int p, int idx) {
  switch (p) { case 0: return c.BC(); case 1: return c.DE();
               case 2: return (idx == 1) ? c.IX() : (idx == 2) ? c.IY() : c.HL();
               default: return c.SP; }
}
static void rpSet(Z80& c, int p, int idx, uint16_t v) {
  switch (p) { case 0: c.setBC(v); break; case 1: c.setDE(v); break;
               case 2: if (idx == 1) c.setIX(v); else if (idx == 2) c.setIY(v); else c.setHL(v); break;
               default: c.SP = v; }
}
// 16-bit pair for PUSH/POP.  p: 0=BC 1=DE 2=HL/idx 3=AF
static uint16_t pp2Get(Z80& c, int p, int idx) {
  switch (p) { case 0: return c.BC(); case 1: return c.DE();
               case 2: return (idx == 1) ? c.IX() : (idx == 2) ? c.IY() : c.HL();
               default: return c.AF(); }
}
static void pp2Set(Z80& c, int p, int idx, uint16_t v) {
  switch (p) { case 0: c.setBC(v); break; case 1: c.setDE(v); break;
               case 2: if (idx == 1) c.setIX(v); else if (idx == 2) c.setIY(v); else c.setHL(v); break;
               default: c.setAF(v); }
}
static uint16_t idx16(Z80& c, int idx) { return (idx == 1) ? c.IX() : (idx == 2) ? c.IY() : c.HL(); }

// (HL) / (IX+d) / (IY+d) effective address.  Fetches the displacement byte when a prefix is active
// and updates MEMPTR (its high byte drives the X/Y flags of BIT n,(IX+d)).
static uint16_t eaHL(Z80& c, int idx) {
  if (idx == 0) return c.HL();
  int8_t d = (int8_t)imm(c);
  uint16_t ea = (uint16_t)(((idx == 1) ? c.IX() : c.IY()) + d);
  c.MEMPTR = ea;
  return ea;
}

// ---- ALU on the accumulator --------------------------------------------------------------------
static void add8(Z80& c, uint8_t v) {
  uint16_t r = c.A + v;
  c.F = (r & 0xFF) ? 0 : Z80_ZF;
  c.F |= r & Z80_SF;
  c.F |= (c.A ^ v ^ r) & Z80_HF;
  c.F |= (((c.A ^ r) & (v ^ r) & 0x80) >> 5);   // overflow -> PF
  c.F |= (r >> 8) & Z80_CF;
  c.F |= r & (Z80_XF | Z80_YF);
  c.A = (uint8_t)r;
}
static void adc8(Z80& c, uint8_t v) {
  uint16_t carry = c.F & Z80_CF;
  uint16_t r = c.A + v + carry;
  c.F = (r & 0xFF) ? 0 : Z80_ZF;
  c.F |= r & Z80_SF;
  c.F |= (c.A ^ v ^ r) & Z80_HF;
  c.F |= (((c.A ^ r) & (v ^ r) & 0x80) >> 5);
  c.F |= (r >> 8) & Z80_CF;
  c.F |= r & (Z80_XF | Z80_YF);
  c.A = (uint8_t)r;
}
static void sub8(Z80& c, uint8_t v) {
  uint16_t r = c.A - v;
  c.F = Z80_NF | ((r & 0xFF) ? 0 : Z80_ZF);
  c.F |= r & Z80_SF;
  c.F |= (c.A ^ v ^ r) & Z80_HF;
  c.F |= (((c.A ^ v) & (c.A ^ r) & 0x80) >> 5);
  c.F |= (r >> 8) & Z80_CF;
  c.F |= r & (Z80_XF | Z80_YF);
  c.A = (uint8_t)r;
}
static void sbc8(Z80& c, uint8_t v) {
  uint16_t carry = c.F & Z80_CF;
  uint16_t r = c.A - v - carry;
  c.F = Z80_NF | ((r & 0xFF) ? 0 : Z80_ZF);
  c.F |= r & Z80_SF;
  c.F |= (c.A ^ v ^ r) & Z80_HF;
  c.F |= (((c.A ^ v) & (c.A ^ r) & 0x80) >> 5);
  c.F |= (r >> 8) & Z80_CF;
  c.F |= r & (Z80_XF | Z80_YF);
  c.A = (uint8_t)r;
}
static void and8(Z80& c, uint8_t v) { c.A &= v; c.F = sz53p[c.A] | Z80_HF; }
static void xor8(Z80& c, uint8_t v) { c.A ^= v; c.F = sz53p[c.A]; }
static void or8 (Z80& c, uint8_t v) { c.A |= v; c.F = sz53p[c.A]; }
static void cp8 (Z80& c, uint8_t v) {            // like SUB but A unchanged; X/Y come from operand v
  uint16_t r = c.A - v;
  c.F = Z80_NF | ((r & 0xFF) ? 0 : Z80_ZF);
  c.F |= r & Z80_SF;
  c.F |= (c.A ^ v ^ r) & Z80_HF;
  c.F |= (((c.A ^ v) & (c.A ^ r) & 0x80) >> 5);
  c.F |= (r >> 8) & Z80_CF;
  c.F |= v & (Z80_XF | Z80_YF);
}
static void alu(Z80& c, int op, uint8_t v) {
  switch (op) { case 0: add8(c, v); break; case 1: adc8(c, v); break; case 2: sub8(c, v); break;
                case 3: sbc8(c, v); break; case 4: and8(c, v); break; case 5: xor8(c, v); break;
                case 6: or8(c, v); break; default: cp8(c, v); }
}
static void inc8(Z80& c, uint8_t& r) {
  r++;
  c.F = (c.F & Z80_CF) | sz53[r];
  c.F |= (r == 0x80) ? Z80_VF : 0;
  c.F |= ((r & 0x0F) == 0) ? Z80_HF : 0;
}
static void dec8(Z80& c, uint8_t& r) {
  c.F = (c.F & Z80_CF) | Z80_NF;
  c.F |= ((r & 0x0F) == 0) ? Z80_HF : 0;
  r--;
  c.F |= sz53[r];
  c.F |= (r == 0x7F) ? Z80_VF : 0;
}

// ---- 16-bit ALU --------------------------------------------------------------------------------
static void add16(Z80& c, int idx, uint16_t val) {
  uint16_t hl = idx16(c, idx);
  uint32_t r = (uint32_t)hl + val;
  c.F = (c.F & (Z80_SF | Z80_ZF | Z80_VF));
  c.F |= ((hl ^ val ^ r) >> 8) & Z80_HF;
  c.F |= (r >> 16) & Z80_CF;
  c.F |= (r >> 8) & (Z80_XF | Z80_YF);
  if (idx == 1) c.setIX((uint16_t)r); else if (idx == 2) c.setIY((uint16_t)r); else c.setHL((uint16_t)r);
  c.MEMPTR = hl + 1;
}
static void adc16(Z80& c, uint16_t val) {
  uint16_t hl = c.HL();
  uint32_t carry = c.F & Z80_CF;
  uint32_t r = (uint32_t)hl + val + carry;
  c.F = (r & 0x8000) ? Z80_SF : 0;
  c.F |= (r & 0xFFFF) ? 0 : Z80_ZF;
  c.F |= ((hl ^ val ^ r) >> 8) & Z80_HF;
  c.F |= ((~(hl ^ val) & (hl ^ r) & 0x8000) >> 13);   // overflow -> PF
  c.F |= (r >> 16) & Z80_CF;
  c.F |= (r >> 8) & (Z80_XF | Z80_YF);
  c.setHL((uint16_t)r);
  c.MEMPTR = hl + 1;
}
static void sbc16(Z80& c, uint16_t val) {
  uint16_t hl = c.HL();
  uint32_t carry = c.F & Z80_CF;
  uint32_t r = (uint32_t)hl - val - carry;
  c.F = Z80_NF | ((r & 0x8000) ? Z80_SF : 0);
  c.F |= (r & 0xFFFF) ? 0 : Z80_ZF;
  c.F |= ((hl ^ val ^ r) >> 8) & Z80_HF;
  c.F |= (((hl ^ val) & (hl ^ r) & 0x8000) >> 13);    // overflow -> PF
  c.F |= (r >> 16) & Z80_CF;
  c.F |= (r >> 8) & (Z80_XF | Z80_YF);
  c.setHL((uint16_t)r);
  c.MEMPTR = hl + 1;
}

// ---- misc accumulator ops ----------------------------------------------------------------------
static void daa(Z80& c) {
  uint8_t a = c.A;
  if (c.F & Z80_NF) {
    if ((c.F & Z80_HF) || ((a & 0x0F) > 9)) a -= 6;
    if ((c.F & Z80_CF) || (c.A > 0x99))     a -= 0x60;
  } else {
    if ((c.F & Z80_HF) || ((a & 0x0F) > 9)) a += 6;
    if ((c.F & Z80_CF) || (c.A > 0x99))     a += 0x60;
  }
  c.F = (c.F & (Z80_CF | Z80_NF)) | ((c.A > 0x99) ? Z80_CF : 0) | ((c.A ^ a) & Z80_HF) | sz53p[a];
  c.A = a;
}
static void cpl(Z80& c) { c.A = ~c.A; c.F = (c.F & (Z80_SF|Z80_ZF|Z80_PF|Z80_CF)) | Z80_HF | Z80_NF | (c.A & (Z80_XF|Z80_YF)); }
static void scf(Z80& c) { c.F = (c.F & (Z80_SF|Z80_ZF|Z80_PF)) | Z80_CF | (c.A & (Z80_XF|Z80_YF)); }
static void ccf(Z80& c) {
  uint8_t oldc = c.F & Z80_CF;
  c.F = (c.F & (Z80_SF|Z80_ZF|Z80_PF)) | (oldc ? Z80_HF : 0) | (oldc ? 0 : Z80_CF) | (c.A & (Z80_XF|Z80_YF));
}

// accumulator rotates (only C/H/N + X/Y change; S/Z/P preserved)
static void rlca(Z80& c) { c.A = (c.A << 1) | (c.A >> 7); c.F = (c.F & (Z80_SF|Z80_ZF|Z80_PF)) | (c.A & (Z80_XF|Z80_YF|Z80_CF)); }
static void rrca(Z80& c) { uint8_t b = c.A & 1; c.A = (c.A >> 1) | (b << 7); c.F = (c.F & (Z80_SF|Z80_ZF|Z80_PF)) | (c.A & (Z80_XF|Z80_YF)) | b; }
static void rla (Z80& c) { uint8_t nc = c.A >> 7; c.A = (c.A << 1) | (c.F & Z80_CF); c.F = (c.F & (Z80_SF|Z80_ZF|Z80_PF)) | (c.A & (Z80_XF|Z80_YF)) | nc; }
static void rra (Z80& c) { uint8_t nc = c.A & 1; c.A = (c.A >> 1) | ((c.F & Z80_CF) << 7); c.F = (c.F & (Z80_SF|Z80_ZF|Z80_PF)) | (c.A & (Z80_XF|Z80_YF)) | nc; }

// CB rotate/shift on a value (y selects the op); sets full flags
static uint8_t doRot(Z80& c, int y, uint8_t v) {
  uint8_t cy;
  switch (y) {
    case 0: cy = v >> 7; v = (v << 1) | cy; break;                 // RLC
    case 1: cy = v & 1;  v = (v >> 1) | (cy << 7); break;          // RRC
    case 2: cy = v >> 7; v = (v << 1) | (c.F & Z80_CF); break;     // RL
    case 3: cy = v & 1;  v = (v >> 1) | ((c.F & Z80_CF) << 7); break; // RR
    case 4: cy = v >> 7; v = v << 1; break;                        // SLA
    case 5: cy = v & 1;  v = (v & 0x80) | (v >> 1); break;         // SRA
    case 6: cy = v >> 7; v = (v << 1) | 1; break;                  // SLL (undocumented)
    default: cy = v & 1; v = v >> 1; break;                        // SRL
  }
  c.F = sz53p[v] | cy;
  return v;
}

// ---- block instructions ------------------------------------------------------------------------
static void ldStep(Z80& c, int dir) {           // LDI(+1) / LDD(-1)
  uint8_t v = c.rd(c.HL());
  c.wr(c.DE(), v);
  c.setHL(c.HL() + dir); c.setDE(c.DE() + dir); c.setBC(c.BC() - 1);
  uint8_t n = v + c.A;
  c.F = (c.F & (Z80_SF | Z80_ZF | Z80_CF)) | (n & Z80_XF) | ((n & 0x02) ? Z80_YF : 0) | (c.BC() ? Z80_PF : 0);
}
static void cpStep(Z80& c, int dir) {            // CPI(+1) / CPD(-1)
  uint8_t v = c.rd(c.HL());
  uint8_t r = c.A - v;
  uint8_t hf = (c.A ^ v ^ r) & Z80_HF;
  c.setHL(c.HL() + dir); c.setBC(c.BC() - 1);
  uint8_t n = r - (hf ? 1 : 0);
  c.F = (c.F & Z80_CF) | Z80_NF | (r & Z80_SF) | (r ? 0 : Z80_ZF) | hf
      | (n & Z80_XF) | ((n & 0x02) ? Z80_YF : 0) | (c.BC() ? Z80_PF : 0);
  c.MEMPTR += dir;
}
static void inStep(Z80& c, int dir) {            // INI(+1) / IND(-1)
  uint8_t v = c.in(c.BC());
  c.MEMPTR = c.BC() + dir;
  c.wr(c.HL(), v);
  c.B--;
  c.setHL(c.HL() + dir);
  uint16_t k = (uint16_t)v + (uint8_t)((c.C + dir) & 0xFF);
  c.F = sz53[c.B] | ((v & 0x80) ? Z80_NF : 0) | ((k > 0xFF) ? (Z80_HF | Z80_CF) : 0)
      | parityT[((uint8_t)(k & 7) ^ c.B) & 0xFF];
}
static void outStep(Z80& c, int dir) {           // OUTI(+1) / OUTD(-1)
  uint8_t v = c.rd(c.HL());
  c.B--;
  c.out(c.BC(), v);
  c.setHL(c.HL() + dir);
  c.MEMPTR = c.BC() + dir;
  uint16_t k = (uint16_t)v + c.L;
  c.F = sz53[c.B] | ((v & 0x80) ? Z80_NF : 0) | ((k > 0xFF) ? (Z80_HF | Z80_CF) : 0)
      | parityT[((uint8_t)(k & 7) ^ c.B) & 0xFF];
}

// ---- condition codes ---------------------------------------------------------------------------
static bool cond(Z80& c, int cc) {   // 0 NZ,1 Z,2 NC,3 C,4 PO,5 PE,6 P,7 M
  switch (cc) {
    case 0: return !(c.F & Z80_ZF); case 1: return  (c.F & Z80_ZF);
    case 2: return !(c.F & Z80_CF); case 3: return  (c.F & Z80_CF);
    case 4: return !(c.F & Z80_PF); case 5: return  (c.F & Z80_PF);
    case 6: return !(c.F & Z80_SF); default: return (c.F & Z80_SF);
  }
}

// ---- CB / DDCB / FDCB --------------------------------------------------------------------------
static void execCB(Z80& c, int idx) {
  uint16_t ea = 0; uint8_t cbop;
  if (idx) { int8_t d = (int8_t)imm(c); ea = (uint16_t)(((idx == 1) ? c.IX() : c.IY()) + d); c.MEMPTR = ea; cbop = imm(c); }
  else     { cbop = fetchOp(c); }
  int reg = cbop & 7;
  int type = (cbop >> 6) & 3;     // 0 rot/shift, 1 BIT, 2 RES, 3 SET
  int y = (cbop >> 3) & 7;
  bool mem = idx || (reg == 6);
  if (!idx && reg == 6) ea = c.HL();
  uint8_t v = mem ? c.rd(ea) : reg8(c, reg, 0);

  if (type == 1) {                // BIT y
    uint8_t bit = v & (1 << y);
    c.F = (c.F & Z80_CF) | Z80_HF | (bit & Z80_SF) | (bit ? 0 : (Z80_ZF | Z80_PF));
    if (mem) c.F |= (c.MEMPTR >> 8) & (Z80_XF | Z80_YF);   // (HL)/(IX+d): X/Y from MEMPTR high
    else     c.F |= v & (Z80_XF | Z80_YF);
    c.cycles += idx ? 20 : (reg == 6 ? 12 : 8);
    return;
  }
  uint8_t res;
  if (type == 0)      res = doRot(c, y, v);
  else if (type == 2) res = v & ~(1 << y);     // RES
  else                res = v | (1 << y);      // SET
  if (mem) {
    c.wr(ea, res);
    if (idx && reg != 6) reg8(c, reg, 0) = res;  // undocumented DDCB: also store into the register
    c.cycles += idx ? 23 : 15;
  } else {
    reg8(c, reg, 0) = res;
    c.cycles += 8;
  }
}

// ---- ED ----------------------------------------------------------------------------------------
static void execED(Z80& c) {
  uint8_t op = fetchOp(c);
  switch (op) {
    // IN r,(C) / OUT (C),r
    case 0x40: case 0x48: case 0x50: case 0x58: case 0x60: case 0x68: case 0x70: case 0x78: {
      int r = (op >> 3) & 7; uint8_t v = c.in(c.BC()); c.MEMPTR = c.BC() + 1;
      c.F = (c.F & Z80_CF) | sz53p[v];
      if (r != 6) reg8(c, r, 0) = v;       // 0x70 = IN (C): only flags
      c.cycles += 12; break;
    }
    case 0x41: case 0x49: case 0x51: case 0x59: case 0x61: case 0x69: case 0x71: case 0x79: {
      int r = (op >> 3) & 7; c.out(c.BC(), (r == 6) ? 0 : reg8(c, r, 0)); c.MEMPTR = c.BC() + 1;
      c.cycles += 12; break;
    }
    // SBC/ADC HL,rp
    case 0x42: case 0x52: case 0x62: case 0x72: sbc16(c, rpGet(c, (op >> 4) & 3, 0)); c.cycles += 15; break;
    case 0x4A: case 0x5A: case 0x6A: case 0x7A: adc16(c, rpGet(c, (op >> 4) & 3, 0)); c.cycles += 15; break;
    // LD (nn),rp  /  LD rp,(nn)
    case 0x43: case 0x53: case 0x63: case 0x73: {
      uint16_t nn = imm16(c); uint16_t v = rpGet(c, (op >> 4) & 3, 0);
      c.wr(nn, (uint8_t)v); c.wr(nn + 1, v >> 8); c.MEMPTR = nn + 1; c.cycles += 20; break;
    }
    case 0x4B: case 0x5B: case 0x6B: case 0x7B: {
      uint16_t nn = imm16(c); uint16_t v = c.rd(nn) | (c.rd(nn + 1) << 8);
      rpSet(c, (op >> 4) & 3, 0, v); c.MEMPTR = nn + 1; c.cycles += 20; break;
    }
    // NEG (all 0x44/4C/54/5C/64/6C/74/7C)
    case 0x44: case 0x4C: case 0x54: case 0x5C: case 0x64: case 0x6C: case 0x74: case 0x7C: {
      uint8_t t = c.A; c.A = 0; sub8(c, t); c.cycles += 8; break;
    }
    // RETN / RETI
    case 0x45: case 0x55: case 0x65: case 0x75: case 0x4D: case 0x5D: case 0x6D: case 0x7D:
      c.IFF1 = c.IFF2; c.PC = pop16(c); c.MEMPTR = c.PC; c.cycles += 14; break;
    // IM 0/1/2
    case 0x46: case 0x4E: case 0x66: case 0x6E: c.IM = 0; c.cycles += 8; break;
    case 0x56: case 0x76:                       c.IM = 1; c.cycles += 8; break;
    case 0x5E: case 0x7E:                       c.IM = 2; c.cycles += 8; break;
    // LD I,A / LD R,A / LD A,I / LD A,R
    case 0x47: c.I = c.A; c.cycles += 9; break;
    case 0x4F: c.R = c.A & 0x7F; c.R7 = c.A & 0x80; c.cycles += 9; break;
    case 0x57: c.A = c.I; c.F = (c.F & Z80_CF) | sz53[c.A] | (c.IFF2 ? Z80_PF : 0); c.cycles += 9; break;
    case 0x5F: c.A = (c.R & 0x7F) | c.R7; c.F = (c.F & Z80_CF) | sz53[c.A] | (c.IFF2 ? Z80_PF : 0); c.cycles += 9; break;
    // RRD / RLD
    case 0x67: { uint8_t m = c.rd(c.HL()); uint8_t nm = (m >> 4) | (c.A << 4);
                 c.A = (c.A & 0xF0) | (m & 0x0F); c.wr(c.HL(), nm);
                 c.F = (c.F & Z80_CF) | sz53p[c.A]; c.MEMPTR = c.HL() + 1; c.cycles += 18; break; }
    case 0x6F: { uint8_t m = c.rd(c.HL()); uint8_t nm = (m << 4) | (c.A & 0x0F);
                 c.A = (c.A & 0xF0) | (m >> 4); c.wr(c.HL(), nm);
                 c.F = (c.F & Z80_CF) | sz53p[c.A]; c.MEMPTR = c.HL() + 1; c.cycles += 18; break; }
    // block transfer / search
    case 0xA0: ldStep(c, +1); c.cycles += 16; break;
    case 0xA8: ldStep(c, -1); c.cycles += 16; break;
    case 0xA1: cpStep(c, +1); c.cycles += 16; break;
    case 0xA9: cpStep(c, -1); c.cycles += 16; break;
    case 0xA2: inStep(c, +1); c.cycles += 16; break;
    case 0xAA: inStep(c, -1); c.cycles += 16; break;
    case 0xA3: outStep(c, +1); c.cycles += 16; break;
    case 0xAB: outStep(c, -1); c.cycles += 16; break;
    case 0xB0: ldStep(c, +1); if (c.BC()) { c.PC -= 2; c.MEMPTR = c.PC + 1; c.cycles += 21; } else c.cycles += 16; break;
    case 0xB8: ldStep(c, -1); if (c.BC()) { c.PC -= 2; c.MEMPTR = c.PC + 1; c.cycles += 21; } else c.cycles += 16; break;
    case 0xB1: cpStep(c, +1); if (c.BC() && !(c.F & Z80_ZF)) { c.PC -= 2; c.MEMPTR = c.PC + 1; c.cycles += 21; } else c.cycles += 16; break;
    case 0xB9: cpStep(c, -1); if (c.BC() && !(c.F & Z80_ZF)) { c.PC -= 2; c.MEMPTR = c.PC + 1; c.cycles += 21; } else c.cycles += 16; break;
    case 0xB2: inStep(c, +1); if (c.B) { c.PC -= 2; c.cycles += 21; } else c.cycles += 16; break;
    case 0xBA: inStep(c, -1); if (c.B) { c.PC -= 2; c.cycles += 21; } else c.cycles += 16; break;
    case 0xB3: outStep(c, +1); if (c.B) { c.PC -= 2; c.cycles += 21; } else c.cycles += 16; break;
    case 0xBB: outStep(c, -1); if (c.B) { c.PC -= 2; c.cycles += 21; } else c.cycles += 16; break;
    default: c.cycles += 8; break;   // undefined ED opcode = NONI + NOP
  }
}

// ---- main table (op already fetched; idx = 0/1/2 for none/IX/IY) --------------------------------
static void execMain(Z80& c, uint8_t op, int idx, bool* wasEI) {
  // INC r / DEC r / LD r,n share a regular bit pattern within 0x00-0x3F
  if (op < 0x40) {
    if ((op & 0xC7) == 0x04) {            // INC r
      int r = (op >> 3) & 7;
      if (r == 6) { uint16_t ea = eaHL(c, idx); uint8_t v = c.rd(ea); inc8(c, v); c.wr(ea, v); c.cycles += 11; }
      else { inc8(c, reg8(c, r, idx)); c.cycles += 4; }
      return;
    }
    if ((op & 0xC7) == 0x05) {            // DEC r
      int r = (op >> 3) & 7;
      if (r == 6) { uint16_t ea = eaHL(c, idx); uint8_t v = c.rd(ea); dec8(c, v); c.wr(ea, v); c.cycles += 11; }
      else { dec8(c, reg8(c, r, idx)); c.cycles += 4; }
      return;
    }
    if ((op & 0xC7) == 0x06) {            // LD r,n
      int r = (op >> 3) & 7;
      if (r == 6) { uint16_t ea = eaHL(c, idx); uint8_t n = imm(c); c.wr(ea, n); c.cycles += 10; }
      else { reg8(c, r, idx) = imm(c); c.cycles += 7; }
      return;
    }
  }

  if (op >= 0x40 && op <= 0x7F) {         // LD r,r'  (0x76 = HALT)
    if (op == 0x76) { c.halted = true; c.cycles += 4; return; }
    int dst = (op >> 3) & 7, src = op & 7;
    if (src == 6 || dst == 6) {
      uint16_t ea = eaHL(c, idx);
      if (src == 6) reg8(c, dst, 0) = c.rd(ea);
      else          c.wr(ea, reg8(c, src, 0));
      c.cycles += 7;
    } else {
      reg8(c, dst, idx) = reg8(c, src, idx);
      c.cycles += 4;
    }
    return;
  }

  if (op >= 0x80 && op <= 0xBF) {         // ALU A,r
    int sub = (op >> 3) & 7, r = op & 7; uint8_t v;
    if (r == 6) { uint16_t ea = eaHL(c, idx); v = c.rd(ea); c.cycles += 7; }
    else        { v = reg8(c, r, idx); c.cycles += 4; }
    alu(c, sub, v);
    return;
  }

  switch (op) {
    case 0x00: c.cycles += 4; break;                                            // NOP
    case 0x08: { uint8_t t; t = c.A; c.A = c.A_; c.A_ = t; t = c.F; c.F = c.F_; c.F_ = t; c.cycles += 4; break; } // EX AF,AF'
    case 0x10: { int8_t d = (int8_t)imm(c); c.B--; if (c.B) { c.PC += d; c.MEMPTR = c.PC; c.cycles += 13; } else c.cycles += 8; break; } // DJNZ
    case 0x18: { int8_t d = (int8_t)imm(c); c.PC += d; c.MEMPTR = c.PC; c.cycles += 12; break; }                  // JR
    case 0x20: case 0x28: case 0x30: case 0x38: {                               // JR cc
      int cc = (op >> 3) & 3; int8_t d = (int8_t)imm(c);
      if (cond(c, cc)) { c.PC += d; c.MEMPTR = c.PC; c.cycles += 12; } else c.cycles += 7; break;
    }
    case 0x07: rlca(c); c.cycles += 4; break;
    case 0x0F: rrca(c); c.cycles += 4; break;
    case 0x17: rla(c);  c.cycles += 4; break;
    case 0x1F: rra(c);  c.cycles += 4; break;
    case 0x27: daa(c);  c.cycles += 4; break;
    case 0x2F: cpl(c);  c.cycles += 4; break;
    case 0x37: scf(c);  c.cycles += 4; break;
    case 0x3F: ccf(c);  c.cycles += 4; break;
    case 0x01: case 0x11: case 0x21: case 0x31: rpSet(c, (op >> 4) & 3, idx, imm16(c)); c.cycles += 10; break; // LD rp,nn
    case 0x09: case 0x19: case 0x29: case 0x39: add16(c, idx, rpGet(c, (op >> 4) & 3, idx)); c.cycles += 11; break; // ADD HL,rp
    case 0x03: case 0x13: case 0x23: case 0x33: rpSet(c, (op >> 4) & 3, idx, rpGet(c, (op >> 4) & 3, idx) + 1); c.cycles += 6; break;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: rpSet(c, (op >> 4) & 3, idx, rpGet(c, (op >> 4) & 3, idx) - 1); c.cycles += 6; break;
    case 0x02: c.wr(c.BC(), c.A); c.MEMPTR = (uint16_t)((c.A << 8) | ((c.BC() + 1) & 0xFF)); c.cycles += 7; break; // LD (BC),A
    case 0x12: c.wr(c.DE(), c.A); c.MEMPTR = (uint16_t)((c.A << 8) | ((c.DE() + 1) & 0xFF)); c.cycles += 7; break; // LD (DE),A
    case 0x0A: c.A = c.rd(c.BC()); c.MEMPTR = c.BC() + 1; c.cycles += 7; break;                                   // LD A,(BC)
    case 0x1A: c.A = c.rd(c.DE()); c.MEMPTR = c.DE() + 1; c.cycles += 7; break;                                   // LD A,(DE)
    case 0x22: { uint16_t nn = imm16(c); uint16_t v = idx16(c, idx); c.wr(nn, (uint8_t)v); c.wr(nn + 1, v >> 8); c.MEMPTR = nn + 1; c.cycles += 16; break; } // LD (nn),HL
    case 0x2A: { uint16_t nn = imm16(c); uint16_t v = c.rd(nn) | (c.rd(nn + 1) << 8); if (idx == 1) c.setIX(v); else if (idx == 2) c.setIY(v); else c.setHL(v); c.MEMPTR = nn + 1; c.cycles += 16; break; } // LD HL,(nn)
    case 0x32: { uint16_t nn = imm16(c); c.wr(nn, c.A); c.MEMPTR = (uint16_t)((c.A << 8) | ((nn + 1) & 0xFF)); c.cycles += 13; break; } // LD (nn),A
    case 0x3A: { uint16_t nn = imm16(c); c.A = c.rd(nn); c.MEMPTR = nn + 1; c.cycles += 13; break; }              // LD A,(nn)
    // 0xC0-0xFF
    case 0xC0: case 0xC8: case 0xD0: case 0xD8: case 0xE0: case 0xE8: case 0xF0: case 0xF8: {   // RET cc
      int cc = (op >> 3) & 7; if (cond(c, cc)) { c.PC = pop16(c); c.MEMPTR = c.PC; c.cycles += 11; } else c.cycles += 5; break;
    }
    case 0xC1: case 0xD1: case 0xE1: case 0xF1: pp2Set(c, (op >> 4) & 3, idx, pop16(c)); c.cycles += 10; break;   // POP
    case 0xC5: case 0xD5: case 0xE5: case 0xF5: push16(c, pp2Get(c, (op >> 4) & 3, idx)); c.cycles += 11; break;  // PUSH
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xE2: case 0xEA: case 0xF2: case 0xFA: {  // JP cc,nn
      int cc = (op >> 3) & 7; uint16_t nn = imm16(c); c.MEMPTR = nn; if (cond(c, cc)) c.PC = nn; c.cycles += 10; break;
    }
    case 0xC3: { uint16_t nn = imm16(c); c.PC = nn; c.MEMPTR = nn; c.cycles += 10; break; }                       // JP nn
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: case 0xE4: case 0xEC: case 0xF4: case 0xFC: { // CALL cc,nn
      int cc = (op >> 3) & 7; uint16_t nn = imm16(c); c.MEMPTR = nn;
      if (cond(c, cc)) { push16(c, c.PC); c.PC = nn; c.cycles += 17; } else c.cycles += 10; break;
    }
    case 0xCD: { uint16_t nn = imm16(c); c.MEMPTR = nn; push16(c, c.PC); c.PC = nn; c.cycles += 17; break; }      // CALL nn
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:    // ALU A,n
      alu(c, (op >> 3) & 7, imm(c)); c.cycles += 7; break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:    // RST
      push16(c, c.PC); c.PC = op & 0x38; c.MEMPTR = c.PC; c.cycles += 11; break;
    case 0xC9: c.PC = pop16(c); c.MEMPTR = c.PC; c.cycles += 10; break;                        // RET
    case 0xD3: { uint8_t n = imm(c); c.out((uint16_t)((c.A << 8) | n), c.A); c.MEMPTR = (uint16_t)((c.A << 8) | ((n + 1) & 0xFF)); c.cycles += 11; break; } // OUT (n),A
    case 0xDB: { uint8_t n = imm(c); uint16_t p = (uint16_t)((c.A << 8) | n); c.A = c.in(p); c.MEMPTR = p + 1; c.cycles += 11; break; }                     // IN A,(n)
    case 0xD9: { uint8_t t;                                                                    // EXX
      t = c.B; c.B = c.B_; c.B_ = t; t = c.C; c.C = c.C_; c.C_ = t;
      t = c.D; c.D = c.D_; c.D_ = t; t = c.E; c.E = c.E_; c.E_ = t;
      t = c.H; c.H = c.H_; c.H_ = t; t = c.L; c.L = c.L_; c.L_ = t; c.cycles += 4; break; }
    case 0xE3: { uint16_t v = idx16(c, idx); uint16_t sp0 = c.rd(c.SP) | (c.rd(c.SP + 1) << 8);  // EX (SP),HL
      c.wr(c.SP, (uint8_t)v); c.wr(c.SP + 1, v >> 8);
      if (idx == 1) c.setIX(sp0); else if (idx == 2) c.setIY(sp0); else c.setHL(sp0);
      c.MEMPTR = sp0; c.cycles += 19; break; }
    case 0xE9: c.PC = idx16(c, idx); c.cycles += 4; break;                                     // JP (HL)/(IX)/(IY)
    case 0xEB: { uint8_t t; t = c.D; c.D = c.H; c.H = t; t = c.E; c.E = c.L; c.L = t; c.cycles += 4; break; } // EX DE,HL
    case 0xF3: c.IFF1 = c.IFF2 = false; c.cycles += 4; break;                                  // DI
    case 0xFB: c.IFF1 = c.IFF2 = true; *wasEI = true; c.cycles += 4; break;                    // EI
    case 0xF9: c.SP = idx16(c, idx); c.cycles += 6; break;                                     // LD SP,HL
    default: c.cycles += 4; break;
  }
}

// ============================ public interface ==================================================
void Z80::reset() {
  if (!tablesReady) initTables();
  A = F = B = C = D = E = H = L = 0;
  A_ = F_ = B_ = C_ = D_ = E_ = H_ = L_ = 0;
  IXH = IXL = IYH = IYL = 0;
  I = R = R7 = 0;
  SP = 0xFFFF; PC = 0x0000; MEMPTR = 0;
  IFF1 = IFF2 = false; IM = 0; halted = false; eiPending = 0;
  cycles = 0;
  // eiBlock lives in this->eiPending's sign convention below; keep it simple:
  _eiBlock = false;
}

int Z80::step() {
  if (!tablesReady) initTables();
  if (halted) { incR(*this); cycles += 4; return 4; }
  uint64_t start = cycles;
  bool wasEI = false;
  int idx = 0;
  uint8_t op = fetchOp(*this);
  while (op == 0xDD || op == 0xFD) { idx = (op == 0xDD) ? 1 : 2; cycles += 4; op = fetchOp(*this); }
  if      (op == 0xCB) execCB(*this, idx);
  else if (op == 0xED) execED(*this);          // ED ignores any DD/FD prefix
  else                 execMain(*this, op, idx, &wasEI);
  _eiBlock = wasEI;
  return (int)(cycles - start);
}

bool Z80::irq(uint8_t bus) {
  if (!IFF1 || _eiBlock) return false;          // not enabled, or still in EI's one-instruction shadow
  IFF1 = IFF2 = false;
  if (halted) halted = false;                   // PC already points past HALT
  incR(*this);
  if (IM == 2) { uint16_t a = (uint16_t)((I << 8) | bus); push16(*this, PC); PC = rd(a) | (rd(a + 1) << 8); MEMPTR = PC; cycles += 19; }
  else if (IM == 1) { push16(*this, PC); PC = 0x0038; MEMPTR = PC; cycles += 13; }
  else { push16(*this, PC); PC = (uint16_t)(bus & 0x38); MEMPTR = PC; cycles += 13; }   // IM0: assume RST on the bus
  return true;
}

void Z80::nmi() {
  IFF2 = IFF1; IFF1 = false;
  if (halted) halted = false;
  incR(*this);
  push16(*this, PC); PC = 0x0066; MEMPTR = PC; cycles += 11;
}
