#pragma once
// Nintendo Entertainment System core (Ricoh 2A03 = NMOS 6502 + PPU + APU).
//
// Built the same way as the C64 core: everything NES-internal lives in `namespace nes`
// so it never collides with the Apple II globals (read8/write8/ram/PC/A/X/Y/SR/cpuLoop).
// Shared CYD globals (tft, touchRead, running, paused, OptionsWindow, currentPlatform,
// joystick ADC, sharedBigBuf) are used from global scope. C-linkage entry points (bottom)
// are called by the platform dispatch (emu8.ino / video.cpp / joystick.cpp).
//
// LESSON from the C64 port: any nes:: function used before its definition MUST be declared
// here, or it silently binds to the Apple global of the same name (read8/write8/push16/...).
//
// Scope: mappers 0 (NROM), 1 (MMC1), 2 (UxROM), 3 (CNROM) — the whole ROM is held in RAM and
// banked by nes_mapper.cpp. Scanline PPU (background + sprites + sprite-0 hit), physical joystick
// as controller 1. No APU (sound) and no on-screen gamepad yet; MMC3 (mapper 4, scanline IRQ)
// and SD bank-streaming for big ROMs are future work.

#include <cstdint>

namespace nes {

// Nametable mirroring (mapper 0 uses HORIZONTAL or VERTICAL from the iNES header).
enum Mirror : uint8_t { MIRROR_HORIZONTAL = 0, MIRROR_VERTICAL = 1,
                        MIRROR_SINGLE0 = 2, MIRROR_SINGLE1 = 3 };

// ---- CPU (nes_cpu.cpp) ----
void cpuReset();
void cpuLoop();
void cpuNMI();
void cpuIRQ();
// CPU registers — exposed as plain namespace globals (like the C64 core's c64::PC) so the desktop
// debug bridge (src/desktop/debug_bridge.cpp) can read/write them for the register/step/soft-reset
// panels. Inside namespace nes these are nes::PC etc., distinct from the Apple II globals of the
// same name (see the linkage note above). Device builds are unaffected (same BSS storage).
extern unsigned short PC;
extern unsigned char STP, A, X, Y, SR;
// Stack ops live in nes_cpu.cpp as file-local always_inline helpers (defined before every use,
// so they bind to the nes versions, not the Apple core's global push16/push8 from proto.h).

// ---- CPU bus (nes_memory.cpp) ----
unsigned char read8(unsigned short addr);
void write8(unsigned short addr, unsigned char val);
unsigned short read16(unsigned short addr);
void write16(unsigned short addr, unsigned short val);

// ---- 2K internal CPU RAM ($0000-$07FF, mirrored to $1FFF) ----
// malloc'd on the NES path (nesSetup) — NOT static BSS, which would overflow dram0_0_seg
// (the static budget is ~123K and already tight with the 64K sharedBigBuf + Apple/C64 BSS).
extern uint8_t *cpuRam;

// ---- Cartridge: full PRG/CHR ROM held in RAM (nes_cart.cpp) ----
// The whole ROM is malloc'd on the NES path; the mapper windows below index into it, so any
// supported mapper just repoints windows instead of copying banks. (Big ROMs that don't fit
// the heap are refused with a log — no PSRAM, see the C64 memory notes.)
extern uint8_t  *prgRom;      // full PRG ROM image
extern uint32_t  prgRomSize;
extern uint8_t  *chrData;     // full CHR (ROM, or 8K RAM when chrIsRam)
extern uint32_t  chrSize;     // CHR size in bytes (8192 when CHR-RAM)
extern bool      chrIsRam;
extern uint8_t  *prgRam;      // optional 8K PRG-RAM at $6000 (nullptr if none)
extern uint8_t   mirrorMode;  // may be changed at run time by the mapper (e.g. MMC1)
extern uint8_t   mapperNum;   // iNES mapper number (0=NROM,1=MMC1,2=UxROM,3=CNROM)
bool nesLoadROM(const char *path);   // parse iNES + load PRG/CHR (mappers 0-4)
void loadNesFilesSync();             // scan SD root for *.nes -> nesFiles
bool nesLoadFirstRom();              // load the first loadable *.nes on the SD root
// Resolve an 8K PRG bank -> RAM pointer (full-RAM, or SD-streamed cache). Used by setPrg8k.
uint8_t *prgBankResolve(int window, int bank8k);

// ---- Mapper banking windows (nes_mapper.cpp) ----
// PRG: 4 x 8K windows for $8000/$A000/$C000/$E000.  CHR: 8 x 1K windows for $0000..$1FFF.
// read8 / chrRead index these directly so reads stay branch-free; register writes recompute them.
extern uint8_t *prgMap[4];
extern uint8_t *chrMap[8];
void mapperInit();                              // set up initial banks from mapperNum/ROM sizes
void mapperWrite(uint16_t addr, uint8_t val);   // $8000-$FFFF mapper register writes
void mapperClockScanline();                     // per-visible-scanline hook (MMC3 IRQ counter)

// ---- PPU (nes_ppu.cpp) ----
extern uint8_t *vram;                // 2K nametable RAM (malloc'd; see cpuRam note)
extern uint8_t paletteRam[0x20];     // palette RAM ($3F00-$3F1F)
extern uint8_t oam[0x100];           // 64 sprites x 4 bytes
extern uint8_t *framebuffer;         // 256x240, 8-bit master-palette indices (= sharedBigBuf)
extern volatile bool frameReady;     // set by the PPU at the end of each frame
extern volatile bool nmiPending;     // VBlank NMI edge for the CPU loop to service
extern volatile bool irqLine;        // mapper IRQ line (MMC3); level-held until $E000 ack
extern volatile bool nesResetReq;    // settings: reset CPU/PPU after loading a new ROM
extern int  dmaStallCycles;          // extra CPU cycles from an OAM DMA ($4014)
extern volatile uint32_t nesFrameCount;   // PPU frames completed (FPS diagnostic)
extern char *loadWarn;                     // startup ROM-skip warning text (malloc'd; see nes_cart.cpp)
void    ppuReset();
void    ppuStep(int cpuCycles);      // advance the PPU by cpuCycles*3 dots
extern int dotAcc;                   // PPU dot accumulator (exposed so cpuLoop inlines ppuStep)
void    endScanline();               // advance one scanline (called when dotAcc crosses 341)
uint8_t ppuRegRead(uint16_t reg);    // reg = address & 7 ($2000-$2007)
void    ppuRegWrite(uint16_t reg, uint8_t val);
void    oamDmaWrite(uint8_t page);   // $4014: copy 256 bytes from CPU page into OAM
uint8_t chrRead(uint16_t addr);      // pattern-table fetch ($0000-$1FFF)
void    chrWrite(uint16_t addr, uint8_t val);

// ---- Controller 1 (nes_memory.cpp) ----
void    setController(uint8_t buttons);  // bit0=A,1=B,2=Select,3=Start,4=Up,5=Down,6=Left,7=Right
void    controllerWrite(uint8_t val);    // $4016 strobe
uint8_t controllerRead(int port);        // $4016 / $4017 serial read

// ---- APU (nes_apu.cpp) ----
void    apuWrite(uint8_t reg, uint8_t val);   // $4000-$4017 register writes (reg = addr & 0x1F)
uint8_t apuReadStatus();                       // $4015 read (length-counter status)

// ---- NES master palette (64 colours -> RGB565), built constexpr (no tft dependency) ----
extern const uint16_t nesPalette[64];
extern uint16_t nesPaletteGray[64];      // luma version used when VIDEO toggle = MONO
void nesBuildGrayPalette();              // fill nesPaletteGray from nesPalette (call once at boot)

} // namespace nes

// ---- C-linkage entry points (nes.cpp), called from the dispatch / render loop ----
void nesSetup();
void nesLoop();
void nesRenderFrame();
void nesSetController(uint8_t buttons);
bool nesRenderLoadWarning();   // draw the startup ROM-skip warning; true while it is showing
void nesApuSetup();            // init the APU audio task (I2S DAC); call last in the NES branch
bool nesLoadSelected(const char *path);  // settings: load a .nes + reset (returns success)
void nesScanFiles();          // settings: (re)scan the SD root for *.nes into nesFiles

// Allocate NES hot RAM from internal DRAM (PSRAM fallback). See nes_globals.cpp — keeps the
// latency-sensitive interpreter/PPU buffers off the slow S3 PSRAM bus.
void *nesAllocFast(size_t n);
