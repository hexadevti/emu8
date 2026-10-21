// μ6502 - Barebones 6502 Emulator By Damian Peckett
// dpeckett.com, <damian@pecke.tt>
// Ported from C64Esp32 cpu.ino into namespace c64 (6510). IRQ/NMI + cycle-based raster
// timing added for the multi-platform CYD build.

#include "../../emu.h"
#if defined(BOARD_DESKTOP)
#include "../desktop/debug_bridge.h"   // desktop debugger hooks (breakpoints/step/heat); no-op on device
#endif
#include "c64.h"
#include "../shared/snapshot.h"
#if defined(BOARD_PICOCALC)
#include <hardware/timer.h>   // timer_hw (C64_NOW_US)
#endif

namespace c64 {

// Address Modes
#define AD_IMP  0x01
#define AD_A    0x02
#define AD_ABS  0x03
#define AD_ABSX 0x04
#define AD_ABSY 0x05
#define AD_IMM  0x06
#define AD_IND  0x07
#define AD_INDX 0x08
#define AD_INDY 0x09
#define AD_REL  0x0A
#define AD_ZPG  0x0B
#define AD_ZPGX 0x0C
#define AD_ZPGY 0x0D

// SR Flag Modes
#define FL_NONE 0x00  // ---- ----
#define FL_N    0x80  // N--- ----
#define FL_V    0x40  // -V-- ----
#define FL_Z    0x20  // --Z- ----
#define FL_C    0x10  // ---C ----
#define FL_ZN   0xA0  // N-Z- ----
#define FL_NVC  0xD0  // NV-C ----
#define FL_NVZ  0xE0  // NVZ- ----
#define FL_ZNC  0xB0  // N-ZC ----
#define FL_ZC   0x30  // --ZC ----
#define FL_ALL  0xF0  // NVZC ----

//Unimplemented ops
#define UNDF 0x00

//Other constants
#define SR_CARRY      0x01
#define SR_ZERO       0x02
#define SR_INT        0x04
#define SR_DEC        0x08
#define SR_BRK        0x10
#define SR_FIXED_BITS 0x20
#define SR_OVER       0x40
#define SR_NEG        0x80

//Stack pointer base address
#define STP_BASE 0x100
long count = 0;
long cycleCount = 0;
uint32_t cpuCycleCount = 0;
uint32_t lastCpuCycleCount = 0;
uint32_t diffCpuCycleCount = 0;

// NMOS 6510: high nibble SR flags, low nibble address mode, for all 256 opcodes. The undocumented
// opcodes (LAX, SAX, DCP, ISC, SLO, RLA, SRE, RRA, the NOP family...) carry their real address
// mode so they consume the right number of operand bytes; they set their flags themselves.
// (This core used to fall back on the 65C02 cases from the Apple II core: BRA, STZ, PHX... ran on
// opcodes that are NOPs or illegals on the 6510, and every unlisted opcode was one byte long.)
C64_HOT_DATA const unsigned char flags6502[] = {
  //X0             X1                X2      X3       X4               X5                X6                X7       X8              X9                XA              XB       XC               XD                XE                XF
  AD_IMP,          AD_INDX,          AD_IMP, AD_INDX, AD_ZPG,          FL_ZN | AD_ZPG,   FL_ZNC | AD_ZPG,  AD_ZPG,  AD_IMP,         FL_ZN | AD_IMM,   FL_ZNC | AD_A,  AD_IMM,  AD_ABS,          FL_ZN | AD_ABS,   FL_ZNC | AD_ABS,  AD_ABS,  // 0X
  AD_REL,          FL_ZN | AD_INDY,  AD_IMP, AD_INDY, AD_ZPGX,         FL_ZN | AD_ZPGX,  FL_ZNC | AD_ZPGX, AD_ZPGX, AD_IMP,         FL_ZN | AD_ABSY,  AD_IMP,         AD_ABSY, AD_ABSX,         FL_ZN | AD_ABSX,  FL_ZNC | AD_ABSX, AD_ABSX, // 1X
  AD_ABS,          FL_ZN | AD_INDX,  AD_IMP, AD_INDX, FL_Z | AD_ZPG,   FL_ZN | AD_ZPG,   FL_ZNC | AD_ZPG,  AD_ZPG,  AD_IMP,         FL_ZN | AD_IMM,   FL_ZNC | AD_A,  AD_IMM,  FL_Z | AD_ABS,   FL_ZN | AD_ABS,   FL_ZNC | AD_ABS,  AD_ABS,  // 2X
  AD_REL,          FL_ZN | AD_INDY,  AD_IMP, AD_INDY, AD_ZPGX,         FL_ZN | AD_ZPGX,  FL_ZNC | AD_ZPGX, AD_ZPGX, AD_IMP,         FL_ZN | AD_ABSY,  AD_IMP,         AD_ABSY, AD_ABSX,         FL_ZN | AD_ABSX,  FL_ZNC | AD_ABSX, AD_ABSX, // 3X
  AD_IMP,          FL_ZN | AD_INDX,  AD_IMP, AD_INDX, AD_ZPG,          FL_ZN | AD_ZPG,   FL_ZNC | AD_ZPG,  AD_ZPG,  AD_IMP,         FL_ZN | AD_IMM,   FL_ZNC | AD_A,  AD_IMM,  AD_ABS,          FL_ZN | AD_ABS,   FL_ZNC | AD_ABS,  AD_ABS,  // 4X
  AD_REL,          FL_ZN | AD_INDY,  AD_IMP, AD_INDY, AD_ZPGX,         FL_ZN | AD_ZPGX,  FL_ZNC | AD_ZPGX, AD_ZPGX, AD_IMP,         FL_ZN | AD_ABSY,  AD_IMP,         AD_ABSY, AD_ABSX,         FL_ZN | AD_ABSX,  FL_ZNC | AD_ABSX, AD_ABSX, // 5X
  AD_IMP,          AD_INDX,          AD_IMP, AD_INDX, AD_ZPG,          AD_ZPG,           FL_ZNC | AD_ZPG,  AD_ZPG,  FL_ZN | AD_IMP, AD_IMM,           FL_ZNC | AD_A,  AD_IMM,  AD_IND,          AD_ABS,           FL_ZNC | AD_ABS,  AD_ABS,  // 6X
  AD_REL,          AD_INDY,          AD_IMP, AD_INDY, AD_ZPGX,         AD_ZPGX,          FL_ZNC | AD_ZPGX, AD_ZPGX, AD_IMP,         AD_ABSY,          AD_IMP,         AD_ABSY, AD_ABSX,         AD_ABSX,          FL_ZNC | AD_ABSX, AD_ABSX, // 7X
  AD_IMM,          AD_INDX,          AD_IMM, AD_INDX, AD_ZPG,          AD_ZPG,           AD_ZPG,           AD_ZPG,  FL_ZN | AD_IMP, AD_IMM,           FL_ZN | AD_IMP, AD_IMM,  AD_ABS,          AD_ABS,           AD_ABS,           AD_ABS,  // 8X
  AD_REL,          AD_INDY,          AD_IMP, AD_INDY, AD_ZPGX,         AD_ZPGX,          AD_ZPGY,          AD_ZPGY, FL_ZN | AD_IMP, AD_ABSY,          AD_IMP,         AD_ABSY, AD_ABSX,         AD_ABSX,          AD_ABSY,          AD_ABSY, // 9X
  FL_ZN | AD_IMM,  FL_ZN | AD_INDX,  FL_ZN | AD_IMM, AD_INDX, FL_ZN | AD_ZPG, FL_ZN | AD_ZPG, FL_ZN | AD_ZPG, AD_ZPG, FL_ZN | AD_IMP, FL_ZN | AD_IMM, FL_ZN | AD_IMP, AD_IMM, FL_ZN | AD_ABS, FL_ZN | AD_ABS, FL_ZN | AD_ABS, AD_ABS, // AX
  AD_REL,          FL_ZN | AD_INDY,  AD_IMP, AD_INDY, FL_ZN | AD_ZPGX, FL_ZN | AD_ZPGX,  FL_ZN | AD_ZPGY,  AD_ZPGY, AD_IMP,         FL_ZN | AD_ABSY,  FL_ZN | AD_IMP, AD_ABSY, FL_ZN | AD_ABSX, FL_ZN | AD_ABSX,  FL_ZN | AD_ABSY,  AD_ABSY, // BX
  FL_ZNC | AD_IMM, FL_ZNC | AD_INDX, AD_IMM, AD_INDX, FL_ZNC | AD_ZPG, FL_ZNC | AD_ZPG,  FL_ZN | AD_ZPG,   AD_ZPG,  FL_ZN | AD_IMP, FL_ZNC | AD_IMM,  FL_ZN | AD_IMP, AD_IMM,  FL_ZNC | AD_ABS, FL_ZNC | AD_ABS,  FL_ZN | AD_ABS,   AD_ABS,  // CX
  AD_REL,          FL_ZNC | AD_INDY, AD_IMP, AD_INDY, AD_ZPGX,         FL_ZNC | AD_ZPGX, FL_ZN | AD_ZPGX,  AD_ZPGX, AD_IMP,         FL_ZNC | AD_ABSY, AD_IMP,         AD_ABSY, AD_ABSX,         FL_ZNC | AD_ABSX, FL_ZN | AD_ABSX,  AD_ABSX, // DX
  FL_ZNC | AD_IMM, AD_INDX,          AD_IMM, AD_INDX, FL_ZNC | AD_ZPG, AD_ZPG,           FL_ZN | AD_ZPG,   AD_ZPG,  FL_ZN | AD_IMP, AD_IMM,           AD_IMP,         AD_IMM,  FL_ZNC | AD_ABS, AD_ABS,           FL_ZN | AD_ABS,   AD_ABS,  // EX
  AD_REL,          AD_INDY,          AD_IMP, AD_INDY, AD_ZPGX,         AD_ZPGX,          FL_ZN | AD_ZPGX,  AD_ZPGX, AD_IMP,         AD_ABSY,          AD_IMP,         AD_ABSY, AD_ABSX,         AD_ABSX,          FL_ZN | AD_ABSX,  AD_ABSX  // FX
};
static_assert(sizeof(flags6502) == 256, "flags6502 needs an entry per opcode");

// NMOS base cycle counts (0 = KIL, which jams the CPU). A page crossed by an indexed read and a
// taken branch add their cycles in cpuLoop. (The first row used to sit inside the comment on
// this line, so every opcode was charged the cycles of opcode+16 and $Fx read past the table.)
C64_HOT_DATA const uint8_t cycles[] = {
  7, 6, 0, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,   // 0X
  2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,   // 1X
  6, 6, 0, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,   // 2X
  2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,   // 3X
  6, 6, 0, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,   // 4X
  2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,   // 5X
  6, 6, 0, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,   // 6X
  2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,   // 7X
  2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,   // 8X
  2, 6, 0, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,   // 9X
  2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,   // AX
  2, 5, 0, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,   // BX
  2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,   // CX
  2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,   // DX
  2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,   // EX
  2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7 }; // FX
static_assert(sizeof(cycles) == 256, "cycles needs an entry per opcode");

// CPU registers
unsigned short PC;
unsigned short lastPC;
unsigned char STP = 0xFD, A = 0x00, X = 0x00, Y = 0x00, SR = SR_FIXED_BITS;

//Execution variables
unsigned char opcode, opflags;
unsigned short argument_addr;

//Temporary variables for flag generation
unsigned char value8;
unsigned short value16, value16_2, result;

bool debug = false;

void cpuReset()
{
  PC = read16(0xFFFC);
  STP = 0xFD;
}

// Hardware interrupt entry. Pushes PC + status (B clear) and vectors through $FFFE/$FFFA.
C64_HOT void cpuIRQ() {
  push16(PC);
  push8((SR & ~SR_BRK) | SR_FIXED_BITS);
  SR |= SR_INT;
  PC = read16(0xFFFE);
}

C64_HOT void cpuNMI() {
  push16(PC);
  push8((SR & ~SR_BRK) | SR_FIXED_BITS);
  SR |= SR_INT;
  PC = read16(0xFFFA);
}

C64_HOT void setflags() {
  // Mask out affected flags
  switch (opflags & 0xF0) {
    case  FL_ZN: SR &= 0x7D; break; // 1010 0000   0111 1101
    case FL_ZNC: SR &= 0x7C; break; // 1011 0000   0111 1100
    case  FL_ZC: SR &= 0xFC; break; // 0011 0000   1111 1100
    case FL_ALL: SR &= 0x3C; break; // 1111 0000   0011 1100
    case   FL_Z: SR &= 0xFD; break; // 0010 0000   1111 1101
  }
  
  // if (lastPC >= 0x5d00 && lastPC < 0x5e00) {
  //   sprintf(buf, "setflag: opflag=%02X result=%02X opflags & 0x80=%02X result & 0x80=%02X", opflags, result, opflags & 0x80, result & 0x80);
  //   printLog(buf);
  // }
  
  // Set various status flags
  if (opflags & FL_N) SR |= (result & 0x80);                    //negative
  if (opflags & FL_Z) SR |= (((result & 0xFF) == 0) ? 0x02 : 0);  //zero
  if (opflags & FL_C) SR |= ((result & 0xFF00) ? 0x01 : 0);       //carry
  if (opflags & FL_V) SR |= ((result ^ ((unsigned short)A)) & (result ^ value16) & 0x0080) >> 1; // overflow
}

// Stack functions
C64_HOT void push16(unsigned short pushval) {
  write8(STP_BASE + (STP--), (pushval >> 8) & 0xFF);
  write8(STP_BASE + (STP--), pushval & 0xFF);
}

C64_HOT void push8(unsigned char pushval) {
  write8(STP_BASE + (STP--), pushval);
}

C64_HOT unsigned short pull16() {
  STP++;
	value16 = read8(STP_BASE + (STP));
  //printAddrVal("pull16", STP_BASE + (STP),value16);
	STP++;
	value16 = value16 | ((unsigned short)read8(STP_BASE + (STP)) << 8);
  //printAddrVal("pull16-2", STP_BASE + (STP),value16);
	return value16;}

C64_HOT unsigned char pull8() {
  return read8(STP_BASE + (++STP));
}

// ---- NMOS ALU helpers (ADC/SBC with the 6502's decimal-mode flag behaviour, compares, and the
// read-modify-write store used by the undocumented opcodes as well as ASL/ROL/LSR/ROR/INC/DEC) ----
static inline void setNZ(uint8_t v) { SR = (SR & 0x7D) | (v & 0x80) | (v ? 0 : SR_ZERO); }

static inline void cmpReg(uint8_t r, uint8_t v) {
  SR = (SR & ~SR_CARRY) | (r >= v ? SR_CARRY : 0);
  setNZ((uint8_t)(r - v));
}

static inline void doADC(uint8_t v) {
  unsigned c = SR & SR_CARRY;
  SR &= ~(SR_NEG | SR_OVER | SR_ZERO | SR_CARRY);
  if (SR & SR_DEC) {
    // NMOS decimal: Z from the binary sum, N and V from the half-adjusted high nibble.
    unsigned lo = (A & 0x0F) + (v & 0x0F) + c;
    if (lo > 9) lo += 6;
    unsigned hi = (A >> 4) + (v >> 4) + (lo > 0x0F ? 1 : 0);
    if (((A + v + c) & 0xFF) == 0) SR |= SR_ZERO;
    if (hi & 0x08) SR |= SR_NEG;
    if (~(A ^ v) & (A ^ (hi << 4)) & 0x80) SR |= SR_OVER;
    if (hi > 9) hi += 6;
    if (hi > 0x0F) SR |= SR_CARRY;
    A = (uint8_t)((hi << 4) | (lo & 0x0F));
  } else {
    unsigned r = A + v + c;
    if (r > 0xFF) SR |= SR_CARRY;
    if (~(A ^ v) & (A ^ r) & 0x80) SR |= SR_OVER;
    A = (uint8_t)r;
    if (A == 0) SR |= SR_ZERO;
    SR |= A & 0x80;
  }
}

static inline void doSBC(uint8_t v) {
  unsigned borrow = (SR & SR_CARRY) ? 0 : 1;
  unsigned r = (unsigned)A - v - borrow;          // NMOS: all flags come from the binary result
  SR &= ~(SR_NEG | SR_OVER | SR_ZERO | SR_CARRY);
  if (r < 0x100) SR |= SR_CARRY;
  if ((A ^ v) & (A ^ r) & 0x80) SR |= SR_OVER;
  if ((r & 0xFF) == 0) SR |= SR_ZERO;
  SR |= r & 0x80;
  if (SR & SR_DEC) {
    unsigned t = (A & 0x0F) - (v & 0x0F) - borrow;
    if (t & 0x10) t = ((t - 6) & 0x0F) | ((A & 0xF0) - (v & 0xF0) - 0x10);
    else          t = (t & 0x0F) | ((A & 0xF0) - (v & 0xF0));
    if (t & 0x100) t -= 0x60;
    A = (uint8_t)t;
  } else {
    A = (uint8_t)r;
  }
}

// Read-modify-write store. The 6510 writes the unmodified value back first, then the new one;
// only I/O can tell, but there it matters: INC/ASL/LSR $D019 acknowledges the VIC interrupt
// with that first write, since the new value no longer has the pending bits.
static inline void rmwWrite(unsigned short addr, uint8_t oldv, uint8_t newv) {
  if ((addr & 0xF000) == 0xD000) write8(addr, oldv);
  write8(addr, newv);
}

// SHA/SHX/SHY/TAS: store v AND (high byte of the unindexed address + 1); when indexing crossed a
// page, that value also replaces the high byte of the target address.
static inline void shStore(unsigned short base, unsigned short addr, uint8_t v) {
  uint8_t val = v & (uint8_t)((base >> 8) + 1);
  if ((base ^ addr) & 0xFF00) addr = (unsigned short)((addr & 0x00FF) | (val << 8));
  write8(addr, val);
}

// Virtual-disk KERNAL LOAD trap. The KERNAL LOAD vector $FFD5 JMPs to $F49E; when the
// CPU reaches $F49E with device 8 selected and a .d64 mounted, we service the load here
// natively (read the directory + sector chain straight off the SD image) and RTS back to
// the caller, instead of running the serial/IEC routine (there is no real 1541).
//
// At $F49E: A = verify flag, X/Y = alt load address, and SETNAM/SETLFS have set the
// zero-page IEC vars: $B7 FNLEN, $BB/$BC FNADR, $BA device, $B9 secondary address.
// On a relocating load (SA=0, e.g. LOAD"name",8) BASIC supplies the address; on a
// non-relocating load (SA<>0, e.g. LOAD"*",8,1) the file's own load address is used.
// Returns true when handled (PC set to the RTS target).
// Throughput meter: the live 6510 speed (c64MeasuredMhz, shown in the settings title) and the share
// of the CPU core spent rendering VIC lines, logged about once a second. Restarted after a pause so
// the time the settings window was open does not count against it.
static uint32_t perfCyc = 0, perfVicUs = 0, perfLastMs = 0;
volatile uint32_t c64PerfKhz = 0;
volatile uint8_t c64PerfVicPct = 0;
volatile uint32_t c64MaxLineGapUs = 0;

// The 1 MHz pacer and the render timer read the clock on the hot path. micros() is out of line and
// lives in flash; on the PicoCalc the free-running 1 MHz timer register is the same value as one
// load (see CPU_NOW_US in src/apple2/cpu.cpp).
#if defined(BOARD_PICOCALC)
#define C64_NOW_US() (timer_hw->timerawl)
#else
#define C64_NOW_US() ((uint32_t)micros())
#endif
#define C64_US_PER_CYC_Q16 66517u
static uint32_t vicStolenCyc = 0;   // VIC DMA cycles not yet charged to the pacer   // 65536 / 0.985248 MHz (PAL): real microseconds per 6510 cycle
// The 1 MHz pacer's running total (see cpuLoop). Restarted after every pause, so the time the
// machine stood still is never caught up on.
static uint32_t paceBaseUs = 0, paceTargetUs = 0, paceAccQ16 = 0;
static void paceRestart() { paceBaseUs = C64_NOW_US(); paceTargetUs = 0; paceAccQ16 = 0; vicStolenCyc = 0; }

// cpuLoop's own loop state, handed out while it idles in its paused spin: a snapshot (render task)
// saves it from here, and a loaded one is picked up when the spin ends. cpuParked says it is there.
static volatile bool cpuParked = false;
static volatile int32_t parkRasterAcc = 0;
static volatile uint8_t parkIrqShadowSR = SR_INT;
static volatile bool parkNmiPrev = false;

static void perfSample() {
  uint32_t now = millis();
  if (perfLastMs == 0) { perfLastMs = now; perfCyc = 0; perfVicUs = 0; return; }
  uint32_t ms = now - perfLastMs;
  if (ms < 1000) return;
  c64MeasuredMhz = (float)perfCyc / ((float)ms * 1000.0f);
#if defined(BOARD_PICOCALC)
  // No log line from here on the device: a USB print stalls this core for a moment once a second,
  // the beam then races to catch up, and the render core falls behind it -- the picture flickered
  // on that beat. The render task logs the speed with its own 5 s report (c64.cpp).
  c64PerfKhz = perfCyc / ms;
  c64PerfVicPct = (uint8_t)((uint64_t)perfVicUs / 10 / ms);
#else
  static uint32_t lastColl = 0;
  snprintf(buf, sizeof(buf), "C64: 6510 %.2f MHz (%d%% of PAL 0.985), VIC render %d%% of CPU core, %lu collision lines",
           c64MeasuredMhz, (int)(c64MeasuredMhz * 100.0f / 0.985f + 0.5f),
           (int)((uint64_t)perfVicUs / 10 / ms), (unsigned long)(c64CollLines - lastColl));
  lastColl = c64CollLines;
  printLog(buf);
#endif
  perfLastMs = now; perfCyc = 0; perfVicUs = 0;
}

static bool c64LoadTrap() {
  // Device 8, or device 1 (tape: a bare LOAD / LOAD"NAME") while a disk is mounted -- there is
  // no tape drive, so that loads from the disk the way LOAD"*",8 / LOAD"NAME",8 would.
  uint8_t dev = read8(0xBA);
  if (dev != 8 && dev != 1) return false;    // other devices -> let the ROM handle it
  if (!::c64DiskMounted()) return false;     // nothing mounted -> ROM ("device not present")
  // Only when $F49E really is the KERNAL: a game running with the KERNAL banked out ($01=$35) or
  // an Ultimax cart has its own code there, and $BA keeps its 8 after any disk load.
  if (bankERAM || (cartActive && cartExrom && !cartGame)) return false;

  if (A != 0) {                              // VERIFY: report a match, never write RAM
    write8(0x90, 0x00);
    SR &= ~SR_CARRY;
    PC = pull16() + 1;
    return true;
  }

  write8(0xC3, X);                           // replicate $F49E: STX $C3 / STY $C4 (MEMUSS)
  write8(0xC4, Y);
  uint16_t altAddr = (uint16_t)(X | (Y << 8));

  uint8_t  len  = read8(0xB7);
  uint16_t fnad = (uint16_t)(read8(0xBB) | (read8(0xBC) << 8));
  uint8_t  name[24];
  if (len > 24) len = 24;
  for (uint8_t i = 0; i < len; i++) name[i] = read8(fnad + i);

  bool useFileAddr = (read8(0xB9) != 0);     // secondary address: 0 = relocate to altAddr
  uint16_t start = 0, end = 0;
  int err;
  if (len >= 1 && name[0] == '$')            // directory listing
    err = ::c64D64LoadDirectory(altAddr, &end) ? 0 : 4;
  else
    err = ::c64D64LoadByName(name, len, useFileAddr, altAddr, &start, &end);

  if (err == 0) {
    write8(0x90, 0x00);                       // STATUS = OK
    write8(0xAE, end & 0xFF);                 // EAL/EAH = end address (+1)
    write8(0xAF, (end >> 8) & 0xFF);
    X = end & 0xFF;                           // KERNAL LOAD returns end addr in X/Y;
    Y = (end >> 8) & 0xFF;                    // BASIC reads it to set VARTAB + relink
    A = 0;
    SR &= ~SR_CARRY;                          // carry clear = no error
  } else {
    A = (uint8_t)err;                         // 4 = FILE NOT FOUND
    SR |= SR_CARRY;                           // carry set = error
  }

  PC = pull16() + 1;                          // RTS to the JSR $FFD5 caller
  return true;
}

// ---- Snapshots (see c64_snapshot.cpp) ----
bool cpuWaitParked(uint32_t ms) {
  for (uint32_t t0 = millis(); !cpuParked && millis() - t0 < ms; ) delay(5);
  return cpuParked;
}

// The 6510 at an instruction boundary (cpuLoop is in its paused spin), plus the loop's raster-cycle
// accumulator and interrupt edge state. The per-instruction scratch (opcode, argument_addr, value16...)
// is rebuilt by the next instruction and is not saved.
void cpuSnapshot(SnapIO &io) {
  uint16_t pc = PC, lpc = lastPC;
  int32_t acc = parkRasterAcc;
  uint8_t shadow = parkIrqShadowSR;
  bool nmi = parkNmiPrev;
  io.val(pc); io.val(lpc);
  io.val(STP); io.val(A); io.val(X); io.val(Y); io.val(SR);
  io.val(acc); io.val(shadow); io.flag(nmi);
  if (!io.loading || !io.ok) return;
  PC = pc; lastPC = lpc;
  parkRasterAcc = (acc >= 0 && acc < 63 + 64) ? acc : 0;
  parkIrqShadowSR = shadow;
  parkNmiPrev = nmi;
  perfLastMs = 0;
  paceRestart();
}

C64_HOT void cpuLoop() {
  write8(0, 0x2f);
  write8(1, 0x37);
  write8(0x8004, 0);
  PC = read16(0xfffc);
  STP = 0xFD;
  
  lastPC = PC;

  int rasterCycleAcc = 0;
  uint8_t irqShadowSR = SR_INT;   // I-flag as of the previous instruction (see below)
  bool nmiPrev = false;           // previous NMI line state (NMI is edge-triggered)
  while (running)
  {
#if defined(BOARD_DESKTOP)
    // Desktop debugger: break at the upcoming PC (breakpoint / run-to-cursor / step-over / step-out).
    if (dbgBpShouldBreak(PC)) paused = true;                                                      // breakpoint
    if (g_dbgRunToPC >= 0 && PC == (uint16_t)g_dbgRunToPC) { paused = true; g_dbgRunToPC = -1; }  // run-to / step-over
    if (g_dbgRunUntilSP >= 0 && STP > (uint8_t)g_dbgRunUntilSP) { paused = true; g_dbgRunUntilSP = -1; } // step-out
#endif
    if (paused)
    {
      parkRasterAcc = rasterCycleAcc;                // a snapshot may save (or replace) these
      parkIrqShadowSR = irqShadowSR;
      parkNmiPrev = nmiPrev;
      cpuParked = true;
      while (paused)
      {
#if defined(BOARD_DESKTOP)
        if (dbgStepReq > 0) { dbgStepReq--; break; }   // debugger single-step: run exactly one instruction
#endif
        delay(100);
        perfLastMs = 0;                              // restart the speed meter after the pause
      }
      cpuParked = false;
      rasterCycleAcc = parkRasterAcc;
      irqShadowSR = parkIrqShadowSR;
      nmiPrev = parkNmiPrev;
      paceRestart();                                 // no racing to catch up on the pause
    }
#if defined(BOARD_DESKTOP)
    g_dbgBreakArmed = true;                          // re-arm after passing the breakpoint check once
#endif

    // Reset request (e.g. after mounting a .crt): re-enter the reset vector so the KERNAL
    // re-detects the cartridge (CBM80 at $8004) or an Ultimax cart's own $FFFC vector.
    if (c64ResetReq) {
      c64ResetReq = false;
      // A cartridge cold-starts via JMP($8000)/($FFFC) and the KERNAL then SKIPS its usual
      // hardware init (IOINIT/CINT) for an autostart cart, so the cart expects power-on
      // VIC/CIA state. But loading a cart from the menu leaves the previous BASIC/menu
      // session's VIC + CIA state behind, which corrupts carts that rely on the defaults
      // (black screen / garbage / hang). Re-init the chips so the cart boots from a clean
      // machine, then clear any held key/joystick and pending IRQ/NMI edge.
      initVarsAndRegs();          // VIC registers + derived state -> power-on defaults
      ciaReset();                 // CIA timers + IRQ/NMI mask -> default (drop stale IRQ)
      kbReset();                  // release any held keys / joystick lines
      register1 = 0x37; decodeRegister1(0x37 & 7);
      write8(0, 0x2f); write8(1, 0x37);
      PC = read16(0xfffc);
      STP = 0xFD;
      SR = SR_FIXED_BITS | SR_INT;
      irqShadowSR = SR_INT;
      nmiPrev = false;
    }

    // IRQ line = CIA1 Timer-A IRQ (KERNAL acks via $DC0D) OR the VIC raster/sprite IRQ
    // (games/carts ack via $D019) - both level-triggered and masked by the I flag. The VIC
    // source was previously never delivered, so raster-IRQ music/effects never ran (silent
    // games). Apply the 6502's 1-instruction interrupt-disable delay: recognise the IRQ using
    // the I-flag value from BEFORE the current instruction, so CLI/SEI/PLP/RTI take effect one
    // instruction later (getting this wrong corrupts the KERNAL boot: banner/$E386 hang).
    // Cycles this step costs beyond cycles[opcode]: a 7-cycle interrupt entry, a page crossed
    // by an indexed read, a taken branch.
    uint8_t extraCyc = 0;
    if (!(irqShadowSR & SR_INT) && (cia1IRQPending() || vicIRQPending())) { cpuIRQ(); extraCyc += 7; }
    irqShadowSR = SR;

    // NMI (CIA2 timers / RESTORE) is edge-triggered and NOT masked by the I flag.
    bool nmiNow = cia2NMIPending();
    if (nmiNow && !nmiPrev) { cpuNMI(); extraCyc += 7; }
    nmiPrev = nmiNow;
    // RESTORE is not in the matrix: it pulls NMI through a one-shot, so a press is one NMI.
    if (c64RestoreReq) { c64RestoreReq = false; cpuNMI(); extraCyc += 7; }

    // Virtual disk: intercept the KERNAL LOAD entry before it runs the (absent) IEC code.
    if (PC == 0xF49E && c64LoadTrap()) continue;

    // Boot-autoload (.prg/.d64): once the KERNAL reaches the BASIC main loop (READY), load
    // the saved image (which queues RUN). One-shot.
    if (::c64AutoloadPending && PC == 0xA480) {
      ::c64AutoloadPending = false;
      ::c64LoadSelected(::selectedC64FileName.c_str());
    }

    lastPC = PC;

    opcode = read8(PC++);
#if defined(BOARD_DESKTOP)
    dbgBusTouch(lastPC, DBG_HEAT_X);   // heat map: this instruction was fetched/executed at lastPC
#endif
    int instrCycles = cycles[opcode];
    if (instrCycles < 1) instrCycles = 2;

    if (!Fast1MhzSpeed)
    {
#if defined(BOARD_PICOCALC) || defined(BOARD_PICOCALC_P4) || defined(BOARD_DESKTOP)
      // Pace against real time at the PAL clock, the way the Apple II core does (src/apple2/cpu.cpp).
      // The ESP branch below charges each emulated cycle 300 host cycles, which is ~1 MHz only on a
      // 240 MHz ESP32: on the 200 MHz RP2040 it capped the 6510 at 0.67 MHz, and since each
      // instruction was measured on its own, the time spent rendering a VIC line was never won back
      // -- about 0.5 MHz in practice. Here the target is a running total, so a slow instruction or a
      // rendered line is caught up on by the ones after it (re-anchored if it falls 100 ms behind).
      paceAccQ16 += ((uint32_t)instrCycles + vicStolenCyc) * C64_US_PER_CYC_Q16;
      vicStolenCyc = 0;
      paceTargetUs += paceAccQ16 >> 16;
      paceAccQ16 &= 0xFFFF;
      int32_t paceLag = (int32_t)(C64_NOW_US() - paceBaseUs - paceTargetUs);
      if (paceLag > 100000)
        { paceBaseUs = C64_NOW_US(); paceTargetUs = 0; paceAccQ16 = 0; }
      else
        while (paceLag < 0)
          paceLag = (int32_t)(C64_NOW_US() - paceBaseUs - paceTargetUs);
#else
      int cycleCount = cycles[opcode];
      cpuCycleCount = ESP.getCycleCount();
      uint32_t expectedDiff = 300;

      diffCpuCycleCount = cpuCycleCount - lastCpuCycleCount;
      while (diffCpuCycleCount < expectedDiff * cycleCount)  
      {
        cpuCycleCount = ESP.getCycleCount();
        diffCpuCycleCount = cpuCycleCount - lastCpuCycleCount;
      }

      lastCpuCycleCount = cpuCycleCount;
#endif
    }
    opflags = flags6502[opcode];

    // Addressing modes. baseAddr is the address before indexing; an indexed read that crosses
    // a page costs one more cycle (the stores and read-modify-writes always pay it, in cycles[]).
    unsigned short baseAddr = 0;
    switch (opflags & 0x0F) {
      case AD_IMP:
      case AD_A: argument_addr = 0xFFFF; break;
      case AD_ABS:
        argument_addr = read16(PC);
        PC += 2;
        break;
      case AD_ABSX:
        baseAddr = read16(PC);
        argument_addr = baseAddr + (unsigned short)X;
        PC += 2;
        if (cycles[opcode] == 4 && ((baseAddr ^ argument_addr) & 0xFF00)) extraCyc++;
        break;
      case AD_ABSY:
        baseAddr = read16(PC);
        argument_addr = baseAddr + (unsigned short)Y;
        PC += 2;
        if (cycles[opcode] == 4 && ((baseAddr ^ argument_addr) & 0xFF00)) extraCyc++;
        break;
      case AD_IMM:
        argument_addr = PC++;
        break;
      case AD_IND:   // JMP ($xxFF) takes the high byte from $xx00: the NMOS page-wrap bug
		    argument_addr = read16(PC);
		    value16 = (argument_addr & 0xFF00) | ((argument_addr + 1) & 0x00FF);
		    argument_addr = (unsigned short)read8(argument_addr) | ((unsigned short)read8(value16) << 8);
		    PC += 2;
		    break;
      case AD_INDX:
        argument_addr = ((unsigned short)read8(PC++) + (unsigned short)X) & 0xFF;
        value16 = (argument_addr & 0xFF00) | ((argument_addr + 1) & 0x00FF);  // Page wrap
        argument_addr = (unsigned short)read8(argument_addr) | ((unsigned short)read8(value16) << 8);
        break;
      case AD_INDY:
        argument_addr = (unsigned short)read8(PC++);
        value16 = (argument_addr & 0xFF00) | ((argument_addr + 1) & 0x00FF);  // Page wrap
        baseAddr = (unsigned short)read8(argument_addr) | ((unsigned short)read8(value16) << 8);
        argument_addr = baseAddr + Y;
        if (cycles[opcode] == 5 && ((baseAddr ^ argument_addr) & 0xFF00)) extraCyc++;
        break;
      case AD_REL:
        argument_addr = (unsigned short)read8(PC++);
        argument_addr |= ((argument_addr & 0x80) ? 0xFF00 : 0);
        break;
      case AD_ZPG:
        argument_addr = (unsigned short)read8(PC++);
        break;
      case AD_ZPGX:
        argument_addr = ((unsigned short)read8(PC++) + (unsigned short)X) & 0xFF;
        break;
      case AD_ZPGY:
        argument_addr = ((unsigned short)read8(PC++) + (unsigned short)Y) & 0xFF;
        break;
    }

    // A taken branch costs one cycle more, two when it lands on another page.
#define C64_BRANCH(cond) \
    if (cond) { unsigned short t = PC + argument_addr; extraCyc += ((t ^ PC) & 0xFF00) ? 2 : 1; PC = t; }

    //opcodes
    switch (opcode) {
      //ADC
      case 0x69:
      case 0x65:
      case 0x75:
      case 0x6D:
      case 0x7D:
      case 0x79:
      case 0x61:
      case 0x71:
        doADC(read8(argument_addr));
    		break;
		    //AND
      case 0x29:
      case 0x25:
      case 0x35:
      case 0x2D:
      case 0x3D:
      case 0x39:
      case 0x21:
      case 0x31:
        result = A & read8(argument_addr);
        A = result & 0xFF;
        setflags();
        break;
      //ASL A
      case 0x0A:
        value16 = (unsigned short)A;
        result = value16 << 1;
        setflags();
        A = result & 0xFF;
        break;
      //ASL
      case 0x06:
      case 0x16:
      case 0x0E:
      case 0x1E:
        value16 = read8(argument_addr);
        result = value16 << 1;
        setflags();
        rmwWrite(argument_addr, value16, result & 0xFF);
        break;
      //BCC
      case 0x90: C64_BRANCH(!(SR & SR_CARRY)); break;
      //BCS
      case 0xB0: C64_BRANCH(SR & SR_CARRY); break;
      //BEQ
      case 0xF0: C64_BRANCH(SR & SR_ZERO); break;
      //BNE
      case 0xD0: C64_BRANCH(!(SR & SR_ZERO)); break;
      //BMI
      case 0x30: C64_BRANCH(SR & SR_NEG); break;
      //BPL
      case 0x10: C64_BRANCH(!(SR & SR_NEG)); break;
      //BVC
      case 0x50: C64_BRANCH(!(SR & SR_OVER)); break;
      //BVS
      case 0x70: C64_BRANCH(SR & SR_OVER); break;
      //BIT
      case 0x24:
      case 0x2C:
        value8 = read8(argument_addr);
        result = A & value8;
        setflags();
        SR = (SR & 0x3F) | (value8 & 0xC0);
        if (result == 0) // Zero Flag
          SR |= 0x02;
        else
          SR &= 0xfd;
        break;
      //BRK (the NMOS 6502 leaves D alone)
      case 0x00:
        PC++;
        push16(PC);
        push8(SR | SR_BRK);
        SR |= SR_INT;
        PC = read16(0xFFFE);
        break;
      //CLC
      case 0x18:
        SR &= 0xFE;
        break;
      //CLD
      case 0xD8:
        SR &= 0xF7;
        break;
      //CLI
      case 0x58:
        SR &= 0xFB;
        break;
      //CLV
      case 0xB8:
        SR &= 0xBF;
        break;
      //CMP
      case 0xC9:
      case 0xC5:
      case 0xD5:
      case 0xCD:
      case 0xDD:
      case 0xD9:
      case 0xC1:
      case 0xD1:
        value16 = ((unsigned short)read8(argument_addr)) ^ 0x00FF;
        result = (unsigned short)A + value16 + (unsigned short)1;
        setflags();
        break;
      //CPX
      case 0xE0:
      case 0xE4:
      case 0xEC:
        value16 = ((unsigned short)read8(argument_addr)) ^ 0x00FF;
        result = (unsigned short)X + value16 + (unsigned short)1;
        setflags();
        break;
      //CPY
      case 0xC0:
      case 0xC4:
      case 0xCC:
        value16 = ((unsigned short)read8(argument_addr)) ^ 0x00FF;
        result = (unsigned short)Y + value16 + (unsigned short)1;
        setflags();
        break;
      //DEC
      case 0xC6:
      case 0xD6:
      case 0xCE:
      case 0xDE:
        value16 = (unsigned short)read8(argument_addr);
        result = value16 - 1;
        setflags();
        rmwWrite(argument_addr, value16, result & 0xFF);
        break;
		//DEX
      case 0xCA:
        result = --X;
        setflags();
        break;
      //DEY
      case 0x88:
        result = --Y;
        setflags();
        break;
      //EOR
      case 0x49:
      case 0x45:
      case 0x55:
      case 0x4D:
      case 0x5D:
      case 0x59:
      case 0x41:
      case 0x51:
        value8 = read8(argument_addr);
        result = A ^ value8;
        setflags();
        A = result & 0xFF;
        break;
      //INC
      case 0xE6:
      case 0xF6:
      case 0xEE:
      case 0xFE:
        value16 = (unsigned short)read8(argument_addr);
        result = value16 + 1;
        setflags();
        rmwWrite(argument_addr, value16, result & 0xFF);
        break;
		  //INX
      case 0xE8:
        result = ++X;
        setflags();
        break;
      //INY
      case 0xC8:
        result = ++Y;
        setflags();
        break;
      //JMP
      case 0x4C:
      case 0x6C:
        PC = argument_addr;
        break;
      //JSR
      case 0x20:
        push16(PC - 1);
        PC = argument_addr;
        break;
      //LDA
      case 0xA9:
      case 0xA5:
      case 0xB5:
      case 0xAD:
      case 0xBD:
      case 0xB9:
      case 0xA1:
      case 0xB1:
        A = read8(argument_addr);
        result = A;
        setflags();
        break;
      //LDX
      case 0xA2:
      case 0xA6:
      case 0xB6:
      case 0xAE:
      case 0xBE:
        X = read8(argument_addr);
        result = X;
        setflags();
        break;
      //LDY
      case 0xA0:
      case 0xA4:
      case 0xB4:
      case 0xAC:
      case 0xBC:
        Y = read8(argument_addr);
        result = Y;
        setflags();
        break;
      //LSR A
      case 0x4A:
        value8 = A;
        result = value8 >> 1;
        result |= (value8 & 0x1) ? 0x8000 : 0;
        setflags();
        A = result & 0xFF;
        break;
		//LSR
      case 0x46:
      case 0x56:
      case 0x4E:
      case 0x5E:
        value8 = read8(argument_addr);
        result = value8 >> 1;
        result |= (value8 & 0x1) ? 0x8000 : 0;
        setflags();
        rmwWrite(argument_addr, value8, result & 0xFF);
        break;
		//ORA
      case 0x09:
      case 0x05:
      case 0x15:
      case 0x0D:
      case 0x1D:
      case 0x19:
      case 0x01:
      case 0x11:
        A |= read8(argument_addr);
        setNZ(A);
        break;
      //PHA
      case 0x48:
        push8(A);
        break;
      //PHP
      case 0x08:
        push8(SR | SR_BRK);
        break;
      //PLA
      case 0x68:
        result = pull8();
        setflags();
        A = result;
        break;
        //PLP
      case 0x28:
        SR = pull8() | SR_FIXED_BITS;
        break;
      //ROL A
      case 0x2A:
        value16 = (unsigned short)A;
        result = (value16 << 1) | (SR & SR_CARRY);
        setflags();
        A = result & 0xFF;
        break;
      //ROL
      case 0x26:
      case 0x36:
      case 0x2E:
      case 0x3E:
        value16 = (unsigned short)read8(argument_addr);
        result = (value16 << 1) | (SR & SR_CARRY);
        setflags();
        rmwWrite(argument_addr, value16, result & 0xFF);
        break;
      //ROR A
      case 0x6A:
        value16 = (unsigned short)A;
        result = (value16 >> 1) | ((SR & SR_CARRY) << 7);
        result |= (value16 & 0x1) ? 0x8000 : 0;
        setflags();
        A = result & 0xFF;
        break;
      //ROR
      case 0x66:
      case 0x76:
      case 0x6E:
      case 0x7E:
        value16 = (unsigned short)read8(argument_addr);
        result = (value16 >> 1) | ((SR & SR_CARRY) << 7);
        result |= (value16 & 0x1) ? 0x8000 : 0;
        setflags();
        rmwWrite(argument_addr, value16, result & 0xFF);
        break;
      //RTI
      case 0x40:
        SR = pull8() | SR_FIXED_BITS;
        PC = pull16();
        break;
      //RTS
      case 0x60:
        PC = pull16() + 1;
        break;
      //SBC ($EB is the undocumented copy of $E9)
      case 0xE9:
      case 0xE5:
      case 0xF5:
      case 0xED:
      case 0xFD:
      case 0xF9:
      case 0xE1:
      case 0xF1:
      case 0xEB:
        doSBC(read8(argument_addr));
        break;
		//SEC
      case 0x38:
        SR |= SR_CARRY;
        break;
      //SED
      case 0xF8:
        SR |= SR_DEC;
        break;
      //SEI
      case 0x78:
        SR |= SR_INT;
        break;
      //STA
      case 0x85:
      case 0x95:
      case 0x8D:
      case 0x9D:
      case 0x99:
      case 0x81:
      case 0x91:
        write8(argument_addr, A);
        break;
      //STX
      case 0x86:
      case 0x96:
      case 0x8E:
        write8(argument_addr, X);
        break;
      //STY
      case 0x84:
      case 0x94:
      case 0x8C:
        write8(argument_addr, Y);
        break;
      //TAX
      case 0xAA:
        X = A;
        result = A;
        setflags();
        break;
      //TAY
      case 0xA8:
        Y = A;
        result = A;
        setflags();
        break;
      //TSX
      case 0xBA:
        X = STP;
        result = STP;
        setflags();
        break;
      //TXA
      case 0x8A:
        A = X;
        result = X;
        setflags();
        break;
      //TXS
      case 0x9A:
        STP = X;
        break;
      //TYA
      case 0x98:
        A = Y;
        result = Y;
        setflags();
        break;

      // ---- Undocumented NMOS opcodes (used by many games, demos, crunchers and loaders) ----
      //SLO: ASL mem, then ORA
      case 0x03: case 0x07: case 0x0F: case 0x13: case 0x17: case 0x1B: case 0x1F:
        value8 = read8(argument_addr);
        SR = (SR & ~SR_CARRY) | (value8 >> 7);
        rmwWrite(argument_addr, value8, (uint8_t)(value8 << 1));
        A |= (uint8_t)(value8 << 1);
        setNZ(A);
        break;
      //RLA: ROL mem, then AND
      case 0x23: case 0x27: case 0x2F: case 0x33: case 0x37: case 0x3B: case 0x3F: {
        value8 = read8(argument_addr);
        uint8_t n = (uint8_t)((value8 << 1) | (SR & SR_CARRY));
        SR = (SR & ~SR_CARRY) | (value8 >> 7);
        rmwWrite(argument_addr, value8, n);
        A &= n;
        setNZ(A);
        break;
      }
      //SRE: LSR mem, then EOR
      case 0x43: case 0x47: case 0x4F: case 0x53: case 0x57: case 0x5B: case 0x5F:
        value8 = read8(argument_addr);
        SR = (SR & ~SR_CARRY) | (value8 & 1);
        rmwWrite(argument_addr, value8, value8 >> 1);
        A ^= value8 >> 1;
        setNZ(A);
        break;
      //RRA: ROR mem, then ADC
      case 0x63: case 0x67: case 0x6F: case 0x73: case 0x77: case 0x7B: case 0x7F: {
        value8 = read8(argument_addr);
        uint8_t n = (uint8_t)((value8 >> 1) | ((SR & SR_CARRY) << 7));
        SR = (SR & ~SR_CARRY) | (value8 & 1);
        rmwWrite(argument_addr, value8, n);
        doADC(n);
        break;
      }
      //SAX: store A AND X
      case 0x83: case 0x87: case 0x8F: case 0x97:
        write8(argument_addr, A & X);
        break;
      //LAX: LDA + LDX
      case 0xA3: case 0xA7: case 0xAF: case 0xB3: case 0xB7: case 0xBF:
        A = X = read8(argument_addr);
        setNZ(A);
        break;
      //LAX #imm (unstable; $EE is the usual "magic" constant)
      case 0xAB:
        A = X = (uint8_t)((A | 0xEE) & read8(argument_addr));
        setNZ(A);
        break;
      //XAA/ANE #imm (unstable)
      case 0x8B:
        A = (uint8_t)((A | 0xEE) & X & read8(argument_addr));
        setNZ(A);
        break;
      //DCP: DEC mem, then CMP
      case 0xC3: case 0xC7: case 0xCF: case 0xD3: case 0xD7: case 0xDB: case 0xDF:
        value8 = read8(argument_addr);
        rmwWrite(argument_addr, value8, (uint8_t)(value8 - 1));
        cmpReg(A, (uint8_t)(value8 - 1));
        break;
      //ISC: INC mem, then SBC
      case 0xE3: case 0xE7: case 0xEF: case 0xF3: case 0xF7: case 0xFB: case 0xFF:
        value8 = read8(argument_addr);
        rmwWrite(argument_addr, value8, (uint8_t)(value8 + 1));
        doSBC((uint8_t)(value8 + 1));
        break;
      //ANC #imm: AND, C = N
      case 0x0B: case 0x2B:
        A &= read8(argument_addr);
        setNZ(A);
        SR = (SR & ~SR_CARRY) | (A >> 7);
        break;
      //ALR/ASR #imm: AND, then LSR A
      case 0x4B:
        A &= read8(argument_addr);
        SR = (SR & ~SR_CARRY) | (A & 1);
        A >>= 1;
        setNZ(A);
        break;
      //ARR #imm: AND, then ROR A; C = bit 6, V = bit 6 XOR bit 5
      case 0x6B:
        A &= read8(argument_addr);
        A = (uint8_t)((A >> 1) | ((SR & SR_CARRY) << 7));
        setNZ(A);
        SR &= ~(SR_CARRY | SR_OVER);
        if (A & 0x40) SR |= SR_CARRY;
        if ((A ^ (A << 1)) & 0x40) SR |= SR_OVER;
        break;
      //SBX/AXS #imm: X = (A AND X) - imm, flags as CMP
      case 0xCB: {
        uint8_t ax = A & X;
        value8 = read8(argument_addr);
        cmpReg(ax, value8);
        X = (uint8_t)(ax - value8);
        break;
      }
      //LAS: A = X = SP = mem AND SP
      case 0xBB:
        A = X = STP = read8(argument_addr) & STP;
        setNZ(A);
        break;
      //TAS/SHS: SP = A AND X, store SP AND (H+1)
      case 0x9B:
        STP = A & X;
        shStore(baseAddr, argument_addr, STP);
        break;
      //SHA/AHX
      case 0x93: case 0x9F:
        shStore(baseAddr, argument_addr, A & X);
        break;
      //SHY
      case 0x9C:
        shStore(baseAddr, argument_addr, Y);
        break;
      //SHX
      case 0x9E:
        shStore(baseAddr, argument_addr, X);
        break;
      //NOP (implied)
      case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
        break;
      //NOP with an operand: it is fetched like a load (NOP $DC0D acknowledges the CIA)
      case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:
      case 0x04: case 0x44: case 0x64:
      case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4:
      case 0x0C:
      case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
        read8(argument_addr);
        break;
      //KIL/JAM: the CPU locks up until reset
      case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
      case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
        PC--;
        break;
    }
#undef C64_BRANCH
    // The pacer was charged cycles[opcode] before the instruction ran; it picks the rest up on
    // the next one, along with the VIC's stolen cycles.
    instrCycles += extraCyc;
    vicStolenCyc += extraCyc;

    // Advance CIA timers and the VIC raster by this instruction's cycle count.
    // A PAL scanline is ~63 cycles; render each line as the raster crosses it.
    ciaTick(instrCycles);
    rasterCycleAcc += instrCycles;
    perfCyc += instrCycles;
    while (rasterCycleAcc >= 63) {
      rasterCycleAcc -= 63;
#if defined(BOARD_PICOCALC) || defined(BOARD_PICOCALC_P4) || defined(BOARD_DESKTOP)
      // Cycles the VIC steals from the 6510 on this line (40 on a badline, 2 per sprite). The
      // CPU is halted for them, but the raster, the CIA timers and real time all move on, so a
      // frame holds ~19000 CPU cycles, not 19656 -- less host work, and timer-driven music keeps
      // its tempo relative to the raster. The pacer is charged for them on the next instruction.
      uint8_t st = nextRasterline();  // raster counter + raster IRQ (cheap)
      if (st) {
        ciaTick(st);
        rasterCycleAcc += st;
        perfCyc += st;
        vicStolenCyc += st;
      }
#else
      nextRasterline();               // raster counter + raster IRQ (cheap)
#endif
      if (bitmap) {                   // full VIC render only when a framebuffer exists
        uint32_t t0 = C64_NOW_US();
        static uint32_t lastLineUs = 0;
        if (t0 - lastLineUs > c64MaxLineGapUs && rasterline >= 52 && rasterline < 251)
          c64MaxLineGapUs = t0 - lastLineUs;
        lastLineUs = t0;
        drawRasterline();
        perfVicUs += C64_NOW_US() - t0;
      }
      if (rasterline == 0) perfSample();   // once a frame is plenty for a 1 s meter
    }
  }
}

} // namespace c64
