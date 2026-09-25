// coleco.h - ColecoVision machine model (Z80 + TMS9918A VDP + SN76489 PSG) for emu8.
//
// The ColecoVision is built from parts this repo already emulates, so the core is mostly glue:
//   * the Z80 core (src/z80/), shared with the MSX and SMS
//   * the TMS9918A VDP -- the MSX one (src/msx/msx_vdp.cpp) is driven directly: we point
//     msx::vram / msx::framebuffer at our buffers and call msx::vdp*()
//   * the SN76489 PSG -- the SMS one (src/sms/sms_psg.cpp), via sms::psgWrite / sms::psgGenSample
// What is new is the memory map (8K BIOS, 1K RAM, 32K cartridge + MegaCart banking), the I/O
// decode, the two-mode controllers (joystick / 12-key keypad) and the VDP interrupt, which on this
// machine is wired to the Z80 NMI rather than INT.
//
// The 8K BIOS (coleco.rom) is copyrighted and not shipped: it is loaded from /roms/coleco on the SD
// card. Like msx.h / sms.h this header is free of Arduino/board headers so the SAME core compiles
// into host/coleco_host.cpp (g++, off-device) and into the board build.

#pragma once
#include <stdint.h>
#include "../z80/z80.h"

namespace coleco {

static const int VDP_W = 256, VDP_H = 192;       // TMS9918A active display
static const int VRAM_SIZE = 0x4000;             // 16 KB VDP RAM
static const int RAM_SIZE  = 0x400;              // 1 KB work RAM (mirrored 0x6000-0x7FFF)
static const int BIOS_SIZE = 0x2000;             // 8 KB BIOS at 0x0000-0x1FFF

// ---- shared state (coleco_globals.cpp) ----
extern Z80            cpu;
extern uint8_t*       ram;          // 1 KB work RAM
extern const uint8_t* bios;         // 8 KB BIOS (SD image; flash on the PicoCalc)
extern int            biosLen;      // 0 = no BIOS -> the machine does not run
extern const uint8_t* rom;          // cartridge image (resident; PSRAM / flash on device)
extern int            romLen;
extern volatile bool  frameReady;   // core 1 filled msx::framebuffer; core 0 displays + clears

// ---- machine (coleco_machine.cpp) ----
void    machineWire();              // point cpu.rd/wr/in/out at the routers below
void    machineReset();             // reset Z80 + VDP + PSG + controllers + MegaCart bank
uint8_t memRead8(uint16_t a);
void    memWrite8(uint16_t a, uint8_t v);
uint8_t ioIn(uint16_t port);
void    ioOut(uint16_t port, uint8_t v);
void    runFrame();                 // one 60 Hz frame: per-scanline Z80 time, VBlank -> NMI
void    cartSetImage(const uint8_t* data, int len);

// ---- controllers ----
// Direction/fire mask, active-LOW like the SMS/MSX paths: b0 up b1 down b2 left b3 right
// b4 left fire b5 right fire.
void setInput(int pad, uint8_t mask);
// Keypad key held on a controller: 0-9, 10 = '*', 11 = '#', -1 = none.
void setKeypad(int pad, int key);

} // namespace coleco

// ===== platform entry points (called from emu8.ino / video.cpp / optionsui.cpp / input) =====
void colecoSetup();
void colecoLoop();
void colecoRenderFrame();
void colecoPsgSetup();
void colecoSetInput(uint8_t joyMask);
void colecoSetKeypad(int key);
void colecoHardReset();
bool colecoLoadSelected(const char* path);
void colecoScanFiles();
bool colecoRenderLoadWarning();
void loadColecoFilesSync();
void colecoBrowseEnter(const char* path);
void colecoBrowseUp();
