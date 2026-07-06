// msx_host.cpp - desktop harness for the MSX Z80 core (fast off-device iteration), like
// host/iigs_host.cpp. Three modes:
//
//   ./msx_host                      -> built-in self-test (deterministic instruction/flag vectors)
//   ./msx_host selftest             -> same
//   ./msx_host path/to/zexdoc.com   -> CP/M test-runner (ZEXDOC / ZEXALL) - the gold-standard check
//   ./msx_host path/to/bios.rom [cart.rom]  -> boot the MSX1 machine, dump the VDP text screen (M2)
//
// Build:  g++ -O2 -I. -o msx_host host/msx_host.cpp src/z80/z80.cpp
//   (later, for the boot mode:  ... src/z80/z80.cpp src/msx/msx_machine.cpp src/msx/msx_vdp.cpp ...)
//
// The built-in self-test is a sanity net (gross-bug catcher), NOT a substitute for ZEXALL: its
// expected values are hand-derived, so run real ZEXALL once a host compiler is available.

#include "../src/z80/z80.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static uint8_t MEM[0x10000];
static uint8_t  hostRd(uint16_t a)            { return MEM[a]; }
static void     hostWr(uint16_t a, uint8_t v) { MEM[a] = v; }
static uint8_t  hostIn(uint16_t /*p*/)        { return 0xFF; }
static void     hostOut(uint16_t /*p*/, uint8_t /*v*/) {}

// ===================== built-in self-test ========================================================
static Z80 T;
static int g_tests = 0, g_fail = 0;

static void tReset() {
  memset(&T, 0, sizeof(T));
  T.rd = hostRd; T.wr = hostWr; T.in = hostIn; T.out = hostOut;
  T.reset();
  T.PC = 0x0100; T.SP = 0xF000;
}
static void put(std::initializer_list<uint8_t> ops) { uint16_t a = 0x0100; for (uint8_t b : ops) MEM[a++] = b; T.PC = 0x0100; }
static void chk(const char* what, uint32_t got, uint32_t exp) {
  g_tests++;
  if (got != exp) { g_fail++; printf("  FAIL %-18s got=%02X exp=%02X\n", what, got, exp); }
}

static int runSelfTest() {
  printf("Z80 self-test (sanity vectors - run ZEXALL for full coverage)\n");

  // --- 8-bit ALU ---
  tReset(); T.A = 0x44; T.B = 0x11; put({0x80});          T.step(); chk("ADD A,B res",  T.A, 0x55); chk("ADD A,B flg", T.F, 0x20);
  tReset(); T.A = 0xFF; T.B = 0x01; put({0x80});          T.step(); chk("ADD wrap res", T.A, 0x00); chk("ADD wrap flg", T.F, 0x51);
  tReset(); T.A = 0x7F; T.B = 0x01; put({0x80});          T.step(); chk("ADD ovf res",  T.A, 0x80); chk("ADD ovf flg",  T.F, 0x94);
  tReset(); T.A = 0x00; T.B = 0x01; put({0x90});          T.step(); chk("SUB borrow",   T.A, 0xFF); chk("SUB borrow flg", T.F, 0xBB);
  tReset(); T.A = 0x20; T.B = 0x20; put({0x90});          T.step(); chk("SUB zero",     T.A, 0x00); chk("SUB zero flg", T.F, 0x42);
  tReset(); T.A = 0x0F; T.B = 0xF0; put({0xA0});          T.step(); chk("AND zero",     T.A, 0x00); chk("AND zero flg", T.F, 0x54);
  tReset(); T.A = 0xFF; T.B = 0x0F; put({0xA0});          T.step(); chk("AND res",      T.A, 0x0F); chk("AND flg",      T.F, 0x1C);
  tReset(); T.A = 0x00; T.B = 0x00; put({0xB0});          T.step(); chk("OR zero",      T.A, 0x00); chk("OR zero flg",  T.F, 0x44);
  tReset(); T.A = 0x5A;            put({0xAF});           T.step(); chk("XOR A",        T.A, 0x00); chk("XOR A flg",    T.F, 0x44);
  tReset(); T.A = 0x10; T.B = 0x20; put({0xB8});          T.step(); chk("CP A unchg",   T.A, 0x10); chk("CP flg",       T.F, 0xA3);

  // --- INC / DEC (carry preserved) ---
  tReset(); T.B = 0x7F; T.F = Z80_CF; put({0x04});        T.step(); chk("INC B ovf",    T.B, 0x80); chk("INC B flg",    T.F, 0x95);
  tReset(); T.B = 0x00; T.F = 0;      put({0x05});        T.step(); chk("DEC B wrap",   T.B, 0xFF); chk("DEC B flg",    T.F, 0xBA);

  // --- rotates ---
  tReset(); T.A = 0x80; put({0x07});                      T.step(); chk("RLCA res",     T.A, 0x01); chk("RLCA flg",     T.F, 0x01);
  tReset(); T.A = 0x01; put({0x0F});                      T.step(); chk("RRCA res",     T.A, 0x80); chk("RRCA flg",     T.F, 0x01);
  tReset(); T.B = 0x80; put({0xCB, 0x00});                T.step(); chk("RLC B res",    T.B, 0x01); chk("RLC B flg",    T.F, 0x01);

  // --- BIT ---
  tReset(); T.B = 0x80; T.F = 0; put({0xCB, 0x78});       T.step(); chk("BIT 7,B set",  T.F, 0x90);
  tReset(); T.B = 0xFE; T.F = 0; put({0xCB, 0x40});       T.step(); chk("BIT 0,B clr",  T.F, 0x7C);

  // --- misc accumulator ---
  tReset(); T.A = 0x00; T.F = 0; put({0x37});             T.step(); chk("SCF flg",      T.F, 0x01);
  tReset(); T.A = 0x00; T.F = 0; put({0x2F});             T.step(); chk("CPL res",      T.A, 0xFF); chk("CPL flg",      T.F, 0x3A);
  tReset(); T.A = 0x01; put({0xED, 0x44});                T.step(); chk("NEG res",      T.A, 0xFF); chk("NEG flg",      T.F, 0xBB);
  tReset(); T.A = 0x00; T.F = 0; put({0x27});             T.step(); chk("DAA noop res", T.A, 0x00); chk("DAA noop flg", T.F, 0x44);

  // --- 16-bit ADD HL,DE ---
  tReset(); T.setHL(0x0FFF); T.setDE(0x0001); T.F = 0; put({0x19}); T.step();
  chk("ADD HL,DE", T.HL(), 0x1000); chk("ADD HL,DE flg", T.F, 0x10);

  // --- EX DE,HL ---
  tReset(); T.setHL(0x1234); T.setDE(0xABCD); put({0xEB}); T.step();
  chk("EX DE,HL HL", T.HL(), 0xABCD); chk("EX DE,HL DE", T.DE(), 0x1234);

  // --- LDI (block transfer) ---
  tReset(); MEM[0x2000] = 0x5A; T.setHL(0x2000); T.setDE(0x3000); T.setBC(0x0002);
  put({0xED, 0xA0}); T.step();
  chk("LDI copied", MEM[0x3000], 0x5A); chk("LDI HL", T.HL(), 0x2001); chk("LDI DE", T.DE(), 0x3001); chk("LDI BC", T.BC(), 0x0001);

  // --- DD (IX) decode: LD IXH,n then INC IX ---
  tReset(); put({0xDD, 0x26, 0x12}); T.step(); chk("LD IXH,n", T.IXH, 0x12);
  tReset(); T.setIX(0x00FF); put({0xDD, 0x23}); T.step(); chk("INC IX", T.IX(), 0x0100);

  // --- DD (IX+d) memory ---
  tReset(); T.setIX(0x4000); MEM[0x4005] = 0x77; put({0xDD, 0x7E, 0x05}); T.step(); chk("LD A,(IX+d)", T.A, 0x77);

  // --- CALL / RET round-trip ---
  tReset(); T.SP = 0xF000; put({0xCD, 0x10, 0x01}); MEM[0x0110] = 0xC9; /* RET */
  T.step();  // CALL 0x0110
  chk("CALL pushed", (uint32_t)(MEM[0xEFFE] | (MEM[0xEFFF] << 8)), 0x0103);
  chk("CALL target", T.PC, 0x0110);
  T.step();  // RET
  chk("RET back", T.PC, 0x0103);

  printf("self-test: %d checks, %d FAIL%s\n", g_tests, g_fail, g_fail ? "  <-- core has bugs" : "  (all passed)");
  return g_fail ? 1 : 0;
}

// ===================== CP/M test runner (ZEXDOC / ZEXALL) =========================================
static int runZex(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open test image: %s\n", path); return 1; }
  size_t n = fread(MEM + 0x0100, 1, 0x10000 - 0x0100, f); fclose(f);
  printf("loaded %zu bytes from %s at 0x0100\n", n, path);
  MEM[0x0005] = 0xC9;   // BDOS: RET (intercepted before it runs)

  Z80 cpu; memset(&cpu, 0, sizeof(cpu));
  cpu.rd = hostRd; cpu.wr = hostWr; cpu.in = hostIn; cpu.out = hostOut;
  cpu.reset(); cpu.PC = 0x0100; cpu.SP = 0xF000;

  const uint64_t MAX_STEPS = 6000ull * 1000 * 1000;
  uint64_t steps = 0;
  for (; steps < MAX_STEPS; steps++) {
    if (cpu.PC == 0x0005) {
      if (cpu.C == 2) putchar(cpu.E);
      else if (cpu.C == 9) { uint16_t de = cpu.DE(); for (int g = 0; g < 65536; g++) { char ch = (char)MEM[(uint16_t)(de + g)]; if (ch == '$') break; putchar(ch); } }
      fflush(stdout);
      cpu.PC = (uint16_t)(MEM[cpu.SP] | (MEM[cpu.SP + 1] << 8)); cpu.SP += 2;
      continue;
    }
    if (cpu.PC == 0x0000) { printf("\n[warm boot - finished]\n"); break; }
    cpu.step();
  }
  if (steps >= MAX_STEPS) printf("\n[step cap reached]\n");
  printf("ran %llu instr, %llu T-states\n", (unsigned long long)steps, (unsigned long long)cpu.cycles);
  return 0;
}

// ===================== MSX boot mode (M2 - wired up once msx_machine.cpp lands) ===================
// Forward decls of the host-side machine entry points (to be implemented in src/msx/msx_machine.cpp
// behind a MSX_HOST guard, mirroring how iigs_host.cpp drives the IIGS map).
#ifdef MSX_HOST_BOOT
extern void   msxHostInit(const uint8_t* bios, int biosLen, const uint8_t* cart, int cartLen);
extern void   msxHostRunFrames(int frames);
extern void   msxHostDumpText();
static int runBoot(const char* biosPath, const char* cartPath) {
  FILE* bf = fopen(biosPath, "rb"); if (!bf) { printf("no BIOS: %s\n", biosPath); return 1; }
  static uint8_t bios[0x8000]; int bl = (int)fread(bios, 1, sizeof(bios), bf); fclose(bf);
  static uint8_t cart[0x20000]; int cl = 0;
  if (cartPath) { FILE* cf = fopen(cartPath, "rb"); if (cf) { cl = (int)fread(cart, 1, sizeof(cart), cf); fclose(cf); } }
  printf("BIOS %d bytes, cart %d bytes\n", bl, cl);
  msxHostInit(bios, bl, cl ? cart : nullptr, cl);
  msxHostRunFrames(180);     // ~3 seconds of emulated frames; enough to reach the BASIC prompt
  msxHostDumpText();
  return 0;
}
#endif

int main(int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) == "selftest") return runSelfTest();
  std::string a1 = argv[1];
  auto ends = [&](const char* s) { size_t n = strlen(s); return a1.size() >= n && a1.compare(a1.size() - n, n, s) == 0; };
  if (ends(".com")) return runZex(argv[1]);
#ifdef MSX_HOST_BOOT
  if (ends(".rom")) return runBoot(argv[1], argc > 2 ? argv[2] : nullptr);
#endif
  printf("unknown image type: %s (use .com for ZEX, .rom for MSX boot, or 'selftest')\n", argv[1]);
  return 1;
}
