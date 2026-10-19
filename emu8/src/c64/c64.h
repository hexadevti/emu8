#pragma once
// Commodore 64 core (ported from the C64Esp32 project into the multi-platform build).
// All C64-internal state/functions live in namespace c64 so they don't collide with the
// Apple II core's globals (read8/write8/ram/PC/A/X/Y/SR/cpuLoop/...). Shared CYD globals
// (tft, touchRead, running, paused, OptionsWindow, currentPlatform, joystick ADC) are used
// from global scope as-is. C-linkage entry points (bottom) are called by the dispatch.

#include <cstdint>

struct SnapIO;   // src/shared/snapshot.h (save states, c64_snapshot.cpp)

// PicoCalc: the 6510 hot path (cpuLoop, read8/write8, the CIA tick, the raster step and the opcode
// tables) runs from SRAM, as the Apple II core's does. From flash it fetched through the 16K XIP
// cache that both cores share, and the render task's panel code kept evicting it -- the C64 ran at
// under half speed. About 8K of SRAM, paid for by the system ROMs having moved into flash. Other
// boards keep their default placement (the ESP32's IRAM is too small to take it).
#if defined(BOARD_PICOCALC)
#define C64_HOT      IRAM_ATTR
#define C64_HOT_DATA DRAM_ATTR
#else
#define C64_HOT
#define C64_HOT_DATA
#endif

// C64 ROM images — loaded at boot from /roms on the SD card into PSRAM buffers
// (c64rom.cpp); pointers, not arrays, and null until c64LoadRoms() has run.
extern const unsigned char *basic_rom;
extern const unsigned char *kernal_rom;
extern const unsigned char *charset_rom;

// Load BASIC/KERNAL/CHARGEN from the SD card (/roms). Returns false if any is missing or
// the wrong size — the caller must show an error and must NOT run the 6510 on the null pointers.
bool c64LoadRoms();

namespace c64 {

// ---- CPU-visible config ----
extern bool mos65c02;   // always false on the C64 (NMOS 6510)

// ---- Memory / banking ----
extern unsigned char *ram;       // 64K
extern bool bankARAM, bankDRAM, bankERAM, bankDIO;
extern uint8_t register1;

// ---- Cartridge (.crt) state ----
// cartExrom/cartGame are the EXROM/GAME line levels (true = HIGH/inactive). 8K cart:
// EXROM=0,GAME=1; 16K: EXROM=0,GAME=0; Ultimax: EXROM=1,GAME=0.
extern uint8_t *cartROML;        // $8000-$9FFF ROM (8K)
extern uint8_t *cartROMH;        // $A000-$BFFF (16K) or $E000-$FFFF (Ultimax) ROM (8K)
extern bool cartActive, cartExrom, cartGame;
extern volatile bool c64ResetReq;   // request a CPU reset (e.g. after mounting a cart)
extern volatile bool c64RestoreReq; // the RESTORE key was pressed: one NMI

// ---- VIC-II state (shared by memory + vic + render) ----
extern uint8_t vicreg[0x40];
extern uint8_t latchd011, latchd012;
// Per-line VIC state, recorded by the emulator core for the render core (see c64_vic.cpp).
struct VicLineRec {
  volatile uint8_t frame;        // vicRecFrame of the frame it belongs to; 0 while being written
  uint8_t lineMap, vertborder;
  uint8_t snapRow;                // character row whose screen/colour bytes were copied, 0xFF: none
  uint16_t screenmem, bitmapstart, vicmem, pad1;
  const uint8_t *charset;
  uint8_t reg[0x30];             // $D000-$D02F
  uint8_t sprPtr[8];             // the sprite pointers (screen + $3F8) on this line
};
#define VIC_STRIP_LINES 8
#define VIC_STRIPS 6             // RGB565 strips the render task streams through (in sharedBigBuf)
#define VIC_REC_OFFSET (VIC_STRIPS * VIC_STRIP_LINES * 320 * 2)   // vicRec[400] follows them
// 2 banks x 200 records (display lines 0..199): frame f is recorded into bank f & 1, so the render
// core can trail the beam by up to a whole frame before a record it needs is overwritten.
extern VicLineRec *vicRec;
extern volatile uint8_t vicRecFrame;   // frame being recorded (1..254; odd/even alternate)
// Even frames go to bank 1, odd ones to bank 0.
inline VicLineRec *vicRecFor(uint8_t frame, uint8_t dline) { return vicRec + ((frame + 1) & 1) * 200 + dline; }
// Per bank, the 40 screen + 40 colour bytes of each character row, copied when the beam enters it:
// the render core trails the beam, and a game rewrites the row right after the beam has drawn it.
#define VIC_SNAP_OFFSET (VIC_REC_OFFSET + 400 * sizeof(VicLineRec))
extern uint8_t *vicSnap;
inline uint8_t *vicSnapFor(uint8_t frame, uint8_t row) { return vicSnap + (((frame + 1) & 1) * 25 + row) * 80; }
extern uint8_t vicLineBuf[320 + 16];
extern volatile uint32_t c64PerfKhz;       // last second's 6510 speed (kHz), logged by the render task
extern volatile uint8_t c64PerfVicPct;     // share of the emulator core spent recording/drawing lines
extern volatile uint32_t c64MaxLineGapUs;  // longest gap between two visible lines (render task resets)
void vicRenderRecord(const VicLineRec &r, uint8_t dline, uint8_t *out, uint16_t *dst,
                     const uint16_t *pal);                  // render core: one line (dst: also RGB565)
void vicLineToRgb(const uint8_t *out, uint16_t *dst, const uint16_t *pal);   // 320 indices -> RGB565
extern bool vicCollPolled;       // the program has read $D01E/$D01F since reset (see c64_vic.cpp)
extern uint16_t vicmem, bitmapstart, screenmemstart, rasterline;
extern uint8_t syncd020;
extern bool screenblank, badlinecond0;
extern uint8_t *bitmap;          // the line the VIC renders into (= vicLineBuf, 8-bit colour indices)
extern uint8_t *colormap;        // 1K color RAM
extern const uint8_t *charset;   // points into chrom (flash) or RAM
extern const uint8_t *chrom;     // character ROM (flash)
extern uint8_t cntRefreshs;
extern const uint16_t c64Colors[16];

// ---- CPU (c64_cpu.cpp) ----
// 6510 register file (defined in c64_cpu.cpp). Declared here so the desktop debug facade
// (src/desktop/debug_bridge.cpp) can read them for the CPU/disasm panels, exactly like the
// Apple II core exposes its 6502 globals. Harmless on the device (just declarations).
extern unsigned short PC, lastPC;
extern unsigned char STP, A, X, Y, SR;
void cpuReset();
void cpuLoop();
void cpuIRQ();
void cpuNMI();
// Stack ops — declared here so cpuIRQ (defined before their definitions) binds to the
// C64 versions, NOT the Apple core's global push16/push8 from proto.h (the bug that
// left the C64 stack pointer unchanged on IRQ -> RTI pulled garbage).
void push16(unsigned short pushval);
void push8(unsigned char pushval);
unsigned short pull16();
unsigned char pull8();

// ---- Memory (c64_memory.cpp) ----
void memoryAlloc();
unsigned char read8(unsigned short addr);
void write8(unsigned short addr, unsigned char val);
unsigned short read16(unsigned short address);
void write16(unsigned short address, unsigned short value);
void decodeRegister1(uint8_t val);
void adaptVICBaseAddrs(bool fromcia);

// ---- VIC-II (c64_vic.cpp) ----
void vicSetup(uint8_t *ram, const uint8_t *charrom);
void initVarsAndRegs();
uint8_t nextRasterline();
void drawRasterline();
void checkFrameColor();
bool vicIRQPending();                 // VIC raster/sprite IRQ -> CPU IRQ line ($D019 bit7)
void drawFrame(uint16_t frameColor);
const uint16_t *getC64Colors();

// ---- CIA (c64_cia.cpp) ----
void ciaReset();
unsigned char cia1Read(uint8_t reg);
void cia1Write(uint8_t reg, uint8_t val);
unsigned char cia2Read(uint8_t reg);
void cia2Write(uint8_t reg, uint8_t val);
void ciaTick(int cpuCycles);          // advance CIA timers by N CPU cycles
bool cia1IRQPending();                // Timer-A etc. -> IRQ
bool cia2NMIPending();                // RESTORE/timer -> NMI

// ---- Keyboard matrix (c64_keyboard.cpp) ----
void kbReset();
void kbSetKey(uint8_t row, uint8_t col, bool down);   // touch/host key -> matrix
uint8_t kbReadRows(uint8_t colSelect);                // CIA1 $DC01 read for selected cols
uint8_t kbReadCols(uint8_t rowSelect);                // CIA1 $DC00 reverse scan + joystick
void kbSetJoystick(uint8_t mask);                     // active-low joystick bits (port 2)
void kbSetJoystickPort(uint8_t port, uint8_t mask);   // route joystick to port 1 or 2

// ---- Snapshots (c64_snapshot.cpp; each part lives next to the file-static state it moves) ----
bool cpuWaitParked(uint32_t ms);      // (c64_cpu.cpp) wait for cpuLoop to idle in its paused spin
void cpuSnapshot(SnapIO &io);         // (c64_cpu.cpp) registers + cpuLoop's loop state, pacer reset
void ciaSnapshot(SnapIO &io);         // (c64_cia.cpp) both CIAs
void vicSnapshot(SnapIO &io);         // (c64_vic.cpp) VIC registers + raster state, records dropped

} // namespace c64

// ---- C-linkage entry points (c64.cpp), called from the dispatch / render loop ----
void c64Setup();
void c64Loop();
void c64RenderFrame();
bool c64CartIsPath(const char *path);   // (c64_crt.cpp) is this .crt the mounted cartridge?
// Snapshots: the cartridge's identity (compared on load, before anything changes; false = rejected)
// and its banking state; the SID's registers and voices; the panel's row cache.
bool c64CartSnapCheck(SnapIO &io);      // (c64_crt.cpp)
void c64CartSnapState(SnapIO &io);      // (c64_crt.cpp)
void c64SidSnapshot(SnapIO &io);        // (c64_sid.cpp)
void c64VideoInvalidate();              // (c64.cpp) next frame goes to the panel whole
