// msx.h - MSX1 machine model (Z80 + TMS9918 VDP + AY-3-8910 PSG + 8255 PPI) for emu8.
//
// Deliberately free of Arduino/board headers so the SAME core compiles into host/msx_host.cpp
// (g++, off-device) and into the board build. Device-only glue (allocation, splash/render/config
// wiring) lives in src/msx/msx.cpp which DOES include emu.h. The C-linkage entry points at the
// bottom (msxSetup/msxLoop/...) are what the platform dispatch in emu8.ino / video.cpp calls.

#pragma once
#include <stdint.h>
#include "../z80/z80.h"

namespace msx {

// ---- machine geometry ----
static const int VDP_W = 256, VDP_H = 192;       // TMS9918 active display
static const int VRAM_SIZE = 0x4000;             // 16 KB
static const int FB_SIZE = VDP_W * VDP_H;        // 256*192 = 49152 bytes (8-bit indexed)

// ---- shared state (defined in msx_globals.cpp) ----
extern Z80      cpu;
extern uint8_t* ram;            // 64 KB work RAM (slot 3)
extern uint8_t* bios;           // BIOS ROM (slot 0), 16 or 32 KB; SD image or embedded C-BIOS
extern int      biosLen;
extern uint8_t* vram;           // 16 KB VDP RAM
extern uint8_t* framebuffer;    // 256*192 indexed; points at sharedBigBuf on the device
extern bool     biosIsCbios;    // true when we fell back to the embedded C-BIOS (no Disk BASIC)
extern volatile bool frameReady;// producer/consumer handshake: core 1 fills the framebuffer, core 0
                                // displays it; neither touches it while the other does (no tearing)

// ---- machine core (msx_machine.cpp) ----
void machineWire();             // point cpu.rd/wr/in/out at the routers below; call after pointers set
void machineReset();            // reset Z80 + VDP + PPI + PSG, clear slot selection
uint8_t memRead8(uint16_t a);
void    memWrite8(uint16_t a, uint8_t v);
uint8_t ioIn(uint16_t port);
void    ioOut(uint16_t port, uint8_t val);
void    runFrame();             // execute ~one 60 Hz frame of Z80 time, then VDP VBlank + IRQ

// ---- VDP TMS9918 (msx_vdp.cpp) ----
void    vdpReset();
void    vdpWriteData(uint8_t v);
uint8_t vdpReadData();
void    vdpWriteCtrl(uint8_t v);
uint8_t vdpReadStatus();
void    vdpEndFrame();          // set the VBlank status flag (and arm IRQ if R1 bit5 set)
bool    vdpIrqActive();         // VBlank flag set AND interrupts enabled
void    vdpRender();            // paint the current mode into framebuffer (256x192 indexed)
uint8_t vdpRegister(int r);     // for diagnostics
extern uint16_t MSX_PALETTE[16];// TMS9918 fixed palette as RGB565 (device uses this directly)

// ---- 8255 PPI + keyboard (msx_ppi.cpp) ----
void    ppiReset();
uint8_t ppiRead(uint8_t port);  // port: 0xA8/0xA9/0xAA
void    ppiWrite(uint8_t port, uint8_t val);
int     ppiPageSlot(int page);  // current primary slot (0-3) mapped to 16 KB page (0-3)
void    kbSetKey(int row, int col, bool down);   // MSX 8x11 matrix injector (row 0-10, col 0-7)
void    kbReset();
void    setJoystick(uint8_t mask);               // active-LOW: b0 up b1 down b2 left b3 right b4 trgA b5 trgB

// ---- PSG AY-3-8910 (msx_psg.cpp) ----  (full audio in M3.5; minimal reg file + joystick I/O now)
void    psgReset();
void    psgWriteAddr(uint8_t a);
void    psgWriteData(uint8_t v);
uint8_t psgReadData();
int     psgGenSample(int masterVol, bool mute);  // one 22050 Hz sample (0..255); used by msx_audio.cpp

// ---- cartridge (msx_cart.cpp, M3) ----
uint8_t cartRead(int slot, uint16_t a);          // slot 1 or 2
void    cartWrite(int slot, uint16_t a, uint8_t v);
bool    cartPresent(int slot);

} // namespace msx

// ===== platform entry points (called from emu8.ino / video.cpp / optionsui.cpp) =====
// Plain C++ linkage to match proto.h and the other platform cores (atari/nes/...).
void msxSetup();
void msxLoop();
void msxRenderFrame();
void msxPsgSetup();
void msxKeyMatrix(uint8_t row, uint8_t col, bool down);
void msxSetInput(uint8_t joyMask);
bool msxLoadSelected(const char* path);
void msxScanFiles();
bool msxRenderLoadWarning();
