#include "../../emu.h"
#include "c64.h"

// Definitions for the C64 state shared across the C64 translation units (cpu/memory/vic).
// CPU registers + decode tables stay file-local in c64_cpu.cpp; VIC scratch stays in
// c64_vic.cpp. This file holds the cross-file state (memory, banking, VIC registers).

namespace c64 {

bool mos65c02 = false;   // C64 is NMOS 6510

// memory / banking
unsigned char *ram = nullptr;
bool bankARAM = true, bankDRAM = true, bankERAM = true, bankDIO = true;
uint8_t register1 = 0x37;

// cartridge (.crt)
uint8_t *cartROML = nullptr, *cartROMH = nullptr;
bool cartActive = false, cartExrom = true, cartGame = true;
volatile bool c64ResetReq = false;
volatile bool c64RestoreReq = false;

// VIC-II state
uint8_t vicreg[0x40];
uint8_t latchd011 = 0, latchd012 = 0;
uint16_t vicmem = 0, bitmapstart = 0x2000, screenmemstart = 1024, rasterline = 0;
uint8_t syncd020 = 0;
bool screenblank = false, badlinecond0 = false;
// The VIC renders each line into vicLineBuf (8-bit colour indices; all VIC writes are
// line-relative) and packs it into one of two 4-bit frames (c64_vic.cpp); c64RenderFrame converts
// a finished frame to RGB565 when pushing it to the TFT.
uint8_t *bitmap = nullptr;          // -> vicLineBuf once vicSetup has run; non-null = on
uint8_t *colormap = nullptr;
const uint8_t *charset = nullptr;
const uint8_t *chrom = nullptr;
uint8_t cntRefreshs = 0;

// RGB565 palette (from the C64Esp32 project)
const uint16_t c64Colors[16] = {
    0x0000, 0xffff, 0x8000, 0xa7fc, 0xc218, 0x064a, 0x0014, 0xe74e,
    0xd42a, 0x6200, 0xfbae, 0x3186, 0x73ae, 0xa7ec, 0x043f, 0xb5d6};

} // namespace c64
