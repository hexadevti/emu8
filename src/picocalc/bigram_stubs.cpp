// bigram_stubs.cpp - empty entry points for the three cores the PicoCalc cannot host.
//
// The Apple IIGS, PC-XT and tiny386 cores each ps_malloc() 1-16 MB of guest RAM. The RP2350 has
// 520 KB of SRAM, and the PicoCalc's 8 MB PSRAM hangs off plain GPIOs (2/3/20/21, bit-bang/PIO SPI)
// rather than the memory-mapped QSPI window, so it cannot back a raw pointer at all. Running them
// would mean a PIO PSRAM driver plus reworking each core onto a paged block API -- a separate
// project, not a board port. So BOARD_HAS_BIGRAM_CORES is 0 here and:
//
//   * emu8.ino's setup()/loop() dispatch and video.cpp's render branches are #if'd out,
//   * the IIGS / PCXT / 386 splash buttons draw greyed as "SOON" and ignore selection,
//   * every src/iigs/*.cpp, src/pcxt/**/*.cpp and src/tiny386/*.c[pp] is compiled to nothing.
//
// What is left is the SHARED code that still names these cores unconditionally -- the settings UI's
// disk browsers, the USB-keyboard dispatcher's PC-XT scancode path, the joystick mapper. Rather than
// thread a capability macro through all of it, this file supplies the symbols so it links; the code
// paths that would call them are unreachable because currentPlatform can never be one of the three.
// (tiny386's own stubs live in src/tiny386/tiny386_s3stub.cpp, which the S3 board already needed.)

#include "../../emu.h"

#if defined(BOARD_PICOCALC)

// --- Apple IIGS (src/iigs/iigs_boot.cpp) ---
void iigsSetup() { printLog("IIGS: not available on this board (no PSRAM)"); }
void iigsLoop() {}
void iigsRenderText() {}
void iigsLoadDisk(const char *) {}
void iigsLoadHD(const char *) {}

// --- PC-XT / Intel 8086 (src/pcxt/pcxt.cpp) ---
void pcxtSetup() { printLog("PC-XT: not available on this board (no PSRAM)"); }
void pcxtLoop() {}
bool pcxtRenderFrame() { return false; }
void pcxtForceRedraw() {}
void pcxtSetInput(uint8_t) {}
void pcxtMouseInput(int, int, uint8_t) {}
void pcxtKeyDown(uint8_t, bool, bool, bool) {}
void pcxtKeyUp(uint8_t) {}
void pcxtHardReset() {}
bool pcxtMountA(const char *) { return false; }
bool pcxtMountC(const char *) { return false; }
bool pcxtMountAuto(const char *) { return false; }
void pcxtUnmount(int) {}
void pcxtScanFiles() {}
void pcxtBrowseEnter(const char *) {}
void pcxtBrowseUp() {}
bool pcxtRenderLoadWarning() { return false; }
void loadPcxtFilesSync() {}

#endif // BOARD_PICOCALC
