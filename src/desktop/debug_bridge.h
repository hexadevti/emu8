// debug_bridge.h — desktop-only uniform debug facade over the per-platform emulator cores.
//
// The ImGui debug panels (ui_imgui.cpp) talk ONLY to this facade; the facade dispatches on
// currentPlatform to reach each core's CPU state / memory. This keeps the panels platform-agnostic
// and confines all the per-core peeking to one place. Lightweight (no ImGui include) so the core
// files can include it for the heat-map bus hooks. All behind BOARD_DESKTOP -> device unaffected.
#pragma once

#include "../../board.h"

#if defined(BOARD_DESKTOP)

#include <cstdint>

extern bool paused;   // globals.cpp — the cores' pause flag; the debugger drives it (incl. watchpoints)

// Single-step request counter. The 6502 cores' `while (paused)` spin decrements it and runs one
// instruction per unit (see src/apple2/cpu.cpp). Defined in debug_bridge.cpp; 0 = no step pending.
extern int dbgStepReq;

// ---- watchpoint state (declared before dbgBusTouch, which trips them on the bus hot path) ----------
enum { WATCH_R = 1, WATCH_W = 2 };
extern bool    g_dbgWatchAny;
extern uint8_t g_dbgWatch[];          // 0x10000, per-address WATCH_R|WATCH_W bits
extern int     g_dbgWatchHit;         // address that last tripped a watchpoint (-1 = none)

// ---- memory-access heat map -----------------------------------------------------------------------
// The cores' bus functions (read8/write8) + the opcode fetch call dbgBusTouch() so the heat panel can
// show which addresses are read/written/executed. Zero-cost when the heat map is off (one branch).
enum { DBG_HEAT_R = 0, DBG_HEAT_W = 1, DBG_HEAT_X = 2, DBG_HEAT_V = 3 };  // V = VIC-II DMA read (C64)
extern bool      g_dbgHeatOn;                  // armed only after the buffers below are allocated
extern uint32_t *g_dbgHeat[4];                 // [R/W/X/V] counters, 0x10000 each (CPU-addressable byte)
// Hot path: called from each core's read8/write8 (R/W) + opcode fetch (X). Records heat AND trips
// watchpoints. Both gated by a single bool so it costs ~one branch when neither feature is active.
static inline void dbgBusTouch(uint32_t addr, int kind) {
  uint16_t a = (uint16_t)addr;
  if (g_dbgHeatOn) g_dbgHeat[kind][a]++;
  if (g_dbgWatchAny && kind != DBG_HEAT_X) {                  // watch data reads/writes, not opcode fetch
    if (g_dbgWatch[a] & (kind == DBG_HEAT_R ? WATCH_R : WATCH_W)) { g_dbgWatchHit = a; paused = true; }
  }
}
void  dbgHeatEnable(bool on);                  // alloc/clear (on) or free (off) the counters
bool  dbgHeatEnabled();
void  dbgHeatClear();
void  dbgHeatDecay(float keep);                // multiply all counters by keep (0..1) for a fading view
const uint32_t *dbgHeatBuf(int kind);          // raw counter buffer for the visualizer (or null)

// ---- disk-read heat map (Apple II Disk II) --------------------------------------------------------
// The $C0EC nibble read calls dbgDiskRead(track, pos); the panel shows a track x position-in-track
// (~sector) grid of read intensity — which tracks the running software is loading.
#define DBG_DISK_TRACKS 80                      // rings: Apple/C64 use 35-40; MSX 3.5" .dsk uses up to 80 cylinders
#define DBG_DISK_BINS   21                      // wedges: C64 .d64 outer tracks = 21; Apple 0-15; MSX 2 sides x 9 = 18
#define DBG_DISK_TRACKLEN 5856                  // trackEncodedSize in src/apple2/disk.cpp
extern bool     g_dbgDiskHeatOn;
extern uint32_t g_dbgDiskHeat[];                // reads  [DBG_DISK_TRACKS * DBG_DISK_BINS]
extern uint32_t g_dbgDiskHeatW[];               // writes [same shape]
extern int      g_dbgDiskTrack;                 // most-recently-accessed track (for the readout)
// Both read and write are binned by the SECTOR the software is accessing (the DOS/ProDOS sector
// variable), so the wedges line up with real sectors and reads/writes are consistent.
static inline void dbgDiskRead(int track, int sector) {
  if (!g_dbgDiskHeatOn || track < 0 || track >= DBG_DISK_TRACKS) return;
  g_dbgDiskTrack = track;
  int bin = sector; if (bin < 0) bin = 0; else if (bin >= DBG_DISK_BINS) bin = DBG_DISK_BINS - 1;
  g_dbgDiskHeat[track * DBG_DISK_BINS + bin]++;
}
static inline void dbgDiskWrite(int track, int sector) {
  if (!g_dbgDiskHeatOn || track < 0 || track >= DBG_DISK_TRACKS) return;
  g_dbgDiskTrack = track;
  int bin = sector; if (bin < 0) bin = 0; else if (bin >= DBG_DISK_BINS) bin = DBG_DISK_BINS - 1;
  g_dbgDiskHeatW[track * DBG_DISK_BINS + bin]++;
}
bool dbgDiskHeatSupported();
void dbgDiskHeatEnable(bool on);
bool dbgDiskHeatEnabled();
void dbgDiskHeatClear();
void dbgDiskHeatDecay(float keep);   // multiply read+write counters by keep (0..1) for a fading view
bool dbgDiskIsFloppy();    // true = a track/sector FLOPPY is mounted (circular disk view); false = HD/ROM (grid)
int  dbgDiskTrackCount();  // number of rings to draw (tracks/cylinders) for the current platform's disk

// ---- C64 cartridge (.crt) ROM-access map ----------------------------------------------------------
// A banked C64 cart is up to 128 x 8K banks (only the current one is in the 64K window), so the regular
// memory heat map can't show it. read8's cart path calls dbgCartRead(bank, offsetIn16K); the "Disk
// read" panel draws a bank x 1K-region grid when a cart is mounted. Shares the disk Record toggle.
#define DBG_CART_BANKS 128
#define DBG_CART_BINS  16                        // 16 columns = 1K each of the $8000-$BFFF (16K) window
extern uint32_t g_dbgCartHeat[];                 // [DBG_CART_BANKS * DBG_CART_BINS] read counters
extern int      g_dbgCartBank;                   // most-recently-read bank (-1 = none)
extern int      g_dbgCartMaxBank;                // highest bank index actually read (grid row count)
static inline void dbgCartRead(int bank, int off16k) {
  if (!g_dbgDiskHeatOn || bank < 0 || bank >= DBG_CART_BANKS) return;
  g_dbgCartBank = bank;
  if (bank > g_dbgCartMaxBank) g_dbgCartMaxBank = bank;
  int bin = (off16k * DBG_CART_BINS) / 0x4000;
  if (bin < 0) bin = 0; else if (bin >= DBG_CART_BINS) bin = DBG_CART_BINS - 1;
  g_dbgCartHeat[bank * DBG_CART_BINS + bin]++;
}
bool dbgCartActive();     // true when a C64 .crt is mounted -> the Disk-read panel shows the cart map
int  dbgCartBankCount();  // total 8K banks of the mounted C64 cart (0 = none)

// --- a single CPU register for the state panel (value shown in `bits`/4 hex digits) ---
struct DbgReg {
  const char *name;
  uint32_t    value;
  uint8_t     bits;     // 8 / 16 / 24 / 32 — display width
};

// --- execution control (built on the shared `paused`; single-step where the core's pause spin
//     honors dbgStepReq — currently the 6502 cores) ---
void dbgSetPaused(bool p);
bool dbgIsPaused();
bool dbgStepSupported();
void dbgStep();              // run exactly one instruction, then re-pause
void dbgReset();             // re-exec the process (true reboot of the same platform) — desktop

// Higher-level run control. The cpuLoop pauses when PC reaches g_dbgRunToPC, or when SP rises above
// g_dbgRunUntilSP (frame returned). -1 = inactive. Set via the helpers below, which then resume.
extern int g_dbgRunToPC;
extern int g_dbgRunUntilSP;
bool dbgRunControlSupported();
void dbgStepOver();          // step, but run JSR subroutines to completion (temp bp at the return addr)
void dbgStepOut();           // run until the current subroutine returns (SP rises above entry)
void dbgRunTo(uint32_t addr);// run until PC == addr (run-to-cursor)
bool dbgSoftResetSupported();
void dbgSoftReset();         // reset the CPU (PC=reset vector) WITHOUT re-exec — keeps the debug session

// --- machine model + adjustable clock (Apple II) ---
bool  dbgAppleModelSupported();
bool  dbgGetAppleIIe();
void  dbgSetAppleIIe(bool iie);     // switch II+ <-> IIe then cold-reboot the machine
bool  dbgClockSupported();
bool  dbgGetThrottle();             // true = throttled/paced, false = uncapped (Fast)
void  dbgSetThrottle(bool on);
float dbgGetClockMhz();             // target clock when throttled (1.0 = stock)
void  dbgSetClockMhz(float mhz);
float dbgClockDefaultMhz();         // the stock/default clock to "reset" to
float dbgGetMeasuredMhz();          // live measured speed
// --- full host speed (uncapped) — available on EVERY platform. Most have a "Fast" flag the pacing
//     loop honors; Atari/PC-XT/tiny386 are inherently uncapped on desktop (dbgFullSpeedFixed = true). ---
bool  dbgFullSpeedSupported();
bool  dbgGetFullSpeed();
void  dbgSetFullSpeed(bool on);
bool  dbgFullSpeedFixed();          // true = always uncapped, can't be paced (Atari/PC-XT/tiny386)

// --- platform control (native menus, so EMU_PLATFORM env isn't needed) ---
int         dbgPlatform();              // currentPlatform
int         dbgPlatformCount();         // number of selectable platforms
const char *dbgPlatformName(int p);     // "Apple II", "C64", ... (display label)
void        dbgSwitchPlatform(int p);   // reboot (re-exec) straight into platform p
bool        dbgLoadFile(const char *sdPath);   // mount/load an SD file for the CURRENT platform
const char *dbgFileExts();              // space-separated lowercase exts to show in the file browser
// PC platforms (PC-XT, tiny386) have two drive slots — A: (floppy) and C: (hard disk) — so you can
// mount one image in each. dbgHasDriveSlots() reports that; dbgLoadFileToSlot mounts into a chosen
// slot (0 = A:, 1 = C:, -1 = auto-by-size). Other platforms ignore the slot (route to dbgLoadFile).
bool        dbgHasDriveSlots();
bool        dbgLoadFileToSlot(const char *sdPath, int slot);
const char *dbgMountedSlotPath(int slot);   // image currently set in slot (0=A:,1=C:), "" if none — for the browser markers
void        dbgEjectSlot(int slot);         // eject/unmount the image in slot (0=A:,1=C:)

// --- CPU identity + register file ---
const char *dbgCpuName();    // e.g. "MOS 6502", or "(unsupported)" for not-yet-wired platforms
int  dbgGetRegs(DbgReg *out, int maxRegs);   // fills out[], returns count
uint32_t dbgGetPC();         // linear PC for disasm/heat centering
// 6502/Z80/x86 status-flag decode for the CPU panel ("N V - B D I Z C" etc.). Returns a static
// string of single-char flag labels (MSB..LSB) and the matching status byte, or false if N/A.
bool dbgGetFlags(const char *const **labels, uint32_t *value, int *count);
// True when SP is a full 16-bit pointer to a downward-growing stack (Z80) rather than the 6502's
// fixed page-1 stack; the CPU panel uses this to render the stack readout correctly.
bool dbgStack16();

// --- memory (side-effect-free peek; addresses are CPU-space, size = dbgMemSize bytes) ---
bool     dbgMemReadable();
uint32_t dbgMemSize();
uint8_t  dbgPeek(uint32_t addr);          // never triggers soft-switches / bank flips
void     dbgPoke(uint32_t addr, uint8_t v);  // writes underlying RAM where safe; no-op otherwise
bool     dbgPokeSupported();

// --- disassembly (6502 today). Writes the mnemonic+operands for the instruction at `addr` into
//     `out`; returns the instruction length in bytes (so the panel can advance). ---
bool dbgDisasmSupported();
int  dbgDisasm(uint32_t addr, char *out, int outsz);

// --- breakpoints (6502 today). The cpuLoop pauses when PC hits an armed breakpoint. ---
extern bool g_dbgBpAny;               // fast-out: any breakpoint set at all
extern bool g_dbgBp[];                // 0x10000 PC bitmap (defined in debug_bridge.cpp)
extern bool g_dbgBreakArmed;          // false for exactly one instruction after Resume (skip current PC)
static inline bool dbgBpShouldBreak(uint32_t pc) {
  return g_dbgBpAny && g_dbgBreakArmed && g_dbgBp[pc & 0xFFFF];
}
bool dbgBpSupported();
bool dbgBpAt(uint32_t addr);
void dbgBpToggle(uint32_t addr);
void dbgBpClearAll();
int  dbgBpList(uint16_t *out, int max);   // enumerate set breakpoint addresses (for the manager panel)

// --- watchpoints: pause when a watched address is read/written (state declared above, near the bus
//     hook). Built on the same hooks as the heat map, so they cost nothing when none are set. ---
bool    dbgWatchSupported();
uint8_t dbgWatchAt(uint32_t addr);
void    dbgWatchToggle(uint32_t addr, uint8_t kindMask);
void    dbgWatchClearAll();

// --- I/O / banking state (per-platform soft switches). For the status panel. ---
struct DbgFlag { const char *label; const char *value; bool active; };
int dbgGetIoState(DbgFlag *out, int max);   // fills out[], returns count (0 = none for this platform)

// --- named memory regions (per-platform map): zero page / stack / screen / ROM / I-O / ... For the
//     heat-map region overlay + the hover tooltip. Ranges are inclusive [start,end]. ---
struct DbgRegion { uint16_t start, end; const char *name; };
int dbgMemRegions(const DbgRegion **out);   // points *out at a static table; returns the entry count (0 = none)

// --- VIC-II inspector / control (C64). Registers come from the chip's shadow; writes route through
//     the normal bus so $D011/$D016/$D018 side effects (raster latch, base-address recompute) apply. ---
bool     dbgVicSupported();
int      dbgVicReadRegs(uint8_t *out, int max);   // copy the 47 VIC registers ($D000-$D02E); returns count
void     dbgVicWriteReg(int reg, uint8_t val);    // write one VIC register (through write8)
uint16_t dbgVicBankBase();                        // CPU base of the current 16K VIC bank (vicmem)
void     dbgVicMarkFrame();                        // once/frame: tag the VIC's DMA-read regions in the heat map

// --- SID inspector / control (C64). 25 registers ($D400-$D418); writes go through the synth's
//     sidWrite() so they're audible immediately. Per-voice envelope + ADSR state are read live. ---
bool dbgSidSupported();
int  dbgSidReadRegs(uint8_t *out, int max);
void dbgSidWriteReg(int reg, uint8_t val);
void dbgSidVoice(int v, float *env, uint8_t *state);   // state: 0 off, 1 atk, 2 dec, 3 sus, 4 rel

// --- PPU / VRAM inspector (NES). The analog of the VIC-II panel: pattern tables, nametables (VRAM),
//     palette RAM and OAM. Render helpers fill caller-owned ABGR8888 buffers the UI uploads as textures. ---
bool     dbgNesPpuSupported();
int      dbgNesReadPalette(uint8_t *out, int max);   // 32 palette-RAM bytes ($3F00-$3F1F)
uint32_t dbgNesMasterRGBA(int masterIdx);            // master palette[idx&63] -> ABGR8888 (swatches)
int      dbgNesReadOam(uint8_t *out, int max);       // 256 OAM bytes (64 sprites x 4: Y,tile,attr,X)
void     dbgNesRenderPattern(int half, int pal4, uint32_t *out);  // 128x128 ABGR8888 ($0000 / $1000)
void     dbgNesRenderNametables(uint32_t *out);                   // 512x480 ABGR8888 (2x2 nametables)
void     dbgNesViewport(int *x, int *y);             // current scroll top-left in the 512x480 NT space

// --- VDP / VRAM inspector (MSX). The TMS9918 VRAM is a SEPARATE 16K bus (ports $98/$99), invisible to
//     the CPU-space Memory panel — this is its viewer: raw VRAM peek/poke, the fixed 16-colour palette,
//     the VDP register file (to derive the table bases), and a pattern-generator tile-sheet render. ---
bool     dbgMsxVdpSupported();
uint8_t  dbgMsxPeekVram(uint32_t addr);              // side-effect-free VRAM read (addr & 0x3FFF)
void     dbgMsxPokeVram(uint32_t addr, uint8_t v);   // write a VRAM byte (shows next frame)
int      dbgMsxVdpRegs(uint8_t *out, int max);       // copy R0-R7 (caller derives the table bases)
uint32_t dbgMsxPaletteRGBA(int idx);                 // fixed TMS9918 palette[idx&15] -> ABGR8888 (swatches)
void     dbgMsxRenderPatterns(uint32_t *out, int bank);  // 128x128 ABGR8888: 256 8x8 patterns (bank 0-2 for G2)

#endif // BOARD_DESKTOP
