// pcxt.h - public API of the PC-XT (Intel 8086) platform, consumed by the shared
// dispatch (emu8.ino, src/shared/video.cpp, optionsui.cpp, usbkeyboard.cpp,
// joystick.cpp). Mirrors sms.h. Implemented in pcxt.cpp (the only PC-XT file that
// includes emu.h / Arduino); the machine core lives in src/pcxt/fabgl/.

#pragma once

#include <stdint.h>

// platform entry points (called from emu8.ino setup()/loop())
void pcxtSetup();
void pcxtLoop();

// core-0 render task (src/shared/video.cpp renderLoop)
bool pcxtRenderFrame();
void pcxtForceRedraw();
bool pcxtRenderLoadWarning();   // startup overlay while no boot disk is mounted

// input
void pcxtSetInput(uint8_t joyMask);   // gamepad -> arrow/enter scancodes
void pcxtKeyDown(uint8_t hidUsage, bool shift, bool ctrl, bool alt);
void pcxtKeyUp(uint8_t hidUsage);
void pcxtHardReset();

// settings / file browser hooks (src/shared/optionsui.cpp)
void pcxtScanFiles();
bool pcxtMountA(const char* path);         // mount image into A: (floppy)
bool pcxtMountC(const char* path);         // mount image into C: (hard disk)
bool pcxtMountAuto(const char* path);      // route by size: floppy(<=2.88MB)->A:, hard disk->C: (+re-POST)

// SD scan helper (called from setup like loadSmsFilesSync)
void loadPcxtFilesSync();
