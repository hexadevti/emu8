#pragma once
// Atari 2600 (VCS) core — MOS 6507 (a 6502 with a 13-bit bus) + TIA (video/sound) + 6532 RIOT
// (128 bytes RAM + interval timer + I/O ports).
//
// Built the same way as the NES core: everything 2600-internal lives in `namespace atari`
// so it never collides with the Apple II globals (read8/write8/ram/PC/A/X/Y/SR/cpuLoop).
// Shared CYD globals (tft, touchRead, running, paused, OptionsWindow, currentPlatform,
// joystick ADC, sharedBigBuf, sound/volume/videoColor) are used from global scope. The
// C-linkage entry points (bottom) are called by the platform dispatch (emu8.ino /
// video.cpp / joystick.cpp).
//
// LESSON from the NES/C64 ports: any atari:: function used before its definition MUST be
// declared here, or it silently binds to the Apple global of the same name (read8/write8/...).
//
// Scope: 6507 + scanline-stepped TIA ("racing the beam": playfield, 2 players, 2 missiles,
// ball, collisions, HMOVE), 6532 RIOT, cartridges 2K/4K + F8/F6/F4 bank-switching + Superchip
// (+128B RAM). 2-channel TIA audio via I2S DAC. The whole ROM (<=32K) is held in RAM.

#include <cstdint>

namespace atari {

// ---- CPU (atari_cpu.cpp) ----
void cpuReset();
void cpuLoop();

// ---- CPU bus (atari_memory.cpp) ----
unsigned char read8(unsigned short addr);
void write8(unsigned short addr, unsigned char val);
unsigned short read16(unsigned short addr);

// ---- RIOT 6532 (atari_riot.cpp): 128B RAM + interval timer + I/O ports ----
extern uint8_t *riotRam;                     // $0080-$00FF (and stack mirror $0180-$01FF); malloc'd
void    riotReset();
void    riotTick(int cpuCycles);             // advance the interval timer by cpuCycles
uint8_t riotRead(uint16_t addr);             // $0280-$029F (SWCHA/SWCHB/INTIM/INSTAT)
void    riotWrite(uint16_t addr, uint8_t val);
// Input ports, latched by the joystick task via atariSetInput (active-low, like real hardware):
extern volatile uint8_t swcha;               // joystick directions (P0 high nibble, P1 low)
extern volatile uint8_t swchb;               // console switches (reset/select/colour/difficulty)
extern volatile uint8_t inpt4;               // P0 fire on bit7 (0 = pressed)
extern volatile uint8_t inpt5;               // P1 fire on bit7

// ---- TIA video (atari_tia.cpp) ----
extern uint8_t  *framebuffer;                // 160x192, 8-bit TIA palette indices (= sharedBigBuf)
extern volatile uint32_t atariFrameCount;    // completed fields (FPS diagnostic)
extern volatile bool wsyncStall;             // CPU halted until end of scanline (TIA $02)
extern int colorClock;                       // current dot 0..227 (exposed so cpuLoop inlines)
void    tiaReset();
void    tiaStep(int cpuCycles);              // advance 3*cpuCycles dots (out-of-line version)
void    tiaLineWrap();                       // finish a scanline (called by tiaStepInline on wrap)
int     tiaTickToLineEnd();                  // WSYNC: drain to dot 0, return cpu cycles consumed

// Inlined beam step for the CPU hot loop: advance colorClock by one add and handle the (rare, ~1 in
// 25 instructions) line wrap out-of-line. Saves the per-instruction call + the old per-dot loop.
static inline void tiaStepInline(int cpuCycles) {
  colorClock += cpuCycles * 3;
  if (colorClock >= 228) { colorClock -= 228; tiaLineWrap(); }
}
void    tiaWrite(uint8_t reg, uint8_t val);  // $00-$2C register writes (reg = addr & 0x3F)
uint8_t tiaRead(uint8_t reg);                // collisions $00-$07, INPT0-5 $08-$0D

// ---- TIA NTSC palette (128 colours -> RGB565), built constexpr (no tft dependency) ----
extern const uint16_t atariPalette[128];
extern uint16_t *atariPaletteGray;           // luma version (VIDEO toggle = MONO); malloc'd at boot
void atariBuildGrayPalette();                // fill atariPaletteGray from atariPalette (once at boot)

// ---- Cartridge (atari_cart.cpp) ----
extern uint8_t  *cartRom;                    // full ROM image held in RAM
extern uint32_t  cartSize;
uint8_t cartRead(uint16_t addr);             // $1000-$1FFF (bank hotspots + Superchip RAM)
void    cartWrite(uint16_t addr, uint8_t val);
bool    atariLoadROM(const char *path);      // load .a26/.bin, detect bank scheme
void    loadAtariFilesSync();                // scan SD root for *.a26 / *.bin -> atariFiles
bool    atariLoadFirstRom();                 // load the first loadable ROM on the SD root
extern char *loadWarn;                       // startup ROM-skip warning text (malloc'd at boot)
extern volatile bool atariResetReq;          // settings: reset after loading a new ROM

// ---- Audio (atari_audio.cpp): 2 TIA voices ----
void    audioWrite(uint8_t reg, uint8_t val);  // AUDC0/F0/V0/C1/F1/V1 (reg = addr & 0x3F)

} // namespace atari

// ---- C-linkage entry points (atari.cpp), called from the platform dispatch ----
void atariSetup();
void atariLoop();
void atariRenderFrame();
void atariSetInput(uint8_t dirBits, bool fire, bool select, bool reset);  // bit0=U,1=D,2=L,3=R
bool atariRenderLoadWarning();   // draw the startup ROM-skip warning; true while it is showing
void atariAudioSetup();          // init the TIA audio task (I2S DAC); call last in the setup branch
bool atariLoadSelected(const char *path);  // settings: load a ROM + reset (returns success)
void atariScanFiles();           // settings: (re)scan the SD root for *.a26 / *.bin
