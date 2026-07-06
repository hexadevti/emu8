#include "../../emu.h"
#if defined(BOARD_DESKTOP)
#include "../desktop/debug_bridge.h"   // dbgBusTouch: desktop heat map + watchpoints (no-op on device)
#endif
#include "c64.h"

// C64 memory map + $01 banking, ported from C64Esp32 memory.ino. I/O ($D000-$DFFF)
// dispatches to VIC registers, color RAM, SID (phase 4) and CIA1/CIA2.

namespace c64 {

#if defined(BOARD_DESKTOP)
// Cartridge ROM-access map: record a read of the current 8K bank at `off16k` (0..0x3FFF within the
// $8000-$BFFF window). Gated on the disk Record toggle so it costs nothing while off.
static inline void cartHeat(uint16_t off16k) { if (::g_dbgDiskHeatOn) ::dbgCartRead(::c64CartCurBank(), off16k); }
#else
static inline void cartHeat(uint16_t) {}
#endif

void memoryAlloc() {
  ram = (unsigned char *)malloc(0x10000 * sizeof(unsigned char));
  if (ram) memset(ram, 0, 0x10000 * sizeof(unsigned char));   // null-guard (don't crash on OOM)
}

unsigned char read8(unsigned short addr) {
#if defined(BOARD_DESKTOP)
  dbgBusTouch(addr, DBG_HEAT_R);
#endif
  // Cartridge ROM overrides (generic carts via EXROM/GAME). Checked first so cart ROM
  // takes precedence over BASIC/KERNAL/RAM in the mapped regions.
  if (cartActive && addr >= 0x8000) {
    bool ultimax = cartExrom && !cartGame;       // EXROM=1, GAME=0
    if (ultimax) {
      if (cartROML && addr <= 0x9fff)            { cartHeat(addr - 0x8000);          return cartROML[addr - 0x8000]; }
      if (cartROMH && addr >= 0xe000)            { cartHeat((addr - 0xe000) + 0x2000); return cartROMH[addr - 0xe000]; }
      if (addr >= 0xa000 && addr <= 0xcfff) return ram[addr];   // open bus (no BASIC)
    } else {
      bool loram = register1 & 1, hiram = register1 & 2;
      if (cartROML && !cartExrom && loram && hiram && addr <= 0x9fff)
        { cartHeat(addr - 0x8000);          return cartROML[addr - 0x8000]; }        // ROML $8000-$9FFF
      if (cartROMH && !cartGame && hiram && addr >= 0xa000 && addr <= 0xbfff)
        { cartHeat((addr - 0xa000) + 0x2000); return cartROMH[addr - 0xa000]; }       // ROMH $A000-$BFFF (16K)
    }
  }
  if ((!bankARAM) && (addr >= 0xa000) && (addr <= 0xbfff)) {
    return basic_rom[addr - 0xa000];
  } else if ((!bankERAM) && (addr >= 0xe000)) {
    return kernal_rom[addr - 0xe000];
  } else if (bankDIO && (addr >= 0xd000) && (addr <= 0xdfff)) {
    if (addr <= 0xd3ff) { // VIC
      uint8_t vicidx = (addr - 0xd000) % 0x40;
      if ((vicidx == 0x1e) || (vicidx == 0x1f)) {
        uint8_t val = vicreg[vicidx];
        vicreg[vicidx] = 0;
        return val;
      } else if (vicidx == 0x11) {
        uint8_t raster8 = (rasterline >= 256) ? 0x80 : 0;
        return (vicreg[0x11] & 0x7f) | raster8;
      }
      return vicreg[vicidx];
    } else if (addr <= 0xd7ff) {          // SID ($D400-$D41F, mirrored to $D7FF)
      return ::sidRead((addr - 0xd400) & 0x1f);
    } else if (addr <= 0xdbff) {          // color RAM
      return colormap[addr - 0xd800];
    } else if (addr <= 0xdcff) {          // CIA1
      return cia1Read((addr - 0xdc00) & 0x0f);
    } else if (addr <= 0xddff) {          // CIA2
      return cia2Read((addr - 0xdd00) & 0x0f);
    } else if (addr >= 0xdf00 && ::c64CartIsEF()) {   // I/O2: EasyFlash RAM
      return ::c64CartRamRead(addr);
    }
    // $DE00-$DEFF (cartridge I/O1): falls through to RAM
  } else if ((!bankDRAM) && (addr >= 0xd000) && (addr <= 0xdfff)) {
    return charset_rom[addr - 0xd000];    // character ROM
  } else if (addr == 0x0001) {
    return register1;
  }
  return ram[addr];
}

void write8(unsigned short addr, unsigned char val) {
#if defined(BOARD_DESKTOP)
  dbgBusTouch(addr, DBG_HEAT_W);
#endif
  if (bankDIO && (addr >= 0xd000) && (addr <= 0xdfff)) {
    if (addr <= 0xd3ff) { // VIC
      uint8_t vicidx = (addr - 0xd000) % 0x40;
      if (vicidx == 0x11) {
        latchd011 = val;
        vicreg[vicidx] = val & 0x7f;
        adaptVICBaseAddrs(false);
        badlinecond0 = false;
        if ((rasterline == 0x30) && (val & 0x10)) badlinecond0 = true;
      } else if (vicidx == 0x12) {
        latchd012 = val;
      } else if (vicidx == 0x16) {
        vicreg[vicidx] = val;
        adaptVICBaseAddrs(false);
      } else if (vicidx == 0x18) {
        vicreg[vicidx] = val;
        adaptVICBaseAddrs(false);
      } else if (vicidx == 0x19) {
        vicreg[vicidx] = 0;
      } else if ((vicidx == 0x1e) || (vicidx == 0x1f)) {
        vicreg[vicidx] = 0;
      } else {
        vicreg[vicidx] = val;
      }
    } else if (addr <= 0xd7ff) {          // SID ($D400-$D41F, mirrored to $D7FF)
      ::sidWrite((addr - 0xd400) & 0x1f, val);
    } else if (addr <= 0xdbff) {          // color RAM
      colormap[addr - 0xd800] = val;
    } else if (addr <= 0xdcff) {          // CIA1
      cia1Write((addr - 0xdc00) & 0x0f, val);
    } else if (addr <= 0xddff) {          // CIA2
      cia2Write((addr - 0xdd00) & 0x0f, val);
    } else if (addr <= 0xdeff) {          // $DE00-$DEFF I/O1: cartridge control/bank register
      if (cartActive) ::c64CartBankWrite(addr, val);
      ram[addr] = val;
    } else {                              // $DF00-$DFFF I/O2: EasyFlash RAM (else plain RAM)
      if (::c64CartIsEF()) ::c64CartRamWrite(addr, val);
      else ram[addr] = val;
    }
  } else if (addr == 0x0001) {
    register1 = val;
    decodeRegister1(register1 & 7);
  } else {
    ram[addr] = val;
  }
}

unsigned short read16(unsigned short address) {
  return (unsigned short)read8(address) | (((unsigned short)read8(address + 1)) << 8);
}

void write16(unsigned short address, unsigned short value) {
  write8(address, value & 0x00FF);
  write8(address + 1, (value >> 8) & 0x00FF);
}

void adaptVICBaseAddrs(bool fromcia) {
  uint8_t val = vicreg[0x18];
  uint16_t val1 = (val & 0xf0) << 6;
  // screenmem is used for text mode and bitmap mode
  screenmemstart = vicmem + val1;
  bool bmm = vicreg[0x11] & 32;
  if ((bmm) || fromcia) {
    if ((val & 8) == 0) {
      bitmapstart = vicmem;
    } else {
      bitmapstart = vicmem + 0x2000;
    }
  }
  uint16_t charmemstart;
  if ((!bmm) || fromcia) {
    val1 = (val & 0x0e) << 10;
    charmemstart = vicmem + val1;
    if ((charmemstart == 0x1800) || (charmemstart == 0x9800)) {
      charset = chrom + 0x0800;
    } else if ((charmemstart == 0x1000) || (charmemstart == 0x9000)) {
      charset = chrom;
    } else {
      charset = ram + charmemstart;
    }
  }
}

void decodeRegister1(uint8_t val) {
  switch (val) {
  case 0:
  case 4:
    bankARAM = true; bankDRAM = true; bankERAM = true; bankDIO = false; break;
  case 1:
    bankARAM = true; bankDRAM = false; bankERAM = true; bankDIO = false; break;
  case 2:
    bankARAM = true; bankDRAM = false; bankERAM = false; bankDIO = false; break;
  case 3:
    bankARAM = false; bankDRAM = false; bankERAM = false; bankDIO = false; break;
  case 5:
    bankARAM = true; bankDRAM = true; bankERAM = true; bankDIO = true; break;
  case 6:
    bankARAM = true; bankDRAM = true; bankERAM = false; bankDIO = true; break;
  case 7:
    bankARAM = false; bankDRAM = true; bankERAM = false; bankDIO = true; break;
  }
}

} // namespace c64
