// gen_sms_test.cpp - emit a tiny Z80 SMS cartridge that exercises the Mode 4 path end-to-end:
// VDP register init, CRAM palette, two solid-colour tiles, and a checkerboard name table. Used only to
// validate the SMS core on the desktop harness (no real ROM needed). Outputs a 32 KB .sms.
//
//   g++ -O2 -o gen_sms_test host/gen_sms_test.cpp && ./gen_sms_test smstest.sms
#include <cstdio>
#include <cstdint>
#include <vector>
static std::vector<uint8_t> R;
static void b(int x) { R.push_back((uint8_t)(x & 0xFF)); }
static void ldA(int n) { b(0x3E); b(n); }
static void outp(int p) { b(0xD3); b(p); }
static void setReg(int reg, int val) { ldA(val); outp(0xBF); ldA(0x80 | reg); outp(0xBF); }
static void setVramW(int a) { ldA(a & 0xFF); outp(0xBF); ldA(0x40 | ((a >> 8) & 0x3F)); outp(0xBF); }
static void setCramW(int a) { ldA(a & 0xFF); outp(0xBF); ldA(0xC0 | ((a >> 8) & 0x3F)); outp(0xBF); }
static void wData(int v) { ldA(v); outp(0xBE); }

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : "smstest.sms";
  b(0xF3);                                   // DI
  //         R0    R1    R2    R3    R4    R5    R6    R7    R8    R9    R10
  int regs[11] = {0x04, 0x40, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x04, 0x00, 0xFF};
  // R6=0x00 -> sprite pattern base 0x0000; R8=0x04 -> horizontal scroll right by 4.
  for (int i = 0; i < 11; i++) setReg(i, regs[i]);

  // CRAM: BG palette 0 = black, 1 = white, 2 = red, 3 = blue; sprite palette: 17 = green.
  int pal[32] = {0};
  pal[0] = 0x00; pal[1] = 0x3F; pal[2] = 0x03; pal[3] = 0x30;
  pal[17] = 0x0C;                       // sprite colour 1 = green
  setCramW(0);
  for (int i = 0; i < 32; i++) wData(pal[i]);

  // Tile 1 = solid colour 1 (plane0 set); Tile 2 = solid colour 2 (plane1 set). 32 bytes each.
  setVramW(0x20);
  for (int row = 0; row < 8; row++) { wData(0xFF); wData(0x00); wData(0x00); wData(0x00); }
  setVramW(0x40);
  for (int row = 0; row < 8; row++) { wData(0x00); wData(0xFF); wData(0x00); wData(0x00); }
  // Tile 3 (VRAM 0x60) = solid colour 1, used as the sprite pattern (sprite pattern base = 0x2000,
  // so the sprite tile index 3 reads pattern 3 from base 0 here only if base 0 - set R6=0 below).
  setVramW(0x60);
  for (int row = 0; row < 8; row++) { wData(0xFF); wData(0x00); wData(0x00); wData(0x00); }

  // Sprite attribute table at 0x3F00: one 8x8 sprite (tile 3) at (60,50), then a Y=0xD0 terminator.
  setVramW(0x3F00);
  wData(50); wData(0xD0);                // sprite0 Y, sprite1 Y = terminator
  setVramW(0x3F80);
  wData(60); wData(3);                   // sprite0 X, sprite0 tile

  // Name table at 0x3800: checkerboard of tile 1 / tile 2 (896 entries, 2 bytes each).
  setVramW(0x3800);
  b(0x11); b(896 & 0xFF); b((896 >> 8) & 0xFF);   // LD DE,896
  b(0x0E); b(0x01);                               // LD C,1
  int ntLoop = (int)R.size();
  b(0x79);                  // LD A,C
  b(0xD3); b(0xBE);         // OUT (BE),A    tile low byte
  b(0xAF);                  // XOR A
  b(0xD3); b(0xBE);         // OUT (BE),A    high byte 0
  b(0x79);                  // LD A,C
  b(0xEE); b(0x03);         // XOR 3         toggle 1<->2
  b(0x4F);                  // LD C,A
  b(0x1B);                  // DEC DE
  b(0x7A);                  // LD A,D
  b(0xB3);                  // OR E
  b(0x20);                  // JR NZ,ntLoop
  int afterJr = (int)R.size() + 1;
  b((ntLoop - afterJr) & 0xFF);

  b(0x18); b(0xFE);         // JR $  (halt loop)

  // pad to 32 KB
  while (R.size() < 0x8000) b(0x00);
  FILE* f = fopen(out, "wb");
  if (!f) { printf("cannot write %s\n", out); return 1; }
  fwrite(R.data(), 1, R.size(), f);
  fclose(f);
  printf("wrote %s (%zu bytes, code=%d bytes)\n", out, R.size(), ntLoop);
  return 0;
}
