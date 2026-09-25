// emu.h must be included FIRST because it is what pulls in board.h and defines the macro.
#include "../../emu.h"
#if BOARD_HAS_ZX_CORE

// zx_machine.cpp - ZX Spectrum 48K memory map, ULA port, frame loop, beeper sampling and the
// instant-load tape trap. Arduino-free: everything board-specific lives in zx.cpp.

#include "zx.h"
#include <string.h>

namespace zx {

static uint8_t  keyRow[8];            // live keyboard, active-LOW, bits 0-4
static uint8_t  atRow[8];             // scripted typing overlay (autoTypeStart), active-LOW
static uint8_t  kempston = 0;         // active-HIGH
static uint8_t  borderCol = 7;
static uint8_t  lastFE = 0;           // last value written to port 0xFE (EAR/MIC bits)
static uint64_t frameStart = 0;       // cpu.cycles at the top of the current frame
static int      frameCount = 0;       // drives the FLASH attribute (toggles every 16 frames)

// ---- memory ---------------------------------------------------------------------------------------
uint8_t memRead8(uint16_t a) {
  return (a < 0x4000) ? rom[a] : ram[a - 0x4000];
}
void memWrite8(uint16_t a, uint8_t v) {
  if (a >= 0x4000) ram[a - 0x4000] = v;          // the ROM ignores writes
}

// ---- beeper ---------------------------------------------------------------------------------------
// The ULA's EAR/MIC outputs are integrated over each 22050 Hz sample period (441 samples per 50 Hz
// frame, ~158.5 T-states each), which is what turns 1-bit toggling at arbitrary T-states into
// something that sounds right. Samples go into a ring the core-0 audio task drains.
static const int SAMPLES_PER_FRAME = AUDIO_FS / 50;   // 441
static const int RING = BEEP_RING;
static volatile uint16_t ringHead = 0, ringTail = 0;
static uint32_t beepAcc = 0, beepLastT = 0;
static int      beepIdx = 0;                           // samples emitted this frame
static uint8_t  beepLevel = 0;

static inline uint32_t sampleBound(int i) { return (uint32_t)((uint64_t)i * T_FRAME / SAMPLES_PER_FRAME); }

static inline void ringPush(uint8_t s) {
  uint16_t h = ringHead, n = (uint16_t)((h + 1) & (RING - 1));
  if (n == ringTail) return;                          // full (FAST mode outruns the DAC): drop
  beepRing[h] = s;
  ringHead = n;
}

bool beeperPop(uint8_t* s) {
  uint16_t t = ringTail;
  if (t == ringHead) return false;
  *s = beepRing[t];
  ringTail = (uint16_t)((t + 1) & (RING - 1));
  return true;
}

int beeperDepth() { return (int)((ringHead - ringTail) & (RING - 1)); }

static void beeperAdvance(uint32_t t) {
  if (t > (uint32_t)T_FRAME) t = T_FRAME;
  while (beepIdx < SAMPLES_PER_FRAME) {
    uint32_t b = sampleBound(beepIdx + 1);
    if (t < b) break;
    beepAcc += (uint32_t)beepLevel * (b - beepLastT);
    ringPush((uint8_t)(beepAcc / (b - sampleBound(beepIdx))));
    beepAcc = 0; beepLastT = b; beepIdx++;
  }
  if (t > beepLastT) { beepAcc += (uint32_t)beepLevel * (t - beepLastT); beepLastT = t; }
}

static inline uint32_t frameT() { return (uint32_t)(cpu.cycles - frameStart); }

// ---- I/O ------------------------------------------------------------------------------------------
uint8_t ioIn(uint16_t port) {
  if ((port & 1) == 0) {                              // ULA: every even port
    uint8_t r = 0x1F;
    uint8_t hi = (uint8_t)(port >> 8);
    for (int i = 0; i < 8; i++)
      if (!(hi & (1 << i))) r &= keyRow[i] & atRow[i];
    // bit 6 = EAR in. With no tape signal an issue-3 board reads back the EAR output bit.
    return (uint8_t)(0xA0 | r | ((lastFE & 0x10) ? 0x40 : 0));
  }
  if ((port & 0xFF) == 0x1F) return kempston;         // Kempston interface
  return 0xFF;                                        // floating bus (idle)
}

void ioOut(uint16_t port, uint8_t v) {
  if ((port & 1) == 0) {
    borderCol = v & 7;
    uint8_t lvl = (uint8_t)(((v & 0x10) ? 200 : 0) + ((v & 0x08) ? 40 : 0));
    if (lvl != beepLevel) { beeperAdvance(frameT()); beepLevel = lvl; }
    lastFE = v;
  }
}

uint8_t border() { return borderCol; }
void setBorder(uint8_t c) { borderCol = c & 7; }

void keySet(int row, int bit, bool down) {
  if (row < 0 || row > 7 || bit < 0 || bit > 4) return;
  if (down) keyRow[row] &= (uint8_t)~(1 << bit); else keyRow[row] |= (uint8_t)(1 << bit);
}
void keyClearAll() { for (int i = 0; i < 8; i++) keyRow[i] = 0x1F; }
void setKempston(uint8_t v) { kempston = v & 0x1F; }

// ---- scripted typing ------------------------------------------------------------------------------
// Used to type LOAD "" after a tape is picked. Waits until the ROM has initialised (its FRAMES
// counter at 0x5C78 is running), then holds each chord for a few frames and releases it long enough
// for the ROM's 5-frame key debounce to let the same key through again.
static const uint16_t* atSeq = nullptr;
static int atLen = 0, atIdx = 0, atTimer = 0, atDelay = 0;
static const int AT_HOLD = 5, AT_GAP = 8;

void autoTypeStart(const uint16_t* chords, int n, int delayFrames) {
  atSeq = chords; atLen = n; atIdx = 0; atTimer = 0; atDelay = delayFrames;
  for (int i = 0; i < 8; i++) atRow[i] = 0x1F;
}

static void atApply(uint16_t chord, bool down) {
  for (int k = 0; k < 2; k++) {
    int c = (chord >> (8 * k)) & 0xFF;
    if (!c) continue;
    c--;
    int row = c >> 3, bit = c & 7;
    if (down) atRow[row] &= (uint8_t)~(1 << bit); else atRow[row] |= (uint8_t)(1 << bit);
  }
}

static void autoTypeFrame() {
  if (!atSeq) return;
  if (atDelay > 0) {
    // The ROM is up once its interrupt handler is bumping FRAMES by one per frame (the RAM test
    // leaves a constant fill value there, and nothing moves it before the first EI).
    static uint16_t lastFrames = 0;
    uint16_t frames = (uint16_t)(ram[0x5C78 - 0x4000] | (ram[0x5C79 - 0x4000] << 8));
    const bool ticking = frames == (uint16_t)(lastFrames + 1);
    lastFrames = frames;
    if (ticking) atDelay--;
    return;
  }
  if (atTimer == 0) atApply(atSeq[atIdx], true);
  if (atTimer == AT_HOLD) atApply(atSeq[atIdx], false);
  if (++atTimer >= AT_HOLD + AT_GAP) {
    atTimer = 0;
    if (++atIdx >= atLen) atSeq = nullptr;
  }
}

// ---- tape trap ------------------------------------------------------------------------------------
// The ROM's LD-BYTES (0x0556) is entered with A = expected flag byte, DE = length, IX = destination
// and carry set for LOAD (clear for VERIFY). We satisfy it from the next tape block in one go, then
// do what its exit path SA/LD-RET would: restore the border from BORDCR, EI, RET. A block with the
// wrong flag fails the call, which makes the ROM simply ask for the next one -- the same as the real
// tape running past it. With no blocks left the real routine runs (waiting for a signal; BREAK works).
static void tapeTrap() {
  if (tapePos >= tapeCount) return;
  const TapeBlock& b = tapeBlocks[tapePos++];
  const uint8_t* p = tapeData + b.off;
  const bool load = (cpu.F & Z80_CF) != 0;
  uint16_t de = cpu.DE(), ix = cpu.IX();
  bool ok = false;
  if (b.len >= 2 && p[0] == cpu.A) {
    uint32_t n = b.len - 2;                           // data bytes between the flag and the checksum
    uint32_t cnt = n < de ? n : de;
    ok = true;
    for (uint32_t i = 0; i < cnt; i++) {
      uint8_t v = p[1 + i];
      if (load) memWrite8(ix, v);
      else if (memRead8(ix) != v) ok = false;
      ix++; de--;
    }
    if (de) ok = false;                               // block shorter than asked for
  }
  cpu.setDE(de); cpu.setIX(ix);
  if (ok) cpu.F |= Z80_CF; else cpu.F &= (uint8_t)~Z80_CF;
  borderCol = (ram[0x5C48 - 0x4000] >> 3) & 7;        // BORDCR
  cpu.IFF1 = cpu.IFF2 = true;
  cpu.PC = (uint16_t)(memRead8(cpu.SP) | (memRead8((uint16_t)(cpu.SP + 1)) << 8));
  cpu.SP += 2;
}

// ---- reset / wiring -------------------------------------------------------------------------------
void machineWire() {
  cpu.rd  = memRead8;
  cpu.wr  = memWrite8;
  cpu.in  = ioIn;
  cpu.out = ioOut;
}

void machineReset() {
  keyClearAll();
  for (int i = 0; i < 8; i++) atRow[i] = 0x1F;
  atSeq = nullptr;
  kempston = 0;
  borderCol = 7;
  lastFE = 0;
  beepLevel = 0; beepAcc = 0; beepLastT = 0; beepIdx = 0;
  tapePos = 0;
  if (ram) memset(ram, 0, RAM_SIZE);
  cpu.reset();
  frameStart = cpu.cycles;
}

// ---- per-frame execution --------------------------------------------------------------------------
// 312 lines of 224 T-states. The ULA holds INT low for 32 T-states at the top of the frame, so an
// interrupt that is blocked for an instruction or two (EI shadow) is still taken. The frame end is
// fixed at frameStart + T_FRAME, so an instruction that overshoots one frame is paid by the next.
void runFrame() {
  const bool capture = !frameReady;                  // core 0 is not reading screen/borderLine
  const bool trap = tapeCount > 0;

  autoTypeFrame();

  while (frameT() < 32) {
    if (cpu.irq(0xFF)) break;
    cpu.step();
  }

  const int firstShown = FIRST_PAPER_LINE - BORDER_Y;   // scanline shown as output row 0
  for (int line = 0; line < LINES; line++) {
    const uint64_t target = frameStart + (uint64_t)(line + 1) * T_LINE;
    if (trap) {
      while (cpu.cycles < target) {
        if (cpu.PC == 0x0556) tapeTrap();
        cpu.step();
      }
    } else {
      while (cpu.cycles < target) cpu.step();
    }
    const int row = line - firstShown;
    if (capture && row >= 0 && row < OUT_H) borderLine[row] = borderCol;
  }

  beeperAdvance(T_FRAME);
  beepIdx = 0; beepLastT = 0; beepAcc = 0;

  frameCount++;
  if (capture) {
    memcpy(screen, ram, SCR_SIZE);
    flashPhase = (frameCount & 16) != 0;
    frameReady = true;
  }
  frameStart += T_FRAME;
}

} // namespace zx

#endif  // BOARD_HAS_ZX_CORE
