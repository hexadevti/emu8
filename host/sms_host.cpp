// sms_host.cpp - desktop harness for the SMS core (fast off-device iteration), like host/msx_host.cpp.
//
//   ./sms_host game.sms            -> boot the cartridge, run ~180 frames, dump VDP regs + CRAM,
//                                     and write sms_out.ppm (the framebuffer) for eyeballing.
//   ./sms_host game.sms 300 out.ppm-> run 300 frames, write out.ppm
//
// Build (the SMS machine is Arduino-free, so it links straight against the same Z80 core as MSX):
//   g++ -O2 -I. -DSMS_HOST_BOOT -o sms_host host/sms_host.cpp src/z80/z80.cpp \
//       src/sms/sms_machine.cpp src/sms/sms_vdp.cpp src/sms/sms_cart.cpp src/sms/sms_io.cpp \
//       src/sms/sms_psg.cpp src/sms/sms_globals.cpp
//
// CPU correctness is already covered by host/msx_host.cpp (same core runs ZEXDOC/ZEXALL); this harness
// exercises the SMS mapper, VDP command port, and Mode 4 renderer.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

extern void smsHostInit(const uint8_t* rom, int romLen);
extern void smsHostRunFrames(int frames);
extern void smsHostDumpRegs();
extern void smsHostDumpPPM(const char* path);

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: %s game.sms [frames] [out.ppm]\n", argv[0]); return 1; }
  const char* path = argv[1];
  int frames = (argc > 2) ? atoi(argv[2]) : 180;
  const char* ppm = (argc > 3) ? argv[3] : "sms_out.ppm";

  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); return 1; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  // Some .sms dumps carry a 512-byte header; skip it if the size is header+power-of-two.
  long skip = (sz & 0x3FFF) == 512 ? 512 : 0;
  if (skip) fseek(f, skip, SEEK_SET);
  long romLen = sz - skip;
  uint8_t* rom = (uint8_t*)malloc(romLen);
  if (fread(rom, 1, romLen, f) != (size_t)romLen) { printf("short read\n"); return 1; }
  fclose(f);
  printf("loaded %s: %ld bytes (%ldK)%s\n", path, romLen, romLen / 1024, skip ? " [skipped 512B header]" : "");

  smsHostInit(rom, (int)romLen);
  smsHostRunFrames(frames);
  printf("ran %d frames\n", frames);
  smsHostDumpRegs();
  smsHostDumpPPM(ppm);
  free(rom);
  return 0;
}
