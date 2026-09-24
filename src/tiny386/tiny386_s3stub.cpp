// tiny386_s3stub.cpp - the i386/tiny386 platform is NOT built for the S3 board (JC4827W543) or the
// PicoCalc (RP2350). On the S3 the vendored core isn't wired for the device toolchain and is too big
// for the budget; on the PicoCalc there is no memory-mapped PSRAM at all to hold the multi-megabyte
// guest. Every src/tiny386/*.c/.cpp is #if'd out on both (each carries the matching guard). These
// empty entry points let the shared dispatch / render / input code still link. tiny386 stays fully
// available on the P4 (BOARD_JC1060P470) and the desktop build; the "386" splash button is shown as
// "SOON" everywhere else (src/shared/video.cpp).
#if defined(BOARD_JC4827W543) || defined(BOARD_PICOCALC)
#include "../../emu.h"

void tiny386Setup() {}
void tiny386Loop() {}
bool tiny386RenderFrame() { return false; }
void tiny386ForceRedraw() {}
void tiny386SetInput(uint8_t) {}
void tiny386KeyDown(uint8_t, bool, bool, bool) {}
void tiny386KeyUp(uint8_t) {}
void tiny386MouseInput(int, int, uint8_t) {}
void tiny386HardReset() {}
bool tiny386LoadSelected(const char *) { return false; }
bool tiny386MountA(const char *) { return false; }
bool tiny386MountC(const char *) { return false; }
void tiny386ScanFiles() {}
void tiny386BrowseEnter(const char *) {}
void tiny386BrowseUp() {}
void loadTiny386FilesSync() {}
bool tiny386RenderLoadWarning() { return false; }

#endif // BOARD_JC4827W543 || BOARD_PICOCALC
