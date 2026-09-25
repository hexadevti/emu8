// zx.h - Sinclair ZX Spectrum 48K machine model (Z80 + ULA + beeper) for emu8.
//
// The Spectrum is the simplest Z80 machine in the repo: 16K ROM, 48K RAM, and one custom chip (the
// ULA) that draws a 256x192 bitmap + 32x24 attribute screen straight out of RAM, raises a 50 Hz INT,
// and owns port 0xFE (keyboard half-rows, border colour, beeper). What is emulated:
//   * the shared Z80 core (src/z80/), 69888 T-states per frame, INT at the top of each frame
//   * the ULA screen at frame granularity, the border colour per scanline (stripes show up)
//   * the keyboard matrix, a Kempston joystick on port 0x1F, the 1-bit beeper
//   * .SNA / .Z80 snapshots, and .TAP / .TZX tapes loaded instantly through a trap on the ROM's
//     LD-BYTES routine (no real-time tape signal; turbo loaders are not supported)
// Memory contention and the floating bus are not emulated.
//
// The 16K ROM (spec48.rom / 48.rom) is loaded from /roms/zxspectrum on the SD card. Like coleco.h this
// header is free of Arduino/board headers; zx.cpp is the device glue.

#pragma once
#include <stdint.h>
#include "../z80/z80.h"

namespace zx {

static const int ROM_SIZE   = 0x4000;             // 16 KB ROM at 0x0000-0x3FFF
static const int RAM_SIZE   = 0xC000;             // 48 KB RAM at 0x4000-0xFFFF
static const int SCR_SIZE   = 6912;               // 6144 bitmap + 768 attributes at 0x4000
static const int T_LINE     = 224;                // T-states per scanline
static const int LINES      = 312;                // scanlines per frame (PAL)
static const int T_FRAME    = T_LINE * LINES;     // 69888 T-states = 20 ms at 3.5 MHz
static const int FIRST_PAPER_LINE = 64;           // scanline of the first bitmap row
static const int OUT_W = 320, OUT_H = 240;        // what we display: 256x192 paper + border
static const int BORDER_X = 32, BORDER_Y = 24;
static const int AUDIO_FS = 22050;
static const int BEEP_RING = 2048;                // beeper sample ring (power of two)

// ---- shared state (zx_globals.cpp) ----
extern Z80            cpu;
extern uint8_t*       ram;          // 48 KB (sharedBigBuf); address a lives at ram[a - 0x4000]
extern const uint8_t* rom;          // 16 KB ROM (SD image; flash on the PicoCalc)
extern int            romLen;       // 0 = no ROM -> the machine does not run
extern uint8_t*       screen;       // SCR_SIZE snapshot of 0x4000-0x5AFF taken at the end of a frame
extern uint8_t*       borderLine;   // OUT_H border colours (0-7), one per displayed row
extern bool           flashPhase;   // FLASH attribute state of the snapshot
extern volatile bool  frameReady;   // core 1 filled screen/borderLine; core 0 displays + clears
extern uint8_t*       beepRing;     // BEEP_RING beeper samples (sharedBigBuf)

// ---- tape (a flattened list of TAP-style data blocks: flag byte, data, checksum) ----
struct TapeBlock { uint32_t off; uint32_t len; };
extern const uint8_t* tapeData;     // resident tape image (flash on the PicoCalc)
extern TapeBlock*     tapeBlocks;
extern int            tapeCount;
extern int            tapeMaxBlocks;
extern int            tapePos;      // next block LD-BYTES will read

// ---- machine (zx_machine.cpp) ----
void    machineWire();
void    machineReset();
uint8_t memRead8(uint16_t a);
void    memWrite8(uint16_t a, uint8_t v);
uint8_t ioIn(uint16_t port);
void    ioOut(uint16_t port, uint8_t v);
void    runFrame();                 // one 50 Hz frame
uint8_t border();                   // current border colour (snapshot loaders set it)
void    setBorder(uint8_t c);

// Keyboard: 8 half-rows x 5 keys, active-LOW like the hardware. row = 0..7 in port-address order
// (A8 = CAPS SHIFT..V, A9 = A..G, A10 = Q..T, A11 = 1..5, A12 = 0..6, A13 = P..Y, A14 = ENTER..H,
// A15 = SPACE..B), bit = 0..4 (outermost key first).
void keySet(int row, int bit, bool down);
void keyClearAll();
// Kempston joystick, active-HIGH: b0 right b1 left b2 down b3 up b4 fire.
void setKempston(uint8_t v);

// Beeper: samples produced by runFrame (22050 Hz, 0..255 time-weighted level). Drained by the
// audio task; returns false when the ring is empty.
bool beeperPop(uint8_t* s);
int  beeperDepth();                 // samples waiting in the ring

// Tape parsing (zx_tape.cpp): index a .TAP or .TZX image into tapeBlocks. Returns the block count.
int  tapeIndexTap(const uint8_t* d, int len);
int  tapeIndexTzx(const uint8_t* d, int len);

// Scripted typing after a reset (LOAD "" for tapes): a list of key chords, each held then released.
// Each chord is up to two (row, bit) pairs packed as (row << 3 | bit) + 1, 0 = unused.
void autoTypeStart(const uint16_t* chords, int n, int delayFrames);

} // namespace zx

// ===== platform entry points (called from emu8.ino / video.cpp / optionsui.cpp / input) =====
void zxSetup();
void zxLoop();
void zxRenderFrame();
void zxAudioSetup();
void zxSetKempston(uint8_t v);
void zxKey(int row, int bit, bool down);
void zxKeysReleaseAll();
void zxHardReset();
bool zxLoadSelected(const char* path);
void zxScanFiles();
bool zxRenderLoadWarning();
void loadZxFilesSync();
void zxBrowseEnter(const char* path);
void zxBrowseUp();
