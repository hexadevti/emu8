// tiny386_s3stub.cpp - the i386/tiny386 platform is NOT built for the S3 board (JC4827W543). The
// vendored tiny386 core isn't wired for the device toolchain, and it is too big for the S3's budget,
// so every src/tiny386/*.c/.cpp is #if'd out on the S3 (each carries a BOARD_JC4827W543 guard). These
// empty entry points let the shared dispatch / render / input code still link on the S3. tiny386
// stays fully available on the P4 (BOARD_JC1060P470) and the desktop build; the "386" splash button
// is shown as "SOON" on the S3 (src/shared/video.cpp).
#if defined(BOARD_JC4827W543)
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
void loadTiny386FilesSync() {}
bool tiny386RenderLoadWarning() { return false; }

#endif // BOARD_JC4827W543
