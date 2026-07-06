#include "../../emu.h"
#include "c64.h"

// Minimal CIA1/CIA2 for phase 1: CIA1 keyboard-matrix ports + Timer A (the ~50/60 Hz
// KERNAL IRQ that scans the keyboard and blinks the cursor); CIA2 VIC bank select + NMI.

namespace c64 {

// ---- CIA1 ----
static uint8_t pra1 = 0xff, prb1 = 0xff, ddra1 = 0xff, ddrb1 = 0x00;
static uint16_t ta1 = 0xffff, ta1latch = 0xffff;
static uint8_t cra1 = 0;
static uint8_t icrData1 = 0, icrMask1 = 0;

// ---- CIA2 (Timer A/B drive the NMI; also VIC bank select) ----
static uint8_t pra2 = 0x97, ddra2 = 0x3f;
static uint16_t ta2 = 0xffff, ta2latch = 0xffff;
static uint16_t tb2 = 0xffff, tb2latch = 0xffff;
static uint8_t cra2 = 0, crb2 = 0;
static uint8_t icrData2 = 0, icrMask2 = 0;

void ciaReset() {
  pra1 = 0xff; prb1 = 0xff; ddra1 = 0xff; ddrb1 = 0x00;
  ta1 = ta1latch = 0xffff; cra1 = 0; icrData1 = icrMask1 = 0;
  pra2 = 0x97; ddra2 = 0x3f;
  ta2 = ta2latch = tb2 = tb2latch = 0xffff; cra2 = crb2 = 0;
  icrData2 = icrMask2 = 0;
}

unsigned char cia1Read(uint8_t reg) {
  switch (reg) {
    case 0x00: return kbReadCols(prb1);   // PRA: reverse keyboard scan + joystick port 2
    case 0x01: return kbReadRows(pra1);   // PRB: keyboard rows for the selected columns
    case 0x02: return ddra1;
    case 0x03: return ddrb1;
    case 0x04: return ta1 & 0xff;
    case 0x05: return (ta1 >> 8) & 0xff;
    case 0x0d: { uint8_t v = icrData1; icrData1 = 0; return v; } // read clears flags + IRQ
    case 0x0e: return cra1;
    default:   return 0xff;
  }
}

void cia1Write(uint8_t reg, uint8_t val) {
  switch (reg) {
    case 0x00: pra1 = val; break;
    case 0x01: prb1 = val; break;
    case 0x02: ddra1 = val; break;
    case 0x03: ddrb1 = val; break;
    case 0x04: ta1latch = (ta1latch & 0xff00) | val; break;
    case 0x05: ta1latch = (ta1latch & 0x00ff) | (val << 8); break;
    case 0x0d:
      if (val & 0x80) icrMask1 |= (val & 0x7f);
      else            icrMask1 &= ~(val & 0x7f);
      break;
    case 0x0e:
      cra1 = val;
      if (val & 0x10) ta1 = ta1latch;   // force latch -> counter
      break;
    default: break;
  }
}

void ciaTick(int cpuCycles) {
  if (cra1 & 0x01) {                     // CIA1 Timer A -> IRQ
    int32_t t = (int32_t)ta1 - cpuCycles;
    while (t < 0) {
      t += (ta1latch ? ta1latch : 0x10000);
      icrData1 |= 0x01;                  // Timer A underflow flag
      if (icrMask1 & 0x01) icrData1 |= 0x80;
    }
    ta1 = (uint16_t)t;
  }
  if (cra2 & 0x01) {                     // CIA2 Timer A -> NMI
    int32_t t = (int32_t)ta2 - cpuCycles;
    while (t < 0) {
      t += (ta2latch ? ta2latch : 0x10000);
      icrData2 |= 0x01;
      if (icrMask2 & 0x01) icrData2 |= 0x80;
    }
    ta2 = (uint16_t)t;
  }
  if ((crb2 & 0x01) && !(crb2 & 0x40)) { // CIA2 Timer B (cycle mode) -> NMI
    int32_t t = (int32_t)tb2 - cpuCycles;
    while (t < 0) {
      t += (tb2latch ? tb2latch : 0x10000);
      icrData2 |= 0x02;
      if (icrMask2 & 0x02) icrData2 |= 0x80;
    }
    tb2 = (uint16_t)t;
  }
}

bool cia1IRQPending() { return (icrData1 & 0x80) != 0; }

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
    case 0x02: return ddra2;
    case 0x04: return ta2 & 0xff;
    case 0x05: return (ta2 >> 8) & 0xff;
    case 0x06: return tb2 & 0xff;
    case 0x07: return (tb2 >> 8) & 0xff;
    case 0x0d: { uint8_t v = icrData2; icrData2 = 0; return v; }  // read clears flags + NMI
    case 0x0e: return cra2;
    case 0x0f: return crb2;
    default:   return 0xff;
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
    case 0x02: ddra2 = val; break;
    case 0x04: ta2latch = (ta2latch & 0xff00) | val; break;
    case 0x05: ta2latch = (ta2latch & 0x00ff) | (val << 8); break;
    case 0x06: tb2latch = (tb2latch & 0xff00) | val; break;
    case 0x07: tb2latch = (tb2latch & 0x00ff) | (val << 8); break;
    case 0x0d:
      if (val & 0x80) icrMask2 |= (val & 0x7f);
      else            icrMask2 &= ~(val & 0x7f);
      break;
    case 0x0e: cra2 = val; if (val & 0x10) ta2 = ta2latch; break;  // bit4 = force latch
    case 0x0f: crb2 = val; if (val & 0x10) tb2 = tb2latch; break;
    default: break;
  }
}

bool cia2NMIPending() { return (icrData2 & 0x80) != 0; }

} // namespace c64
