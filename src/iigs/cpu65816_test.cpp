// cpu65816_test.cpp - on-device self-test for the M1 65C816 core (src/iigs/cpu65816.*).
//
// Compiles only with -DIIGS_M1_TEST. Runs hand-assembled 65816 programs against a 64KB bank-0
// test RAM and verifies memory/registers, printing PASS/FAIL + a register dump per test. Hooked
// (looping) from emu6502.ino so a plain serial read captures the result. See the IIGS feasibility
// memory for the GO verdict that motivated building this core.

#ifdef IIGS_M1_TEST

#include <Arduino.h>
#include <string.h>
#include "cpu65816.h"
#include "iigs_mem.h"

static uint8_t   testRAM[0x10000];
static CPU65816  cpu;
static int       passN, failN;

static uint8_t trd(uint32_t a) { return testRAM[a & 0xFFFF]; }     // bank-agnostic 64KB test RAM
static void    twr(uint32_t a, uint8_t v) { testRAM[a & 0xFFFF] = v; }
static void    poke(uint16_t at, const uint8_t* p, int n) { for (int i = 0; i < n; i++) testRAM[(uint16_t)(at + i)] = p[i]; }

static void loadProg(const uint8_t* p, int n, uint16_t at) {
  memset(testRAM, 0, sizeof(testRAM));
  for (int i = 0; i < n; i++) testRAM[(uint16_t)(at + i)] = p[i];
  testRAM[0xFFFC] = at & 0xFF; testRAM[0xFFFD] = at >> 8;          // reset vector
}
static void runProg() {
  cpu.rd = trd; cpu.wr = twr; cpu.reset();
  for (int i = 0; i < 200000 && !cpu.stopped; i++) cpu.step();
}
static void check(const char* name, bool cond) {
  Serial.printf("M1 %-26s %s   [A=%04X X=%04X Y=%04X S=%04X P=%02X E=%d PC=%04X]\n",
                name, cond ? "PASS" : "*** FAIL ***",
                cpu.A, cpu.X, cpu.Y, cpu.S, cpu.P, cpu.E ? 1 : 0, cpu.PC);
  if (cond) passN++; else failN++;
}

void runIIgsM1Test() {
  Serial.println();
  Serial.println("=== IIGS M1: 65C816 core self-test ===");
  passN = failN = 0;

  { // T1: LDA #imm, STA zp, LDA zp, STA abs
    const uint8_t p[] = { 0xA9,0x42, 0x85,0x10, 0xA9,0x00, 0xA5,0x10, 0x8D,0x00,0x20, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T1 LDA/STA imm+zp+abs", testRAM[0x10]==0x42 && testRAM[0x2000]==0x42 && (cpu.A&0xFF)==0x42);
  }
  { // T2: CLC, ADC with carry-out (F0+20 = 110 -> A=10, C=1)
    const uint8_t p[] = { 0x18, 0xA9,0xF0, 0x69,0x20, 0x85,0x11, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T2 ADC binary + carry", testRAM[0x11]==0x10 && (cpu.P&P65_C));
  }
  { // T3: count-down loop (LDX #5; 5x ADC #1; DEX/BNE) -> A=5
    const uint8_t p[] = { 0xA2,0x05, 0xA9,0x00, 0x18, 0x69,0x01, 0xCA, 0xD0,0xFA, 0x85,0x12, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T3 branch/loop DEX/BNE", testRAM[0x12]==0x05 && (cpu.X&0xFF)==0x00 && (cpu.P&P65_Z));
  }
  { // T4: JSR/RTS + stack
    const uint8_t p[] = { 0x20,0x08,0x80, 0xA5,0x13, 0x85,0x14, 0xDB, 0xA9,0xAA, 0x85,0x13, 0x60 };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T4 JSR/RTS", testRAM[0x13]==0xAA && testRAM[0x14]==0xAA);
  }
  { // T5: native mode 16-bit (XCE, REP #$30, 16-bit LDA/ADC/STA, SEP, XCE back).
    //  Note CLC before ADC: CLC;XCE leaves C = old E = 1, so clear it for a clean 1234+1111=2345.
    const uint8_t p[] = { 0x18, 0xFB, 0xC2,0x30, 0x18, 0xA9,0x34,0x12, 0x69,0x11,0x11,
                          0x8D,0x00,0x30, 0xE2,0x30, 0x38, 0xFB, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T5 native 16-bit add/store", testRAM[0x3000]==0x45 && testRAM[0x3001]==0x23 && cpu.E);
  }
  { // T6: decimal ADC (BCD 25+48 = 73)
    const uint8_t p[] = { 0xF8, 0x18, 0xA9,0x25, 0x69,0x48, 0x85,0x15, 0xD8, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T6 decimal ADC (BCD)", testRAM[0x15]==0x73);
  }
  { // T7: absolute-indexed store/load (STA $2500,X / LDA $2500,X)
    const uint8_t p[] = { 0xA2,0x03, 0xA9,0x99, 0x9D,0x00,0x25, 0xBD,0x00,0x25, 0x85,0x16, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T7 abs,X store/load", testRAM[0x2503]==0x99 && testRAM[0x16]==0x99);
  }
  { // T8: (dp),Y indirect-indexed store/load via pointer $20 -> $2600, Y=4
    const uint8_t p[] = { 0xA9,0x00, 0x85,0x20, 0xA9,0x26, 0x85,0x21, 0xA0,0x04,
                          0xA9,0x77, 0x91,0x20, 0xB1,0x20, 0x85,0x17, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T8 (dp),Y indirect", testRAM[0x2604]==0x77 && testRAM[0x17]==0x77);
  }
  { // T9: native 16-bit INC/DEC of the accumulator + 16-bit stores
    //  $00FF --INC--> $0100 (stored at $3100), --DEC DEC--> $00FE (stored at $3102)
    const uint8_t p[] = { 0x18, 0xFB, 0xC2,0x30,        // native 16-bit
                          0xA9,0xFF,0x00,               // LDA #$00FF
                          0x1A,                         // INC A -> $0100
                          0x8D,0x00,0x31,               // STA $3100 (3100=00, 3101=01)
                          0x3A, 0x3A,                   // DEC A, DEC A -> $00FE
                          0x8D,0x02,0x31,               // STA $3102 (3102=FE, 3103=00)
                          0xE2,0x30, 0x38, 0xFB, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T9 native 16-bit INC/DEC", testRAM[0x3100]==0x00 && testRAM[0x3101]==0x01 &&
                                      testRAM[0x3102]==0xFE && testRAM[0x3103]==0x00);
  }

  // ---------------- M2 hardening: previously-untested opcodes / addressing modes ----------------
  { // T10: absolute long store/load (STA al / LDA al), bank 0
    const uint8_t p[] = { 0xA9,0x5A, 0x8F,0x00,0x20,0x00, 0xA9,0x00, 0xAF,0x00,0x20,0x00, 0x85,0x30, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T10 long STA/LDA", testRAM[0x2000]==0x5A && testRAM[0x30]==0x5A);
  }
  { // T11: [dp] indirect long (STA [dp] / LDA [dp]) via 24-bit pointer at $40 -> $00:2700
    const uint8_t p[] = { 0xA9,0x00,0x85,0x40, 0xA9,0x27,0x85,0x41, 0xA9,0x00,0x85,0x42,
                          0xA9,0xC3, 0x87,0x40, 0xA9,0x00, 0xA7,0x40, 0x85,0x31, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T11 [dp] indirect long", testRAM[0x2700]==0xC3 && testRAM[0x31]==0xC3);
  }
  { // T12: [dp],Y long indirect indexed, ptr $40 -> $00:2700, Y=5 -> $2705
    const uint8_t p[] = { 0xA9,0x00,0x85,0x40, 0xA9,0x27,0x85,0x41, 0xA9,0x00,0x85,0x42,
                          0xA0,0x05, 0xA9,0xD4, 0x97,0x40, 0xA9,0x00, 0xB7,0x40, 0x85,0x32, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T12 [dp],Y long", testRAM[0x2705]==0xD4 && testRAM[0x32]==0xD4);
  }
  { // T13: stack-relative (PHA #$99, LDA $01,S reads the pushed byte)
    const uint8_t p[] = { 0xA9,0x99, 0x48, 0xA9,0x00, 0xA3,0x01, 0x85,0x33, 0x68, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T13 stack-relative LDA", testRAM[0x33]==0x99);
  }
  { // T14: PHP/PLP restore carry (SEC; PHP; CLC; PLP; ROL A captures restored C=1)
    const uint8_t p[] = { 0x38, 0x08, 0x18, 0x28, 0xA9,0x00, 0x2A, 0x85,0x34, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T14 PHP/PLP flags", testRAM[0x34]==0x01);
  }
  { // T15: transfers TAX/TXA; then native 16-bit XBA swaps bytes of $1234 -> $3412
    const uint8_t pa[] = { 0xA9,0x3C, 0xAA, 0xA9,0x00, 0x8A, 0x85,0x35, 0xDB };
    loadProg(pa, sizeof(pa), 0x8000); runProg();
    bool a = (testRAM[0x35]==0x3C);
    const uint8_t pb[] = { 0x18, 0xFB, 0xC2,0x30, 0xA9,0x34,0x12, 0xEB, 0x8D,0x04,0x31, 0xE2,0x30, 0x38, 0xFB, 0xDB };
    loadProg(pb, sizeof(pb), 0x8000); runProg();
    bool b = (testRAM[0x3104]==0x12 && testRAM[0x3105]==0x34);
    check("T15 TAX/TXA + XBA", a && b);
  }
  { // T16: MVN block move of 4 bytes $1000 -> $2000 (native 16-bit), A=count-1=3
    const uint8_t p[] = { 0xA9,0x11,0x8D,0x00,0x10, 0xA9,0x22,0x8D,0x01,0x10,
                          0xA9,0x33,0x8D,0x02,0x10, 0xA9,0x44,0x8D,0x03,0x10,
                          0x18, 0xFB, 0xC2,0x30, 0xA2,0x00,0x10, 0xA0,0x00,0x20, 0xA9,0x03,0x00,
                          0x54,0x00,0x00, 0xE2,0x30, 0x38, 0xFB, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T16 MVN block move", testRAM[0x2000]==0x11 && testRAM[0x2001]==0x22 &&
                                testRAM[0x2002]==0x33 && testRAM[0x2003]==0x44);
  }
  { // T17: BRK -> handler ($9000) -> RTI -> resume (two blobs + IRQ/BRK vector $FFFE)
    memset(testRAM, 0, sizeof(testRAM));
    const uint8_t mn[] = { 0x00,0x00, 0xA9,0xDD, 0x85,0x37, 0xDB };   // main: BRK; (ret) LDA #$DD; STA $37; STP
    const uint8_t hd[] = { 0xA9,0xEE, 0x85,0x36, 0x40 };             // handler: LDA #$EE; STA $36; RTI
    poke(0x8000, mn, sizeof(mn)); poke(0x9000, hd, sizeof(hd));
    testRAM[0xFFFC] = 0x00; testRAM[0xFFFD] = 0x80;                  // reset vector
    testRAM[0xFFFE] = 0x00; testRAM[0xFFFF] = 0x90;                  // emulation IRQ/BRK vector
    runProg();
    check("T17 BRK/RTI", testRAM[0x36]==0xEE && testRAM[0x37]==0xDD);
  }
  { // T18: decimal SBC (BCD 50-25 = 25)
    const uint8_t p[] = { 0xF8, 0x38, 0xA9,0x50, 0xE9,0x25, 0x85,0x38, 0xD8, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T18 decimal SBC (BCD)", testRAM[0x38]==0x25);
  }
  { // T19: native 16-bit CMP equal -> BEQ taken
    const uint8_t p[] = { 0x18, 0xFB, 0xC2,0x30, 0xA9,0x00,0x10, 0xC9,0x00,0x10, 0xE2,0x30,
                          0xF0,0x04, 0xA9,0x00, 0x80,0x02, 0xA9,0xAB, 0x85,0x39, 0x38, 0xFB, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T19 16-bit CMP/BEQ", testRAM[0x39]==0xAB);
  }
  { // T20: TSB/TRB (mem $50: 0F --TSB A=F0--> FF --TRB A=F0--> 0F).
    //  Note: reload A=#$F0 before TRB (the LDA $50 above clobbers A with the memory value).
    const uint8_t p[] = { 0xA9,0x0F,0x85,0x50, 0xA9,0xF0, 0x04,0x50, 0xA5,0x50,0x85,0x3A,
                          0xA9,0xF0, 0x14,0x50, 0xA5,0x50,0x85,0x3B, 0xDB };
    loadProg(p, sizeof(p), 0x8000); runProg();
    check("T20 TSB/TRB", testRAM[0x3A]==0xFF && testRAM[0x3B]==0x0F);
  }

  // ---------------- M2 integration: CPU running against the IIGS banked memory ----------------
  { // T21: bank 0 (SRAM) code does a long store/load to bank $05 (PSRAM) and a JSL/RTL into bank $05
    if (!iigsMemInit()) { check("T21 banked-mem init", false); }
    else {
      const uint8_t mn[] = { 0xA9,0x7E, 0x8F,0x00,0x00,0x05, 0xA9,0x00, 0xAF,0x00,0x00,0x05,
                             0x8D,0x00,0x03, 0x22,0x00,0x80,0x05, 0x8D,0x02,0x03, 0xDB };
      const uint8_t sb[] = { 0xA9,0xC9, 0x6B };   // bank $05 subroutine: LDA #$C9; RTL
      for (unsigned i = 0; i < sizeof(mn); i++) iigsWrite24(0x008000 + i, mn[i]);
      for (unsigned i = 0; i < sizeof(sb); i++) iigsWrite24(0x058000 + i, sb[i]);
      iigsWrite24(0x00FFFC, 0x00); iigsWrite24(0x00FFFD, 0x80);
      iigsWrite24(0x050000, 0); iigsWrite24(0x000300, 0); iigsWrite24(0x000302, 0);
      cpu.rd = iigsRead24; cpu.wr = iigsWrite24; cpu.reset();
      for (int i = 0; i < 200000 && !cpu.stopped; i++) cpu.step();
      bool pass = iigsRead24(0x050000)==0x7E && iigsRead24(0x000300)==0x7E && iigsRead24(0x000302)==0xC9;
      Serial.printf("M2 %-26s %s   [PSRAM 05:0000=%02X  00:0300=%02X  00:0302=%02X  PBR=%02X]\n",
                    "T21 banked SRAM/PSRAM+JSL", pass ? "PASS" : "*** FAIL ***",
                    iigsRead24(0x050000), iigsRead24(0x000300), iigsRead24(0x000302), cpu.PBR);
      if (pass) passN++; else failN++;
      cpu.rd = trd; cpu.wr = twr;
    }
  }

  Serial.printf("M1 RESULT: %d passed, %d failed\n", passN, failN);
  Serial.println(failN ? "=== M1 FAIL ===" : "=== M1 PASS ===");
}

#endif // IIGS_M1_TEST
