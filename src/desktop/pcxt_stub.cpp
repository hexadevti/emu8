// pcxt_stub.cpp — empty PC-XT entry points so the desktop build links without the fabgl chipset.
// PC-XT (Intel 8086 + fabgl, GPLv3, deep ESP dependencies) is a Phase-3 desktop port; until then
// selecting PLATFORM_PCXT on desktop does nothing. Signatures mirror proto.h.
#if defined(BOARD_DESKTOP)

#include "../../emu.h"

void pcxtSetup() { printLog("PC-XT: not available on desktop yet (Phase 3)"); }
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
void pcxtUnmount(int) {}
void pcxtScanFiles() {}
bool pcxtRenderLoadWarning() { return false; }
void loadPcxtFilesSync() {}

#endif // BOARD_DESKTOP
