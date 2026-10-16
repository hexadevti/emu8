#include "../../emu.h"
#include "atari.h"
#include "atari_snap.h"

// 6532 RIOT: 128 bytes of RAM, an interval timer, and two 8-bit I/O ports (SWCHA = joysticks,
// SWCHB = console switches). The RAM is the 2600's only RAM; it doubles as the CPU stack
// (zero page $80-$FF == stack $180-$1FF — the famous overlap, handled by the bus decode in
// atari_cpu.cpp / atari_memory.cpp).

namespace atari {

uint8_t *riotRam = nullptr;      // 128 bytes, malloc'd in riotReset (not static BSS — DRAM is full)

// Input ports, latched by the joystick task (atariSetInput). Active-low, like real hardware.
volatile uint8_t swcha = 0xFF;   // joystick directions: P0 = high nibble, P1 = low nibble
volatile uint8_t swchb = 0x3F;   // console switches: bit0 reset, bit1 select, bit3 colour, b6/7 diff
volatile uint8_t inpt4 = 0x80;   // P0 fire on bit7 (0 = pressed)
volatile uint8_t inpt5 = 0x80;   // P1 fire on bit7

// Port output latches + data-direction registers (writes are accepted but the joystick lines are
// inputs, so reads return the latched switch state regardless).
static uint8_t outa = 0, ddra = 0, outb = 0, ddrb = 0;

// Interval timer.
static uint8_t  timerValue    = 0;
static uint16_t timerInterval = 1024;   // cycles per decrement: 1 / 8 / 64 / 1024
static int      timerCycleAcc = 0;
static uint8_t  instat        = 0;      // bit7 = timer underflowed since last INTIM read

void riotReset() {
  if (!riotRam) riotRam = (uint8_t *)malloc(128);
  outa = ddra = outb = ddrb = 0;
  // Power-on: the real 6532 timer comes up with an arbitrary count at the /1024 rate. Not 0: that
  // expires ~1000 cycles in, before a boot that polls INTIM ever sets it (H.E.R.O. waits with an
  // 8-cycle LDA INTIM/BNE loop, which phase-locks against the 256-cycle post-expiry wrap and
  // never reads 0). 0x20 runs out ~27 ms after reset, well after any init.
  timerValue = 0x20; timerInterval = 1024; timerCycleAcc = 0; instat = 0;
}

uint8_t riotRead(uint16_t addr) {
  if (addr & 0x04) {                    // timer / status block ($284 INTIM, $285 INSTAT)
    if (addr & 0x01) {                  // INSTAT
      uint8_t r = instat;
      return r;
    }
    instat &= 0x7F;                     // reading INTIM clears the underflow flag
    return timerValue;
  }
  switch (addr & 0x03) {                // I/O ports ($280-$283)
    case 0: return swcha;               // SWCHA (joystick directions)
    case 1: return ddra;                // SWACNT
    case 2: return swchb;               // SWCHB (console switches)
    case 3: return ddrb;                // SWBCNT
  }
  return 0;
}

void riotWrite(uint16_t addr, uint8_t val) {
  if (addr & 0x04) {                    // timer write block ($294-$297)
    static const uint16_t iv[4] = {1, 8, 64, 1024};
    timerInterval = iv[addr & 0x03];
    timerValue    = val;
    timerCycleAcc = 0;
    instat       &= 0x3F;               // clear underflow + edge flags
    return;
  }
  switch (addr & 0x03) {
    case 0: outa = val; break;
    case 1: ddra = val; break;
    case 2: outb = val; break;
    case 3: ddrb = val; break;
  }
  (void)outa; (void)outb;               // outputs unused (joystick/switch lines are inputs)
}

void riotTick(int cycles) {
  timerCycleAcc += cycles;
  while (timerCycleAcc >= (int)timerInterval) {
    timerCycleAcc -= (int)timerInterval;
    if (timerValue == 0) {
      timerValue    = 0xFF;
      timerInterval = 1;                // after underflow the timer runs at 1 cycle/tick
      instat       |= 0xC0;             // set the timer-underflow flag (bit7)
    } else {
      timerValue--;
    }
  }
}

// Snapshot (atari_snapshot.cpp). The input latches are restored too, though the joystick task
// rewrites them on its next poll with whatever is held now.
void riotSnap(SnapIO &io) {
  if (!riotRam) { io.fail("Atari RAM missing"); return; }
  io.bytes(riotRam, 128);
  io.val(outa); io.val(ddra); io.val(outb); io.val(ddrb);
  io.val(timerValue); io.val(timerInterval); snapI32(io, timerCycleAcc); io.val(instat);
  snapU8v(io, swcha); snapU8v(io, swchb); snapU8v(io, inpt4); snapU8v(io, inpt5);
  if (io.loading && io.ok) {
    if ((timerInterval != 1 && timerInterval != 8 && timerInterval != 64 && timerInterval != 1024) ||
        timerCycleAcc < 0 || timerCycleAcc >= (int)timerInterval) {
      timerInterval = 1024; timerCycleAcc = 0;
      io.fail("Snapshot is damaged");
    }
  }
}

} // namespace atari
