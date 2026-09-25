// Link-time stubs for the two Z80 cores (MSX, SMS) on any board that clears BOARD_HAS_MSX_CORE or
// BOARD_HAS_SMS_CORE -- today no board does (see board.h), but the gates stay so a tight target
// can drop a core again without touching the shared code.
//
// The cores are gone, but the shared UI, input, joystick and render code still NAMES their entry
// points from ordinary runtime branches on currentPlatform. Those branches are unreachable here:
// setup() normalises a stale platform out of EEPROM before anything reads it, and
// splashEnabled() refuses to select either one. Defining the symbols as no-ops is what lets the
// six-board shared code stay free of #ifs at every one of those call sites, which matters far
// more for readability than the handful of bytes these empty functions cost.
//
// The bool returns are false on purpose: "no warning overlay is showing" for the two
// RenderLoadWarning hooks, and "nothing was loaded" for the two LoadSelected hooks -- exactly
// what a caller expects from a core with nothing to draw and no file to open.
// The browse hooks are no-ops for the same reason: with no core there is no browser to move.
#include "../../emu.h"

#if !BOARD_HAS_MSX_CORE

void msxKeyMatrix(uint8_t, uint8_t, bool) {}
void msxSetInput(uint8_t) {}
void msxRenderFrame() {}
bool msxRenderLoadWarning() { return false; }
void msxScanFiles() {}
void msxBrowseEnter(const char *) {}
void msxBrowseUp() {}
bool msxLoadSelected(const char *) { return false; }

#endif  // !BOARD_HAS_MSX_CORE

#if !BOARD_HAS_SMS_CORE

void smsSetInput(uint8_t) {}
void smsPauseButton() {}
void smsHardReset() {}
void smsRenderFrame() {}
bool smsRenderLoadWarning() { return false; }
void smsScanFiles() {}
void smsBrowseEnter(const char *) {}
void smsBrowseUp() {}
bool smsLoadSelected(const char *) { return false; }

#endif  // !BOARD_HAS_SMS_CORE

#if !BOARD_HAS_COLECO_CORE

void colecoSetInput(uint8_t) {}
void colecoSetKeypad(int) {}
void colecoHardReset() {}
void colecoRenderFrame() {}
bool colecoRenderLoadWarning() { return false; }
void colecoScanFiles() {}
void colecoBrowseEnter(const char *) {}
void colecoBrowseUp() {}
bool colecoLoadSelected(const char *) { return false; }

#endif  // !BOARD_HAS_COLECO_CORE

#if !BOARD_HAS_ZX_CORE

void zxSetKempston(uint8_t) {}
void zxKey(int, int, bool) {}
void zxKeysReleaseAll() {}
void zxHardReset() {}
void zxRenderFrame() {}
bool zxRenderLoadWarning() { return false; }
void zxScanFiles() {}
void zxBrowseEnter(const char *) {}
void zxBrowseUp() {}
bool zxLoadSelected(const char *) { return false; }

#endif  // !BOARD_HAS_ZX_CORE
