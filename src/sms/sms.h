// sms.h - Sega Master System machine model (Z80 + 315-5124 VDP + SN76489 PSG) for emu8.
//
// The SMS shares the Z80 core (src/z80/) with the MSX1 platform and a VDP that is a SUPERSET of the
// TMS9918 - the new work versus MSX is Mode 4 (the tile/sprite mode all SMS games use), the SN76489
// PSG, the Sega memory mapper, and the SMS I/O-port decode. SMS games boot the cartridge ROM directly
// at PC=0x0000, so there is NO BIOS to load.
//
// Like msx.h, this header is deliberately free of Arduino/board headers so the SAME core compiles into
// host/sms_host.cpp (g++, off-device) and into the board build. Device-only glue (allocation,
// render/config wiring) lives in src/sms/sms.cpp which DOES include emu.h. The C-linkage entry points
// at the bottom (smsSetup/smsLoop/...) are what the platform dispatch in emu8.ino / video.cpp calls.

#pragma once
#include <stdint.h>
#include "../z80/z80.h"

namespace sms {

// ---- machine geometry ----
static const int VDP_W = 256, VDP_H = 192;       // Mode 4 active display (NTSC 192-line)
static const int VRAM_SIZE = 0x4000;             // 16 KB VDP RAM
static const int CRAM_SIZE = 32;                 // two 16-colour palettes (6-bit --BBGGRR)
static const int WRAM_SIZE = 0x2000;             // 8 KB work RAM (mirrored 0xC000-0xFFFF)
static const int FB_SIZE = VDP_W * VDP_H;        // 256*192 = 49152 bytes (8-bit indexed 0..31)

// ---- shared state (defined in sms_globals.cpp) ----
extern Z80      cpu;
extern uint8_t* ram;            // 8 KB work RAM (0xC000-0xDFFF, mirror 0xE000-0xFFFF)
extern uint8_t* rom;            // cartridge ROM image (resident; PSRAM on device)
extern int      romLen;
extern uint8_t* vram;           // 16 KB VDP RAM
extern uint8_t* framebuffer;    // 256*192 indexed 0..31; points at sharedBigBuf on the device
extern volatile bool frameReady;// producer/consumer handshake (core 1 fills, core 0 displays)

// ---- machine core (sms_machine.cpp) ----
void machineWire();             // point cpu.rd/wr/in/out at the routers; call after pointers set
void machineReset();            // reset Z80 + VDP + PSG + I/O + mapper
uint8_t memRead8(uint16_t a);
void    memWrite8(uint16_t a, uint8_t v);
void    runFrame();             // one 60 Hz frame: per-scanline Z80 time + VDP line/VBlank interrupts
void    smsPause();             // SMS PAUSE button -> latched Z80 NMI (consumed at frame boundary)

// ---- I/O ports (sms_io.cpp) ----
uint8_t ioIn(uint16_t port);
void    ioOut(uint16_t port, uint8_t val);
void    ioReset();
void    setInput1(uint8_t mask);// controller 1, active-LOW: b0 up b1 down b2 left b3 right b4 TL b5 TR
void    setInput2(uint8_t mask);// controller 2 (same bit order)

// ---- VDP 315-5124 / Mode 4 (sms_vdp.cpp) ----
void    vdpReset();
void    vdpWriteData(uint8_t v);   // port $BE
uint8_t vdpReadData();             // port $BE
void    vdpWriteCtrl(uint8_t v);   // port $BF (2-byte command)
uint8_t vdpReadStatus();           // port $BF (clears INT flags + control latch)
uint8_t vdpVCounter();             // port $7E
uint8_t vdpHCounter();             // port $7F
void    vdpLineTick(int line);     // advance one scanline (line-IRQ counter; VBlank flag at line 192)
void    vdpSnapshotLine(int line); // latch per-line horizontal scroll for split-screen rendering
bool    vdpIrqActive();            // frame or line interrupt pending AND enabled
void    vdpRender();               // paint Mode 4 (bg + sprites) into framebuffer (indexed 0..31)
void    vdpBuildPalette(uint16_t* lut32); // CRAM -> 32-entry RGB565 LUT (for the device render path)
uint8_t vdpRegister(int r);        // diagnostics
uint8_t vdpCram(int i);            // diagnostics

// ---- SN76489 PSG (sms_psg.cpp) ----
void    psgReset();
void    psgWrite(uint8_t v);       // single write port ($7F/$7E range): latch/data protocol
int     psgGenSample(int masterVol, bool mute);  // one 22050 Hz sample (0..255); used by sms_audio.cpp

// ---- cartridge / Sega mapper (sms_cart.cpp) ----
void    cartReset();
uint8_t cartRead(uint16_t a);      // CPU 0x0000-0xBFFF (ROM banks + optional cart RAM)
void    cartWrite(uint16_t a, uint8_t v);  // cart-RAM writes + mapper registers $FFFC-$FFFF
void    cartSetImage(const uint8_t* data, int len);

} // namespace sms

// ===== platform entry points (called from emu8.ino / video.cpp / optionsui.cpp) =====
// Plain C++ linkage to match proto.h and the other platform cores (msx/nes/atari/...).
void smsSetup();
void smsLoop();
void smsRenderFrame();
void smsPsgSetup();
void smsSetInput(uint8_t joyMask);
bool smsLoadSelected(const char* path);
void smsScanFiles();
bool smsRenderLoadWarning();
void loadSmsFilesSync();
