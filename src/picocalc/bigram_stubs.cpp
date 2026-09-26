// bigram_stubs.cpp - empty entry points for the cores the PicoCalc cannot host.
//
// The PC-XT runs on the RP2350 from a paged guest address space (src/pcxt/fabgl/pcmem.h,
// BOARD_HAS_PCXT_CORE in board.h); on the RP2040 there is too little heap for DOS, so its
// src/pcxt/**/*.cpp compile to nothing and the splash button draws greyed as "SOON".
//
// What is left is the SHARED code that still names the core unconditionally -- the settings UI's
// disk browsers, the USB-keyboard dispatcher's PC-XT scancode path, the joystick mapper. Rather than
// thread a capability macro through all of it, this file supplies the symbols so it links; the code
// paths that would call them are unreachable because currentPlatform can never be PLATFORM_PCXT.

#include "../../emu.h"

#if defined(BOARD_PICOCALC)

#if !BOARD_HAS_PCXT_CORE
// --- PC-XT / Intel 8086 (src/pcxt/pcxt.cpp), RP2040 only ---
void pcxtSetup() { printLog("PC-XT: not available on the RP2040 (too little RAM)"); }
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
#endif

#endif // BOARD_PICOCALC
