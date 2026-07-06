#pragma once
// Commodore 64 core (ported from the C64Esp32 project into the multi-platform build).
// All C64-internal state/functions live in namespace c64 so they don't collide with the
// Apple II core's globals (read8/write8/ram/PC/A/X/Y/SR/cpuLoop/...). Shared CYD globals
// (tft, touchRead, running, paused, OptionsWindow, currentPlatform, joystick ADC) are used
// from global scope as-is. C-linkage entry points (bottom) are called by the dispatch.

#include <cstdint>

// C64 ROM images — loaded at boot from /roms/c64/*.bin on the SD card into PSRAM buffers
// (c64rom.cpp); pointers, not arrays, and null until c64LoadRoms() has run.
extern const unsigned char *basic_rom;
extern const unsigned char *kernal_rom;
extern const unsigned char *charset_rom;

// Load BASIC/KERNAL/CHARGEN from the SD card (/roms/c64). Returns false if any is missing or
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

// ---- VIC-II state (shared by memory + vic + render) ----
extern uint8_t vicreg[0x40];
extern uint8_t latchd011, latchd012;
extern uint16_t vicmem, bitmapstart, screenmemstart, rasterline;
extern uint8_t syncd020;
extern bool screenblank, badlinecond0;
extern uint8_t *bitmap;          // current scanline within a half (8-bit colour indices)
extern uint8_t *fbTop, *fbBot;   // 8-bit indexed framebuffer halves (lines 0..99 / 100..199)
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

} // namespace c64

// ---- C-linkage entry points (c64.cpp), called from the dispatch / render loop ----
void c64Setup();
void c64Loop();
void c64RenderFrame();
