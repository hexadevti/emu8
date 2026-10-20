#include "../../emu.h"
#include "c64.h"
#include "../shared/snapshot.h"

// CIA1/CIA2 (6526). Both chips share the timer / interrupt / TOD model below:
//   - Timer A and Timer B, continuous or one-shot (CRx bit 3), force-load (bit 4); Timer B counts
//     cycles or Timer A underflows (CRB bits 6-5). The CNT pin is idle, so CNT-counting stops.
//   - ICR: a flag raises the IRQ (CIA1) / NMI (CIA2) when its mask bit is set -- also when the
//     mask is enabled while the flag is already pending. Reading $xC0D returns and clears it.
//   - TOD clock (BCD tenths/seconds/minutes/hours + PM) on the 50 Hz mains tick, with the
//     latch-on-hours-read, halt-on-hours-write and alarm behaviour.
// Ports: CIA1 = keyboard matrix + joysticks, CIA2 port A = VIC bank + serial bus.
// (Timer B, TOD and one-shot used to be missing on CIA1: games that time music or loaders with
// them hung or ran at the wrong speed.)

namespace c64 {

struct Cia {
  uint16_t ta, taLatch, tb, tbLatch;
  uint8_t cra, crb, icr, mask;
  uint8_t tod[4], alarm[4], todLatch[4];   // tenths, seconds, minutes, hours (BCD, bit 7 = PM)
  bool todLatched, todHalted;
  uint8_t todPulses;                       // mains ticks towards the next tenth
  uint32_t todCyc;                         // CPU cycles towards the next mains tick
};

static Cia cia1, cia2;

// ---- CIA1 ports (keyboard matrix / joysticks) ----
static uint8_t pra1 = 0xff, prb1 = 0xff, ddra1 = 0xff, ddrb1 = 0x00;
// ---- CIA2 ports (VIC bank select + serial bus; port B = user port) ----
static uint8_t pra2 = 0x97, ddra2 = 0x3f, prb2 = 0xff, ddrb2 = 0x00;

// PAL: 985248 Hz / 50 Hz mains.
#define CIA_TOD_TICK_CYC 19705u

static void ciaInit(Cia &c) {
  c.ta = c.taLatch = c.tb = c.tbLatch = 0xffff;
  c.cra = c.crb = c.icr = c.mask = 0;
  c.tod[0] = 0x00; c.tod[1] = 0x00; c.tod[2] = 0x00; c.tod[3] = 0x01;   // 1:00:00.0 AM
  for (int i = 0; i < 4; i++) { c.alarm[i] = 0; c.todLatch[i] = c.tod[i]; }
  c.todLatched = c.todHalted = false;
  c.todPulses = 0;
  c.todCyc = 0;
}

void ciaReset() {
  pra1 = 0xff; prb1 = 0xff; ddra1 = 0xff; ddrb1 = 0x00;
  pra2 = 0x97; ddra2 = 0x3f; prb2 = 0xff; ddrb2 = 0x00;
  ciaInit(cia1);
  ciaInit(cia2);
}

static inline void ciaRaise(Cia &c, uint8_t flag) {
  c.icr |= flag;
  if (c.icr & c.mask & 0x1f) c.icr |= 0x80;
}

static inline uint8_t bcdInc(uint8_t v) {
  v++;
  if ((v & 0x0f) > 9) v += 6;
  return v;
}

static void todAdvance(Cia &c) {
  c.tod[0]++;
  if (c.tod[0] > 9) {
    c.tod[0] = 0;
    c.tod[1] = bcdInc(c.tod[1]);
    if (c.tod[1] >= 0x60) {
      c.tod[1] = 0;
      c.tod[2] = bcdInc(c.tod[2]);
      if (c.tod[2] >= 0x60) {
        c.tod[2] = 0;
        uint8_t h = c.tod[3] & 0x1f, pm = c.tod[3] & 0x80;
        if (h == 0x11)      { h = 0x12; pm ^= 0x80; }
        else if (h == 0x12) h = 0x01;
        else                h = bcdInc(h);
        c.tod[3] = h | pm;
      }
    }
  }
  if (c.tod[0] == c.alarm[0] && c.tod[1] == c.alarm[1] &&
      c.tod[2] == c.alarm[2] && c.tod[3] == c.alarm[3])
    ciaRaise(c, 0x04);
}

static uint8_t todRead(Cia &c, uint8_t i) {     // i: 0 = tenths .. 3 = hours
  if (i == 3 && !c.todLatched) {                 // reading hours freezes the read-out ...
    for (int k = 0; k < 4; k++) c.todLatch[k] = c.tod[k];
    c.todLatched = true;
  }
  uint8_t v = c.todLatched ? c.todLatch[i] : c.tod[i];
  if (i == 0) c.todLatched = false;              // ... until tenths is read
  return v;
}

static void todWrite(Cia &c, uint8_t i, uint8_t val) {
  static const uint8_t mask[4] = { 0x0f, 0x7f, 0x7f, 0x9f };
  val &= mask[i];
  if (c.crb & 0x80) { c.alarm[i] = val; return; }   // CRB bit 7: writes set the alarm
  c.tod[i] = val;
  if (i == 3) c.todHalted = true;                   // writing hours stops the clock ...
  if (i == 0) { c.todHalted = false; c.todPulses = 0; }   // ... until tenths is written
}

// Timer / ICR / TOD registers ($x04-$x0F), common to both chips.
static uint8_t ciaRegRead(Cia &c, uint8_t reg) {
  switch (reg) {
    case 0x04: return c.ta & 0xff;
    case 0x05: return c.ta >> 8;
    case 0x06: return c.tb & 0xff;
    case 0x07: return c.tb >> 8;
    case 0x08: case 0x09: case 0x0a: case 0x0b: return todRead(c, reg - 0x08);
    case 0x0d: { uint8_t v = c.icr; c.icr = 0; return v; }   // read clears flags + IRQ/NMI
    case 0x0e: return c.cra & ~0x10;
    case 0x0f: return c.crb & ~0x10;
    default:   return 0xff;
  }
}

static void ciaRegWrite(Cia &c, uint8_t reg, uint8_t val) {
  switch (reg) {
    case 0x04: c.taLatch = (c.taLatch & 0xff00) | val; break;
    case 0x05:
      c.taLatch = (c.taLatch & 0x00ff) | (val << 8);
      // A stopped timer loads the new latch; in one-shot mode the write also starts it.
      if (c.cra & 0x08) { c.ta = c.taLatch; c.cra |= 0x01; }
      else if (!(c.cra & 0x01)) c.ta = c.taLatch;
      break;
    case 0x06: c.tbLatch = (c.tbLatch & 0xff00) | val; break;
    case 0x07:
      c.tbLatch = (c.tbLatch & 0x00ff) | (val << 8);
      if (c.crb & 0x08) { c.tb = c.tbLatch; c.crb |= 0x01; }
      else if (!(c.crb & 0x01)) c.tb = c.tbLatch;
      break;
    case 0x08: case 0x09: case 0x0a: case 0x0b: todWrite(c, reg - 0x08, val); break;
    case 0x0d:
      if (val & 0x80) c.mask |= (val & 0x1f);
      else            c.mask &= ~(val & 0x1f);
      if (c.icr & c.mask & 0x1f) c.icr |= 0x80;      // a flag already pending interrupts now
      break;
    case 0x0e:
      c.cra = val & ~0x10;
      if (val & 0x10) c.ta = c.taLatch;              // bit 4 = force load (strobe)
      break;
    case 0x0f:
      c.crb = val & ~0x10;
      if (val & 0x10) c.tb = c.tbLatch;
      break;
    default: break;
  }
}

C64_HOT static inline void ciaChipTick(Cia &c, int cpuCycles) {
  int taUnder = 0;
  if ((c.cra & 0x21) == 0x01) {                  // Timer A started, counting phi2
    int32_t t = (int32_t)c.ta - cpuCycles;
    while (t < 0) {                              // counts latch..0, then reloads: latch+1 cycles
      taUnder++;
      ciaRaise(c, 0x01);
      if (c.cra & 0x08) { c.cra &= ~0x01; t = c.taLatch; break; }   // one-shot: stop
      t += (int32_t)c.taLatch + 1;
    }
    c.ta = (uint16_t)t;
  }
  if (c.crb & 0x01) {                            // Timer B started
    int n = (c.crb & 0x40) ? taUnder                     // counts Timer A underflows
                           : ((c.crb & 0x20) ? 0 : cpuCycles);   // CNT (idle) / phi2
    if (n) {
      int32_t t = (int32_t)c.tb - n;
      while (t < 0) {
        ciaRaise(c, 0x02);
        if (c.crb & 0x08) { c.crb &= ~0x01; t = c.tbLatch; break; }
        t += (int32_t)c.tbLatch + 1;
      }
      c.tb = (uint16_t)t;
    }
  }
  c.todCyc += cpuCycles;
  if (c.todCyc >= CIA_TOD_TICK_CYC) {            // 50 Hz mains tick; CRA bit 7 = 50 Hz divider
    c.todCyc -= CIA_TOD_TICK_CYC;
    if (!c.todHalted && ++c.todPulses >= ((c.cra & 0x80) ? 5 : 6)) {
      c.todPulses = 0;
      todAdvance(c);
    }
  }
}

C64_HOT void ciaTick(int cpuCycles) {
  ciaChipTick(cia1, cpuCycles);
  ciaChipTick(cia2, cpuCycles);
}

unsigned char cia1Read(uint8_t reg) {
#if defined(BOARD_DESKTOP)   // TEMP: EMU_CIALOG=1 logs each distinct joystick-port read site
  static bool lg = getenv("EMU_CIALOG") != nullptr;
  if (lg && reg <= 1) {
    static uint32_t seen[512]; static int ns = 0;
    uint32_t key = ((uint32_t)lastPC << 8) | (reg << 4) | (ddra1 == 0xff) << 1 | (ddrb1 == 0);
    bool f = false; for (int i = 0; i < ns; i++) if (seen[i] == key) { f = true; break; }
    if (!f && ns < 512) { seen[ns++] = key;
      fprintf(stderr, "CIA1 rd $DC0%d pc=%04x pra=%02x prb=%02x ddra=%02x ddrb=%02x\n", reg, lastPC, pra1, prb1, ddra1, ddrb1); }
  }
#endif
  switch (reg) {
    // A port line set as output reads its latch; one set as input floats high. Only output lines
    // drive the matrix. Reading the joystick through PRA alone dropped the latch, so a game that
    // tests the whole byte (Bomb Jack: LDA $DC00 / CMP #$6F with PRA = $7F) never saw FIRE.
    case 0x00: return (uint8_t)((pra1 | ~ddra1) & kbReadCols(prb1 | ~ddrb1));   // PRA: reverse scan + joystick port 2
    case 0x01: return (uint8_t)((prb1 | ~ddrb1) & kbReadRows(pra1 | ~ddra1));   // PRB: rows for the selected columns + port 1
    case 0x02: return ddra1;
    case 0x03: return ddrb1;
    default:   return ciaRegRead(cia1, reg);
  }
}

void cia1Write(uint8_t reg, uint8_t val) {
  switch (reg) {
    case 0x00: pra1 = val; break;
    case 0x01: prb1 = val; break;
    case 0x02: ddra1 = val; break;
    case 0x03: ddrb1 = val; break;
    default:   ciaRegWrite(cia1, reg, val); break;
  }
}

bool cia1IRQPending() { return (cia1.icr & 0x80) != 0; }

unsigned char cia2Read(uint8_t reg) {
  switch (reg) {
    case 0x00: {
      // Port A: bits 0-5 = VIC bank + serial/RS232 OUTPUTS (read back the latch); bits 6-7 = serial
      // CLK IN / DATA IN, which are INPUTS (ddra2=0x3f). With no drive on the bus (open-collector +
      // pull-ups) those inputs read the INVERSE of the C64's own CLK OUT (PA4) / DATA OUT (PA5):
      // a released output leaves the bus line high. Cart loaders poll this to confirm the serial bus
      // is idle before starting (write PA4/PA5=0, then wait for PA6/PA7=1); returning the raw latch
      // left bit6 stuck at 0 and hung them in an infinite loop. (No real 1541 here — LOAD is trapped
      // at $F49E, so modelling the idle bus this way is both correct and harmless.)
      uint8_t v = pra2 & 0x3f;
      if (!(pra2 & 0x10)) v |= 0x40;   // CLK OUT released  -> CLK IN  high
      if (!(pra2 & 0x20)) v |= 0x80;   // DATA OUT released -> DATA IN high
      return v;
    }
    case 0x01: return prb2 | ~ddrb2;   // user port: nothing attached, inputs float high
    case 0x02: return ddra2;
    case 0x03: return ddrb2;
    default:   return ciaRegRead(cia2, reg);
  }
}

void cia2Write(uint8_t reg, uint8_t val) {
  switch (reg) {
    case 0x00: {                         // VIC bank select (PRA bits 0-1, inverted)
      pra2 = val;
      switch (val & 3) {
        case 0: vicmem = 0xc000; break;
        case 1: vicmem = 0x8000; break;
        case 2: vicmem = 0x4000; break;
        case 3: vicmem = 0x0000; break;
      }
      adaptVICBaseAddrs(true);
      break;
    }
    case 0x01: prb2 = val; break;
    case 0x02: ddra2 = val; break;
    case 0x03: ddrb2 = val; break;
    default:   ciaRegWrite(cia2, reg, val); break;
  }
}

bool cia2NMIPending() { return (cia2.icr & 0x80) != 0; }

// Snapshot (c64_snapshot.cpp): every register this CIA model keeps, same order both ways. The VIC
// bank ($DD00) is restored with the port; the VIC's base addresses are rebuilt by vicSnapshot.
// Version 1 snapshots predate CIA1 Timer B, CIA2 port B and the TOD clocks: those load as reset.
static void ciaTodSnapshot(SnapIO &io, Cia &c) {
  io.val(c.tb); io.val(c.tbLatch); io.val(c.crb);
  for (int i = 0; i < 4; i++) { io.val(c.tod[i]); io.val(c.alarm[i]); io.val(c.todLatch[i]); }
  io.flag(c.todLatched); io.flag(c.todHalted);
  io.val(c.todPulses); io.val(c.todCyc);
}

void ciaSnapshot(SnapIO &io, uint16_t ver) {
  if (io.loading) { ciaInit(cia1); ciaInit(cia2); prb2 = 0xff; ddrb2 = 0x00; }
  uint16_t tb2 = cia2.tb, tb2Latch = cia2.tbLatch;   // version 1 order: kept as it was
  uint8_t crb2 = cia2.crb;
  io.val(pra1); io.val(prb1); io.val(ddra1); io.val(ddrb1);
  io.val(cia1.ta); io.val(cia1.taLatch); io.val(cia1.cra); io.val(cia1.icr); io.val(cia1.mask);
  io.val(pra2); io.val(ddra2);
  io.val(cia2.ta); io.val(cia2.taLatch); io.val(tb2); io.val(tb2Latch);
  io.val(cia2.cra); io.val(crb2); io.val(cia2.icr); io.val(cia2.mask);
  if (io.loading) { cia2.tb = tb2; cia2.tbLatch = tb2Latch; cia2.crb = crb2; }
  if (ver < 2) return;
  io.val(prb2); io.val(ddrb2);
  ciaTodSnapshot(io, cia1);
  ciaTodSnapshot(io, cia2);   // re-saves CIA2's Timer B: harmless, keeps one helper
}

} // namespace c64
