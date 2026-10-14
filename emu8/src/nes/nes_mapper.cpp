#include "../../emu.h"
#include "nes.h"

// NES mapper layer. Phase 3a: mappers 0 (NROM), 1 (MMC1), 2 (UxROM), 3 (CNROM).
//
// The whole PRG + CHR ROM is held in RAM (malloc'd by nes_cart.cpp). Banking is done by
// repointing windows into those buffers rather than copying banks:
//   PRG: 4 x 8K windows -> prgMap[0..3] for $8000/$A000/$C000/$E000
//   CHR: 8 x 1K windows -> chrMap[0..7] for $0000..$1FFF
// read8 (nes_memory.cpp) and chrRead (nes_ppu.cpp) index these windows directly, so reads stay
// branch-free and fast. Register writes ($8000-$FFFF) land in mapperWrite, which recomputes the
// affected windows. 8K/1K granularity is the least common multiple of every supported mapper
// (UxROM/MMC1 16K PRG, MMC3-style 8K PRG, CNROM 8K CHR, MMC1 4K CHR) so the same window array
// serves all of them and leaves room for future mappers.

namespace nes {

static int prg8kCount = 0;     // prgRomSize / 8192
static int chr1kCount = 0;     // chrSize   / 1024

// ---- window setters (bank numbers are wrapped to the ROM size, like real hardware) ----
static inline void setPrg8k(int window, int bank8k) {
  if (prg8kCount <= 0) return;
  bank8k %= prg8kCount; if (bank8k < 0) bank8k += prg8kCount;
  prgMap[window & 3] = prgBankResolve(window & 3, bank8k);  // full-RAM ptr or SD-streamed slot
}
// 16K bank into a window pair (window 0 -> $8000, 2 -> $C000)
static inline void setPrg16k(int window, int bank16k) {
  setPrg8k(window,     bank16k * 2);
  setPrg8k(window + 1, bank16k * 2 + 1);
}
static inline void setPrg32k(int bank32k) {
  for (int i = 0; i < 4; i++) setPrg8k(i, bank32k * 4 + i);
}
static inline void setChr1k(int window, int bank1k) {
  if (chr1kCount <= 0) return;
  bank1k %= chr1kCount; if (bank1k < 0) bank1k += chr1kCount;
  chrMap[window & 7] = chrData + (uint32_t)bank1k * 1024;
}
// 4K bank into a window quad (window 0 -> $0000, 4 -> $1000)
static inline void setChr4k(int window, int bank4k) {
  for (int i = 0; i < 4; i++) setChr1k(window + i, bank4k * 4 + i);
}
static inline void setChr8k(int bank8k) {
  for (int i = 0; i < 8; i++) setChr1k(i, bank8k * 8 + i);
}

// ============================ MMC1 (mapper 1) ============================
// 5-bit serial shift register loaded LSB-first; bit7 set on a write resets it (and forces PRG
// mode 3 = fixed last bank). Every 5th write commits to one of four registers selected by the
// write address: $8000 control, $A000 CHR0, $C000 CHR1, $E000 PRG.
static uint8_t mmc1Shift = 0x10;   // bit4 = "ready" marker; reaches bit0 on the 5th write
static uint8_t mmc1Ctrl  = 0x0C;   // power-on: PRG mode 3 (fix last 16K at $C000)
static uint8_t mmc1Chr0  = 0, mmc1Chr1 = 0, mmc1Prg = 0;

static void mmc1ApplyMirror() {
  switch (mmc1Ctrl & 3) {
    case 0: mirrorMode = MIRROR_SINGLE0;    break;
    case 1: mirrorMode = MIRROR_SINGLE1;    break;
    case 2: mirrorMode = MIRROR_VERTICAL;   break;
    case 3: mirrorMode = MIRROR_HORIZONTAL; break;
  }
}
static void mmc1ApplyBanks() {
  int mode    = (mmc1Ctrl >> 2) & 3;
  int prgBank = mmc1Prg & 0x0F;                 // 16K units
  int last16k = prg8kCount / 2 - 1;
  switch (mode) {
    case 0: case 1: setPrg32k(prgBank >> 1);                 break;  // 32K (low bit ignored)
    case 2: setPrg16k(0, 0);       setPrg16k(2, prgBank);   break;  // fix first 16K @ $8000
    case 3: setPrg16k(0, prgBank); setPrg16k(2, last16k);   break;  // fix last 16K  @ $C000
  }
  if (mmc1Ctrl & 0x10) {                        // two independent 4K CHR banks
    setChr4k(0, mmc1Chr0);
    setChr4k(4, mmc1Chr1);
  } else {                                       // single 8K CHR bank (low bit ignored)
    setChr8k(mmc1Chr0 >> 1);
  }
}
static void mmc1Write(uint16_t addr, uint8_t val) {
  if (val & 0x80) {                              // reset
    mmc1Shift = 0x10;
    mmc1Ctrl |= 0x0C;
    mmc1ApplyBanks();
    return;
  }
  bool willComplete = mmc1Shift & 1;             // marker at bit0 -> this is the 5th write
  mmc1Shift = ((val & 1) << 4) | (mmc1Shift >> 1);
  if (!willComplete) return;
  uint8_t reg = mmc1Shift & 0x1F;
  mmc1Shift = 0x10;
  switch ((addr >> 13) & 3) {
    case 0: mmc1Ctrl = reg; mmc1ApplyMirror(); break;   // $8000-$9FFF control
    case 1: mmc1Chr0 = reg;                    break;   // $A000-$BFFF CHR bank 0
    case 2: mmc1Chr1 = reg;                    break;   // $C000-$DFFF CHR bank 1
    case 3: mmc1Prg  = reg;                    break;   // $E000-$FFFF PRG bank
  }
  mmc1ApplyBanks();
}

// ============================ UxROM (mapper 2) ===========================
// 16K switchable bank at $8000, fixed last 16K at $C000. CHR is usually 8K RAM.
static void uxromWrite(uint16_t /*addr*/, uint8_t val) {
  setPrg16k(0, val & 0x0F);
}

// ============================ CNROM (mapper 3) ===========================
// Fixed PRG (16K mirrored or 32K); 8K CHR bank selected by any write to $8000-$FFFF.
static void cnromWrite(uint16_t /*addr*/, uint8_t val) {
  setChr8k(val & 0x03);
}

// ============================ MMC3 (mapper 4) ============================
// 8K PRG banking (2 swappable + 2 fixed), 1K/2K CHR banking, dynamic mirroring, and a scanline
// counter IRQ used for mid-frame splits (status bars). The A12-clocked counter is approximated
// by clocking once per visible scanline (mapperClockScanline, called from the PPU) — accurate
// enough for the common case (SMB3-style status bars). IRQ is delivered via the global irqLine.
static uint8_t mmc3BankSelect = 0;   // $8000: bits0-2 reg index, bit6 PRG mode, bit7 CHR mode
static uint8_t mmc3Regs[8] = {0};    // R0..R7 (R0/R1 = 2K CHR, R2-R5 = 1K CHR, R6/R7 = 8K PRG)
static uint8_t mmc3IrqLatch = 0;
static uint8_t mmc3IrqCounter = 0;
static bool    mmc3IrqReload = false;
static bool    mmc3IrqEnable = false;

static void mmc3UpdateBanks() {
  int last = prg8kCount - 1, secondLast = prg8kCount - 2;
  if (mmc3BankSelect & 0x40) {                 // PRG mode 1: $C000 swappable, $8000 fixed
    setPrg8k(0, secondLast); setPrg8k(1, mmc3Regs[7]);
    setPrg8k(2, mmc3Regs[6]); setPrg8k(3, last);
  } else {                                      // PRG mode 0: $8000 swappable, $C000 fixed
    setPrg8k(0, mmc3Regs[6]); setPrg8k(1, mmc3Regs[7]);
    setPrg8k(2, secondLast);  setPrg8k(3, last);
  }
  if (mmc3BankSelect & 0x80) {                 // CHR mode 1: 1K banks at $0000, 2K at $1000
    setChr1k(0, mmc3Regs[2]); setChr1k(1, mmc3Regs[3]);
    setChr1k(2, mmc3Regs[4]); setChr1k(3, mmc3Regs[5]);
    setChr1k(4, mmc3Regs[0] & 0xFE); setChr1k(5, (mmc3Regs[0] & 0xFE) + 1);
    setChr1k(6, mmc3Regs[1] & 0xFE); setChr1k(7, (mmc3Regs[1] & 0xFE) + 1);
  } else {                                      // CHR mode 0: 2K banks at $0000, 1K at $1000
    setChr1k(0, mmc3Regs[0] & 0xFE); setChr1k(1, (mmc3Regs[0] & 0xFE) + 1);
    setChr1k(2, mmc3Regs[1] & 0xFE); setChr1k(3, (mmc3Regs[1] & 0xFE) + 1);
    setChr1k(4, mmc3Regs[2]); setChr1k(5, mmc3Regs[3]);
    setChr1k(6, mmc3Regs[4]); setChr1k(7, mmc3Regs[5]);
  }
}
static void mmc3Write(uint16_t addr, uint8_t val) {
  switch (addr & 0xE001) {
    case 0x8000: mmc3BankSelect = val; mmc3UpdateBanks(); break;
    case 0x8001: mmc3Regs[mmc3BankSelect & 7] = val; mmc3UpdateBanks(); break;
    case 0xA000: mirrorMode = (val & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL; break;
    case 0xA001: break;                         // PRG-RAM protect — ignored (RAM always enabled)
    case 0xC000: mmc3IrqLatch = val; break;
    case 0xC001: mmc3IrqReload = true; break;
    case 0xE000: mmc3IrqEnable = false; irqLine = false; break;  // disable + acknowledge
    case 0xE001: mmc3IrqEnable = true; break;
  }
}
// Clocked once per visible scanline by the PPU when rendering is enabled.
static void mmc3ClockIrq() {
  if (mmc3IrqCounter == 0 || mmc3IrqReload) { mmc3IrqCounter = mmc3IrqLatch; mmc3IrqReload = false; }
  else                                       { mmc3IrqCounter--; }
  if (mmc3IrqCounter == 0 && mmc3IrqEnable) irqLine = true;
}

void mapperClockScanline() {
  if (mapperNum == 4) mmc3ClockIrq();
}

// ============================ public API ================================
void mapperInit() {
  prg8kCount = prgRomSize / 8192;
  chr1kCount = chrSize    / 1024;

  // Default linear mapping (NROM, and the fixed/initial state of the others):
  // 32K straight through, or a 16K image mirrored into both halves.
  if (prg8kCount >= 4) setPrg32k(0);
  else { setPrg16k(0, 0); setPrg16k(2, 0); }
  setChr8k(0);

  switch (mapperNum) {
    case 1:   // MMC1: reset to power-on state, then apply
      mmc1Shift = 0x10; mmc1Ctrl = 0x0C;
      mmc1Chr0 = 0; mmc1Chr1 = 0; mmc1Prg = 0;
      mmc1ApplyMirror();
      mmc1ApplyBanks();
      break;
    case 2:   // UxROM: bank 0 at $8000, last 16K fixed at $C000
      setPrg16k(0, 0);
      setPrg16k(2, prg8kCount / 2 - 1);
      break;
    case 4:   // MMC3: reset registers, fix last two 8K PRG banks, apply
      mmc3BankSelect = 0;
      memset(mmc3Regs, 0, sizeof(mmc3Regs));
      mmc3IrqLatch = 0; mmc3IrqCounter = 0; mmc3IrqReload = false; mmc3IrqEnable = false;
      irqLine = false;
      mmc3UpdateBanks();
      break;
    case 3:   // CNROM: PRG linear (set above), CHR bank 0 (set above)
    default:  // NROM
      break;
  }
}

void mapperWrite(uint16_t addr, uint8_t val) {
  switch (mapperNum) {
    case 1: mmc1Write(addr, val);  break;
    case 2: uxromWrite(addr, val); break;
    case 3: cnromWrite(addr, val); break;
    case 4: mmc3Write(addr, val);  break;
    default: break;  // NROM: $8000-$FFFF writes are ignored
  }
}

} // namespace nes
