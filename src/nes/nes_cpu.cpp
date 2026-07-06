// Ricoh 2A03 CPU core — the same proven µ6502 (Damian Peckett base) used by the Apple II
// and C64 cores, copied into namespace nes with nes::read8/write8 as the bus. The 2A03 is an
// NMOS 6502 with decimal mode disabled; the decimal ADC/SBC paths below are gated on the D
// flag, which NES games CLD at boot and never set, so they are effectively unused.
//
// The NES cpuLoop steps the PPU 3 dots per CPU cycle, services the VBlank NMI edge, and adds
// the OAM-DMA stall ($4014) to the cycle budget. No IRQ source exists in phase 1 (mapper 0
// has none and the APU is stubbed), so the IRQ line is never asserted.

#include "../../emu.h"
#include "nes.h"
#if defined(BOARD_DESKTOP)
#include "../desktop/debug_bridge.h"   // dbgBusTouch + breakpoint/step hooks (compiled out on device)
#endif

namespace nes {

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
#define AD_IABX 0x0E
#define AD_IZPG 0x0F

// SR Flag Modes
#define FL_NONE 0x00
#define FL_N    0x80
#define FL_V    0x40
#define FL_Z    0x20
#define FL_C    0x10
#define FL_ZN   0xA0
#define FL_NVC  0xD0
#define FL_NVZ  0xE0
#define FL_ZNC  0xB0
#define FL_ZC   0x30
#define FL_ALL  0xF0

#define UNDF 0x00

#define SR_CARRY      0x01
#define SR_ZERO       0x02
#define SR_INT        0x04
#define SR_DEC        0x08
#define SR_BRK        0x10
#define SR_FIXED_BITS 0x20
#define SR_OVER       0x40
#define SR_NEG        0x80

#define STP_BASE 0x100

// high nibble = SR flags affected, low nibble = address mode (NMOS 6502 official set)
static const DRAM_ATTR unsigned char flags6502[] = {
  AD_IMP, AD_INDX, UNDF, UNDF, UNDF, FL_ZN | AD_ZPG, FL_ZNC | AD_ZPG, UNDF, AD_IMP, FL_ZN | AD_IMM, FL_ZNC | AD_A, UNDF, UNDF, FL_ZN | AD_ABS, FL_ZNC | AD_ABS, UNDF,
  AD_REL, FL_ZN | AD_INDY, UNDF, UNDF, UNDF, FL_ZN | AD_ZPGX, FL_ZNC | AD_ZPGX, UNDF, AD_IMP, FL_ZN | AD_ABSY, UNDF, UNDF, UNDF, FL_ZN | AD_ABSX, FL_ZNC | AD_ABSX, UNDF,
  AD_ABS, FL_ZN | AD_INDX, UNDF, UNDF, FL_Z | AD_ZPG, FL_ZN | AD_ZPG, FL_ZNC | AD_ZPG, UNDF, AD_IMP, FL_ZN | AD_IMM, FL_ZNC | AD_A, UNDF, FL_Z | AD_ABS, FL_ZN | AD_ABS, FL_ZNC | AD_ABS, UNDF,
  AD_REL, FL_ZN | AD_INDY, UNDF, UNDF, UNDF, FL_ZN | AD_ZPGX, FL_ZNC | AD_ZPGX, UNDF, AD_IMP, FL_ZN | AD_ABSY, UNDF, UNDF, UNDF, FL_ZN | AD_ABSX, FL_ZNC | AD_ABSX, UNDF,
  AD_IMP, FL_ZN | AD_INDX, UNDF, UNDF, UNDF, FL_ZN | AD_ZPG, FL_ZNC | AD_ZPG, UNDF, AD_IMP, FL_ZN | AD_IMM, FL_ZNC | AD_A, UNDF, AD_ABS, FL_ZN | AD_ABS, FL_ZNC | AD_ABS, UNDF,
  AD_REL, FL_ZN | AD_INDY, UNDF, UNDF, UNDF, FL_ZN | AD_ZPGX, FL_ZNC | AD_ZPGX, UNDF, AD_IMP, FL_ZN | AD_ABSY, UNDF, UNDF, UNDF, FL_ZN | AD_ABSX, FL_ZNC | AD_ABSX, UNDF,
  AD_IMP, FL_ALL | AD_INDX, UNDF, UNDF, UNDF, FL_ALL | AD_ZPG, FL_ZNC | AD_ZPG, UNDF, FL_ZN | AD_IMP, FL_ALL | AD_IMM, FL_ZNC | AD_A, UNDF, AD_IND, FL_ALL | AD_ABS, FL_ZNC | AD_ABS, UNDF,
  AD_REL, FL_ALL | AD_INDY, UNDF, UNDF, UNDF, FL_ALL | AD_ZPGX, FL_ZNC | AD_ZPGX, UNDF, AD_IMP, FL_ALL | AD_ABSY, UNDF, UNDF, UNDF, FL_ALL | AD_ABSX, FL_ZNC | AD_ABSX, UNDF,
  UNDF, AD_INDX, UNDF, UNDF, AD_ZPG, AD_ZPG, AD_ZPG, UNDF, FL_ZN | AD_IMP, UNDF, FL_ZN | AD_IMP, UNDF, AD_ABS, AD_ABS, AD_ABS, UNDF,
  AD_REL, AD_INDY, UNDF, UNDF, AD_ZPGX, AD_ZPGX, AD_ZPGY, UNDF, FL_ZN | AD_IMP, AD_ABSY, AD_IMP, UNDF, UNDF, AD_ABSX, UNDF, UNDF,
  FL_ZN | AD_IMM, FL_ZN | AD_INDX, FL_ZN | AD_IMM, UNDF, FL_ZN | AD_ZPG, FL_ZN | AD_ZPG, FL_ZN | AD_ZPG, UNDF, FL_ZN | AD_IMP, FL_ZN | AD_IMM, FL_ZN | AD_IMP, UNDF, FL_ZN | AD_ABS, FL_ZN | AD_ABS, FL_ZN | AD_ABS, UNDF,
  AD_REL, FL_ZN | AD_INDY, UNDF, UNDF, FL_ZN | AD_ZPGX, FL_ZN | AD_ZPGX, FL_ZN | AD_ZPGY, UNDF, AD_IMP, FL_ZN | AD_ABSY, FL_ZN | AD_IMP, UNDF, FL_ZN | AD_ABSX, FL_ZN | AD_ABSX, FL_ZN | AD_ABSY, UNDF,
  FL_ZNC | AD_IMM, FL_ZNC | AD_INDX, UNDF, UNDF, FL_ZNC | AD_ZPG, FL_ZNC | AD_ZPG, FL_ZN | AD_ZPG, UNDF, FL_ZN | AD_IMP, FL_ZNC | AD_IMM, FL_ZN | AD_IMP, UNDF, FL_ZNC | AD_ABS, FL_ZNC | AD_ABS, FL_ZN | AD_ABS, UNDF,
  AD_REL, FL_ZNC | AD_INDY, UNDF, UNDF, UNDF, FL_ZNC | AD_ZPGX, FL_ZN | AD_ZPGX, UNDF, AD_IMP, FL_ZNC | AD_ABSY, UNDF, UNDF, UNDF, FL_ZNC | AD_ABSX, FL_ZN | AD_ABSX, UNDF,
  FL_ZNC | AD_IMM, FL_ALL | AD_INDX, UNDF, UNDF, FL_ZNC | AD_ZPG, FL_ALL | AD_ZPG, FL_ZN | AD_ZPG, UNDF, FL_ZN | AD_IMP, FL_ALL | AD_IMM, AD_IMP, UNDF, FL_ZNC | AD_ABS, FL_ALL | AD_ABS, FL_ZN | AD_ABS, UNDF,
  AD_REL, FL_ALL | AD_INDY, UNDF, UNDF, UNDF, FL_ALL | AD_ZPGX, FL_ZN | AD_ZPGX, UNDF, AD_IMP, FL_ALL | AD_ABSY, UNDF, UNDF, UNDF, FL_ALL | AD_ABSX, FL_ZN | AD_ABSX, UNDF
};

// uint8_t (not int): the cycle counts are all 0-7, so a byte table is 4x smaller and stays in
// fast DRAM — freeing the static DRAM the 4th (Atari) core needs, with zero performance change.
static const DRAM_ATTR uint8_t cycles[] = { 7, 6, 1, 0, 0, 3, 5, 0, 3, 2, 2, 0, 0, 4, 6, 0,
                       2, 5, 1, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
                       6, 6, 1, 0, 3, 3, 5, 0, 4, 2, 2, 0, 4, 4, 6, 0,
                       2, 5, 1, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
                       6, 6, 1, 0, 0, 3, 5, 0, 3, 2, 2, 0, 3, 4, 6, 0,
                       2, 5, 1, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
                       6, 6, 1, 0, 0, 3, 5, 0, 4, 2, 2, 0, 5, 4, 6, 0,
                       2, 5, 1, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
                       0, 6, 0, 0, 3, 3, 3, 0, 2, 0, 2, 0, 4, 4, 4, 0,
                       2, 6, 1, 0, 4, 4, 4, 0, 2, 5, 2, 0, 0, 5, 0, 0,
                       2, 6, 2, 0, 3, 3, 3, 0, 2, 2, 2, 0, 4, 4, 4, 0,
                       2, 5, 1, 0, 4, 4, 4, 0, 2, 4, 2, 0, 4, 4, 4, 0,
                       2, 6, 0, 0, 3, 3, 5, 0, 2, 2, 2, 0, 4, 4, 6, 0,
                       2, 5, 1, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
                       2, 6, 0, 0, 3, 3, 5, 0, 2, 2, 2, 0, 4, 4, 6, 0,
                       2, 5, 1, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0 };

// CPU registers. PC/A/X/Y/STP/SR are exposed (non-static; declared in nes.h) so the desktop debug
// bridge can read/write them — inside namespace nes they're nes::PC etc., distinct from the Apple
// globals. The remaining instruction-decode scratch stays file-local.
unsigned short PC;
unsigned char STP = 0xFD, A = 0, X = 0, Y = 0, SR = SR_FIXED_BITS | SR_INT;
static unsigned short lastPC, argument_addr;
static unsigned char opcode, opflags, value8;
static unsigned short value16, value16_2, result;

// Fast inline bus access for the CPU hot path: the common RAM ($0000-$1FFF, mirrored) and PRG
// ROM ($8000-$FFFF) regions resolve inline, skipping the read8/write8 call + region-branch chain
// that dominated per-instruction cost. PPU/APU/controller/mapper I/O still routes through the full
// bus. Safe because cpuLoop refuses to run without a loaded ROM, so prgMap[] is always non-null.
static inline __attribute__((always_inline)) uint8_t cpuRead(uint16_t a) {
#if defined(BOARD_DESKTOP)
  dbgBusTouch(a, DBG_HEAT_R);   // heat map + read watchpoints (one branch when off; no-op on device)
#endif
  if (a < 0x2000)  return cpuRam[a & 0x07FF];
  if (a >= 0x8000) return prgMap[(a >> 13) & 3][a & 0x1FFF];
  return read8(a);
}
static inline __attribute__((always_inline)) uint16_t cpuRead16(uint16_t a) {
  return (uint16_t)cpuRead(a) | ((uint16_t)cpuRead((uint16_t)(a + 1)) << 8);
}
static inline __attribute__((always_inline)) void cpuWrite(uint16_t a, uint8_t v) {
#if defined(BOARD_DESKTOP)
  dbgBusTouch(a, DBG_HEAT_W);   // heat map + write watchpoints (one branch when off; no-op on device)
#endif
  if (a < 0x2000) { cpuRam[a & 0x07FF] = v; return; }
  write8(a, v);
}
// Stack ops (page $0100, RAM) — always_inline, defined ahead of all callers (cpuIRQ/NMI/loop)
// so JSR/RTS/PHA/PLA/interrupts avoid a function call. File-local: name lookup binds these (not
// the Apple core's global push16/push8 from proto.h) because the definition precedes every use.
static inline __attribute__((always_inline)) void push16(unsigned short pushval) {
  cpuWrite(STP_BASE + (STP--), (pushval >> 8) & 0xFF);
  cpuWrite(STP_BASE + (STP--), pushval & 0xFF);
}
static inline __attribute__((always_inline)) void push8(unsigned char pushval) { cpuWrite(STP_BASE + (STP--), pushval); }
static inline __attribute__((always_inline)) unsigned short pull16() {
  STP++;
  value16 = cpuRead(STP_BASE + (STP));
  STP++;
  value16 = value16 | ((unsigned short)cpuRead(STP_BASE + (STP)) << 8);
  return value16;
}
static inline __attribute__((always_inline)) unsigned char pull8() { return cpuRead(STP_BASE + (++STP)); }

void cpuReset() {
  PC = cpuRead16(0xFFFC);
  STP = 0xFD;
  SR = SR_FIXED_BITS | SR_INT;
}

void cpuIRQ() {
  push16(PC);
  push8((SR & ~SR_BRK) | SR_FIXED_BITS);
  SR |= SR_INT;
  PC = cpuRead16(0xFFFE);
}

void cpuNMI() {
  push16(PC);
  push8((SR & ~SR_BRK) | SR_FIXED_BITS);
  SR |= SR_INT;
  PC = cpuRead16(0xFFFA);
}

static inline __attribute__((always_inline)) void setflags() {
  switch (opflags & 0xF0) {
    case  FL_ZN: SR &= 0x7D; break;
    case FL_ZNC: SR &= 0x7C; break;
    case  FL_ZC: SR &= 0xFC; break;
    case FL_ALL: SR &= 0x3C; break;
    case   FL_Z: SR &= 0xFD; break;
  }
  if (opflags & FL_N) SR |= (result & 0x80);
  if (opflags & FL_Z) SR |= (((result & 0xFF) == 0) ? 0x02 : 0);
  if (opflags & FL_C) SR |= ((result & 0xFF00) ? 0x01 : 0);
  if (opflags & FL_V) SR |= ((result ^ ((unsigned short)A)) & (result ^ value16) & 0x0080) >> 1;
}

void cpuLoop() {
  if (!prgMap[0]) { printLog("NES: no ROM loaded; CPU idle"); while (running) delay(100); return; }
  cpuReset();
  lastPC = PC;

  // Frame pacing + FPS/MHz readout, gated on the frame counter changing (≤speed checks/sec) so it
  // adds no per-instruction cost. NORMAL mode (default) paces to the real ~60.0988 fps; FAST runs
  // uncapped. nesMeasuredMhz is derived from fps x the NES's fixed CPU cycles/frame.
  const uint32_t NES_FRAME_US = 16639;        // 1e6 / 60.0988 fps (NTSC)
  uint32_t fpsLastMs = millis(), fpsLastFrames = nesFrameCount, fpsSeenFrame = nesFrameCount;
  uint32_t nextFrameUs = micros();

  while (running) {
#if defined(BOARD_DESKTOP)
    // Desktop debugger: break at the upcoming PC (breakpoint / run-to-cursor / step-over / step-out).
    if (dbgBpShouldBreak(PC)) paused = true;                                                      // breakpoint
    if (g_dbgRunToPC >= 0 && PC == (uint16_t)g_dbgRunToPC) { paused = true; g_dbgRunToPC = -1; }  // run-to / step-over
    if (g_dbgRunUntilSP >= 0 && STP > (uint8_t)g_dbgRunUntilSP) { paused = true; g_dbgRunUntilSP = -1; } // step-out
#endif
    while (paused) {
#if defined(BOARD_DESKTOP)
      if (dbgStepReq > 0) { dbgStepReq--; break; }   // debugger single-step: run exactly one instruction
#endif
      delay(100); fpsLastMs = millis(); fpsLastFrames = nesFrameCount; nextFrameUs = micros();
    }
#if defined(BOARD_DESKTOP)
    g_dbgBreakArmed = true;                           // re-arm after passing the breakpoint check once
#endif

    // A new ROM was loaded from the settings window -> reset CPU+PPU to start it cleanly.
    if (nesResetReq) { nesResetReq = false; ppuReset(); cpuReset(); lastPC = PC; nextFrameUs = micros(); }

    if (nesFrameCount != fpsSeenFrame) {        // a frame just completed
      fpsSeenFrame = nesFrameCount;

      // Pace to ~60 fps in NORMAL mode (FAST = uncapped). Sleep the bulk, spin the last ~1.5ms for
      // a steady cadence; resync if we ever fall far behind.
      if (!nesFast) {
        nextFrameUs += NES_FRAME_US;
        int32_t wait = (int32_t)(nextFrameUs - micros());
        if (wait > 2000) vTaskDelay(pdMS_TO_TICKS((uint32_t)(wait - 1500) / 1000));
        while ((int32_t)(nextFrameUs - micros()) > 0) { /* spin to the frame boundary */ }
        if ((int32_t)(micros() - nextFrameUs) > (int32_t)(4 * NES_FRAME_US)) nextFrameUs = micros();
      } else {
        nextFrameUs = micros();
      }

      uint32_t nowMs = millis();
      if (nowMs - fpsLastMs >= 1000) {
        uint32_t frames = nesFrameCount - fpsLastFrames;
        float secs = (nowMs - fpsLastMs) / 1000.0f;
        if (secs > 0) nesMeasuredMhz = (frames / secs) * (29780.5f / 1.0e6f);   // fps x CPU cycles/frame
        sprintf(buf, "NES fps=%u %.2fMHz heap=%u", (unsigned)frames, nesMeasuredMhz,
                (unsigned)ESP.getFreeHeap());
        printLog(buf);
        fpsLastMs = nowMs; fpsLastFrames = nesFrameCount;
      }
    }

    // VBlank NMI (edge-triggered, set by the PPU).
    if (nmiPending) { nmiPending = false; cpuNMI(); }
    // Mapper IRQ (MMC3 scanline counter), level-held until the handler acks ($E000); maskable.
    if (irqLine && !(SR & SR_INT)) cpuIRQ();

    lastPC = PC;
    opcode = cpuRead(PC++);
#if defined(BOARD_DESKTOP)
    dbgBusTouch(lastPC, DBG_HEAT_X);   // heat map: this instruction was fetched/executed at lastPC
#endif
    int instrCycles = cycles[opcode];
    if (instrCycles < 1) instrCycles = 2;
    opflags = flags6502[opcode];

    // Addressing modes
    switch (opflags & 0x0F) {
      case AD_IMP:
      case AD_A: argument_addr = 0xFFFF; break;
      case AD_ABS:
        argument_addr = cpuRead16(PC); PC += 2; break;
      case AD_ABSX:
        argument_addr = cpuRead16(PC) + (unsigned short)X; PC += 2; break;
      case AD_IABX:
        argument_addr = cpuRead16(PC) + (unsigned short)X;
        value16 = (argument_addr + 1 & 0xFF00) | ((argument_addr + 1) & 0x00FF);
        argument_addr = (unsigned short)cpuRead(argument_addr) | ((unsigned short)cpuRead(value16) << 8);
        PC += 2; break;
      case AD_ABSY:
        argument_addr = cpuRead16(PC) + (unsigned short)Y; PC += 2; break;
      case AD_IMM:
        argument_addr = PC++; break;
      case AD_IND:
        argument_addr = cpuRead16(PC);
        value16 = (argument_addr + 1 & 0xFF00) | ((argument_addr + 1) & 0x00FF);
        argument_addr = (unsigned short)cpuRead(argument_addr) | ((unsigned short)cpuRead(value16) << 8);
        PC += 2; break;
      case AD_INDX:
        argument_addr = ((unsigned short)cpuRead(PC++) + (unsigned short)X) & 0xFF;
        value16 = (argument_addr & 0xFF00) | ((argument_addr + 1) & 0x00FF);
        argument_addr = (unsigned short)cpuRead(argument_addr) | ((unsigned short)cpuRead(value16) << 8);
        break;
      case AD_INDY:
        argument_addr = (unsigned short)cpuRead(PC++);
        value16 = (argument_addr & 0xFF00) | ((argument_addr + 1) & 0x00FF);
        argument_addr = (unsigned short)cpuRead(argument_addr) | ((unsigned short)cpuRead(value16) << 8);
        argument_addr += Y;
        break;
      case AD_REL:
        argument_addr = (unsigned short)cpuRead(PC++);
        argument_addr |= ((argument_addr & 0x80) ? 0xFF00 : 0);
        break;
      case AD_ZPG:
        argument_addr = (unsigned short)cpuRead(PC++); break;
      case AD_IZPG:
        argument_addr = (unsigned short)cpuRead(PC++);
        value16 = (argument_addr + 1 & 0xFF00) | ((argument_addr + 1) & 0x00FF);
        argument_addr = (unsigned short)cpuRead(argument_addr) | ((unsigned short)cpuRead(value16) << 8);
        break;
      case AD_ZPGX:
        argument_addr = ((unsigned short)cpuRead(PC++) + (unsigned short)X) & 0xFF; break;
      case AD_ZPGY:
        argument_addr = ((unsigned short)cpuRead(PC++) + (unsigned short)Y) & 0xFF; break;
    }

    switch (opcode) {
      // ADC
      case 0x69: case 0x65: case 0x75: case 0x6D: case 0x7D: case 0x79: case 0x61: case 0x71: case 0x72:
        value16 = (unsigned short)cpuRead(argument_addr);
        if (SR & SR_DEC) {
          result = (unsigned short)(A & 0x0F) + (unsigned short)(value16 & 0x0f) + (SR & 0x01 > 0);
          if (result > 0x09) result += 0x06;
          if (result <= 0x0F)
            result = (unsigned short)(result & 0x0F) + (unsigned short)(A & 0xF0) + (unsigned short)(value16 & 0xF0);
          else
            result = (unsigned short)(result & 0x0F) + (unsigned short)(A & 0xF0) + (unsigned short)(value16 & 0xF0) + 0x10;
          if (result == 0) SR |= 0x02; else SR &= 0xfd;
          if (((A ^ result) & 0x80) > 0) SR |= 0x40; else SR &= 0xbf;
          if ((result & 0x1F0) > 0x90) result += 0x60;
          if ((result & 0xFF0) > 0xF0) SR |= 0x01; else SR &= 0xfe;
        } else {
          result = (unsigned short)A + value16 + (unsigned short)(SR & SR_CARRY);
          if (!(((A ^ value16) & 0x80) > 0)) SR |= 0x40; else SR &= 0xbf;
          if (result >= 0x100) { SR |= 0x01; if (result >= 0x180) SR &= 0xbf; }
          else { SR &= 0xfe; if (result < 0x80) SR &= 0xbf; }
        }
        setflags();
        A = result & 0xFF;
        if ((A & 0x80) == 0x80) SR |= 0x80; else SR &= 0x7f;
        break;
      // AND
      case 0x29: case 0x25: case 0x35: case 0x2D: case 0x3D: case 0x39: case 0x21: case 0x31: case 0x32:
        result = A & cpuRead(argument_addr); A = result & 0xFF; setflags(); break;
      // ASL A
      case 0x0A:
        value16 = (unsigned short)A; result = value16 << 1; setflags(); A = result & 0xFF; break;
      // ASL
      case 0x06: case 0x16: case 0x0E: case 0x1E:
        value16 = cpuRead(argument_addr); result = value16 << 1; setflags(); cpuWrite(argument_addr, result & 0xFF); break;
      // BCC
      case 0x90: if (!(SR & SR_CARRY)) PC += argument_addr; break;
      // BCS
      case 0xB0: if ((SR & SR_CARRY)) PC += argument_addr; break;
      // BEQ
      case 0xF0: if ((SR & SR_ZERO)) PC += argument_addr; break;
      // BNE
      case 0xD0: if (!(SR & SR_ZERO)) PC += argument_addr; break;
      // BIT
      case 0x24: case 0x2C: case 0x34: case 0x3C:
        value8 = cpuRead(argument_addr); result = A & value8; setflags();
        SR = (SR & 0x3F) | (value8 & 0xC0);
        if (result == 0) SR |= 0x02; else SR &= 0xfd; break;
      // BIT #imm (65C02; harmless)
      case 0x89:
        value8 = cpuRead(argument_addr); result = A & value8;
        if (result == 0) SR |= 0x02; else SR &= 0xfd; break;
      // BMI
      case 0x30: if ((SR & SR_NEG)) PC += argument_addr; break;
      // BPL
      case 0x10: if (!(SR & SR_NEG)) PC += argument_addr; break;
      // BRK
      case 0x00:
        PC++; push16(PC); push8(SR | SR_BRK); SR |= SR_INT; PC = cpuRead16(0xFFFE); SR &= 0xF7; break;
      // BRA (65C02)
      case 0x80: PC += argument_addr; break;
      // BVC
      case 0x50: if (!(SR & SR_OVER)) PC += argument_addr; break;
      // BVS
      case 0x70: if (SR & SR_OVER) PC += argument_addr; break;
      // CLC
      case 0x18: SR &= 0xFE; break;
      // CLD
      case 0xD8: SR &= 0xF7; break;
      // CLI
      case 0x58: SR &= 0xFB; break;
      // CLV
      case 0xB8: SR &= 0xBF; break;
      // CMP
      case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9: case 0xC1: case 0xD1: case 0xD2:
        value16 = ((unsigned short)cpuRead(argument_addr)) ^ 0x00FF;
        result = (unsigned short)A + value16 + (unsigned short)1; setflags(); break;
      // CPX
      case 0xE0: case 0xE4: case 0xEC:
        value16 = ((unsigned short)cpuRead(argument_addr)) ^ 0x00FF;
        result = (unsigned short)X + value16 + (unsigned short)1; setflags(); break;
      // CPY
      case 0xC0: case 0xC4: case 0xCC:
        value16 = ((unsigned short)cpuRead(argument_addr)) ^ 0x00FF;
        result = (unsigned short)Y + value16 + (unsigned short)1; setflags(); break;
      // DEC
      case 0xC6: case 0xD6: case 0xCE: case 0xDE:
        value16 = (unsigned short)cpuRead(argument_addr); result = value16 - 1; setflags(); cpuWrite(argument_addr, result & 0xFF); break;
      // DEC A (65C02)
      case 0x3A: result = A - 1; setflags(); A = result; break;
      // DEX
      case 0xCA: result = --X; setflags(); break;
      // DEY
      case 0x88: result = --Y; setflags(); break;
      // EOR
      case 0x49: case 0x45: case 0x55: case 0x4D: case 0x5D: case 0x59: case 0x41: case 0x51: case 0x52:
        value8 = cpuRead(argument_addr); result = A ^ value8; setflags(); A = result & 0xFF; break;
      // INC
      case 0xE6: case 0xF6: case 0xEE: case 0xFE:
        value16 = (unsigned short)cpuRead(argument_addr); result = value16 + 1; setflags(); cpuWrite(argument_addr, result & 0xFF); break;
      // INC A (65C02)
      case 0x1A: result = A + 1; setflags(); A = result; break;
      // INX
      case 0xE8: result = ++X; setflags(); break;
      // INY
      case 0xC8: result = ++Y; setflags(); break;
      // JMP
      case 0x4C: case 0x6C: case 0x7C: PC = argument_addr; break;
      // JSR
      case 0x20: push16(PC - 1); PC = argument_addr; break;
      // LDA
      case 0xA9: case 0xA5: case 0xB5: case 0xAD: case 0xBD: case 0xB9: case 0xA1: case 0xB1: case 0xB2:
        A = cpuRead(argument_addr); result = A; setflags(); break;
      // LDX
      case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE:
        X = cpuRead(argument_addr); result = X; setflags(); break;
      // LDY
      case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC:
        Y = cpuRead(argument_addr); result = Y; setflags(); break;
      // LSR A
      case 0x4A:
        value8 = A; result = value8 >> 1; result |= (value8 & 0x1) ? 0x8000 : 0; setflags(); A = result & 0xFF; break;
      // LSR
      case 0x46: case 0x56: case 0x4E: case 0x5E:
        value8 = cpuRead(argument_addr); result = value8 >> 1; result |= (value8 & 0x1) ? 0x8000 : 0; setflags(); cpuWrite(argument_addr, result & 0xFF); break;
      // ORA
      case 0x09: case 0x05: case 0x15: case 0x0D: case 0x1D: case 0x19: case 0x01: case 0x11: case 0x12:
        value8 = cpuRead(argument_addr); result = value8 | A; setflags();
        if ((result & 0x80) == 0x80) SR |= 0x80; else SR &= 0x7f;
        if ((result & 0xFF) == 0) SR |= 0x02; else SR &= 0xfd; A = result; break;
      // PHA
      case 0x48: push8(A); break;
      // PHX (65C02)
      case 0xDA: push8(X); break;
      // PHY (65C02)
      case 0x5A: push8(Y); break;
      // PHP
      case 0x08: push8(SR | SR_BRK); break;
      // PLA
      case 0x68: result = pull8(); setflags(); A = result; break;
      // PLX (65C02)
      case 0xFA: result = pull8(); setflags(); X = result; break;
      // PLY (65C02)
      case 0x7A: result = pull8(); setflags(); Y = result; break;
      // PLP
      case 0x28: SR = pull8() | SR_FIXED_BITS; break;
      // ROL A
      case 0x2A:
        value16 = (unsigned short)A; result = (value16 << 1) | (SR & SR_CARRY); setflags(); A = result & 0xFF; break;
      // ROL
      case 0x26: case 0x36: case 0x2E: case 0x3E:
        value16 = (unsigned short)cpuRead(argument_addr); result = (value16 << 1) | (SR & SR_CARRY); setflags(); cpuWrite(argument_addr, result & 0xFF); break;
      // ROR A
      case 0x6A:
        value16 = (unsigned short)A; result = (value16 >> 1) | ((SR & SR_CARRY) << 7); result |= (value16 & 0x1) ? 0x8000 : 0; setflags(); A = result & 0xFF; break;
      // ROR
      case 0x66: case 0x76: case 0x6E: case 0x7E:
        value16 = (unsigned short)cpuRead(argument_addr); result = (value16 >> 1) | ((SR & SR_CARRY) << 7); result |= (value16 & 0x1) ? 0x8000 : 0; setflags(); cpuWrite(argument_addr, result & 0xFF); break;
      // RTI
      case 0x40: SR = pull8(); PC = pull16(); break;
      // RTS
      case 0x60: PC = pull16() + 1; break;
      // SBC
      case 0xE9: case 0xE5: case 0xF5: case 0xED: case 0xFD: case 0xF9: case 0xE1: case 0xF1: case 0xF2:
        if (SR & SR_DEC) {
          value16 = (unsigned short)cpuRead(argument_addr);
          if (!(((A ^ value16) & 0x80) > 0)) SR |= 0x40; else SR &= 0xbf;
          unsigned short value2 = (unsigned short)(A - value16 - (!(SR & 0x01 > 0)));
          result = (unsigned short)((unsigned short)(A & 0x0F) - (unsigned short)(value16 & 0x0F) - (unsigned short)(!(SR & 0x01 > 0)));
          if ((result & 0x10) > 0) result = ((result - 0x06) & 0x0F) | ((A & 0xF0) - (value16 & 0xF0) - 0x10);
          else result = (result & 0x0F) | ((A & 0xF0) - (value16 & 0xF0));
          if ((result & 0x100) > 0) result -= 0x60;
          if ((unsigned short)value2 < (unsigned short)0x0100) SR |= 0x01; else SR &= 0xfe;
        } else {
          value16 = ((unsigned short)cpuRead(argument_addr)) ^ 0x00FF;
          result = (unsigned short)A + value16 + (unsigned short)(SR & SR_CARRY); setflags();
        }
        A = result & 0xFF;
        if ((A & 0x80) > 0) SR |= 0x80; else SR &= 0x7f;
        if (!((A & 0xFF) > 0)) SR |= 0x02; else SR &= 0xfd; break;
      // SEC
      case 0x38: SR |= SR_CARRY; break;
      // SED
      case 0xF8: SR |= SR_DEC; break;
      // SEI
      case 0x78: SR |= SR_INT; break;
      // STA
      case 0x85: case 0x95: case 0x8D: case 0x9D: case 0x99: case 0x81: case 0x91: case 0x92:
        cpuWrite(argument_addr, A); break;
      // STX
      case 0x86: case 0x96: case 0x8E: cpuWrite(argument_addr, X); break;
      // STY
      case 0x84: case 0x94: case 0x8C: cpuWrite(argument_addr, Y); break;
      // STZ (65C02)
      case 0x64: case 0x74: case 0x9C: case 0x9E: cpuWrite(argument_addr, 0); break;
      // TAX
      case 0xAA: X = A; result = A; setflags(); break;
      // TAY
      case 0xA8: Y = A; result = A; setflags(); break;
      // TSX
      case 0xBA: X = STP; result = STP; setflags(); break;
      // TXA
      case 0x8A: A = X; result = X; setflags(); break;
      // TXS
      case 0x9A: STP = X; break;
      // TYA
      case 0x98: A = Y; result = Y; setflags(); break;
      // TRB (65C02)
      case 0x14: case 0x1C:
        value8 = cpuRead(argument_addr); result = value8 & A; value8 = (char)(value8 & ~A); cpuWrite(argument_addr, value8); setflags(); break;
      // TSB (65C02)
      case 0x04: case 0x0C:
        value8 = cpuRead(argument_addr); result = value8 & A; value8 = (char)(value8 | A); cpuWrite(argument_addr, value8); setflags(); break;
      // NOPs / unofficial no-ops
      case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xc2: case 0xe2:
      case 0x44: case 0x54: case 0xd4: case 0xf4:
        PC++; break;
      case 0x5c: case 0xdc: case 0xfc: PC += 2; break;
      case 0xea: break;  // NOP
      default: break;    // unhandled / unofficial: treat as NOP
    }

    if (dmaStallCycles) { instrCycles += dmaStallCycles; dmaStallCycles = 0; }
    // Inlined ppuStep: the common per-instruction work is just an accumulate + compare;
    // endScanline (the heavy per-scanline render) is still a call but fires only ~1/113 cycles.
    dotAcc += instrCycles * 3;
    while (dotAcc >= 341) { dotAcc -= 341; endScanline(); }
  }
}

} // namespace nes
