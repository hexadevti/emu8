#include "../../emu.h"
#include "nes.h"

// NES CPU bus ($0000-$FFFF). Layout:
//   $0000-$1FFF  2K internal RAM, mirrored every $0800
//   $2000-$3FFF  PPU registers, mirrored every 8 ($2000-$2007)
//   $4000-$4013  APU (stubbed in phase 1)
//   $4014        OAM DMA (write)
//   $4015        APU status (read 0)
//   $4016        controller 1 (read) / strobe (write)
//   $4017        controller 2 (read) / APU frame counter (write, ignored)
//   $4020-$5FFF  expansion / unused (open bus -> 0)
//   $6000-$7FFF  cartridge PRG-RAM (mapper 0: optional 8K)
//   $8000-$FFFF  cartridge PRG ROM (mapper 0: 16K mirrored or 32K)

namespace nes {

// ---- Controller 1 (standard pad) ----
// Buttons are latched by `setController` (called from the joystick task). A write to $4016
// with bit0=1 holds the parallel-load strobe; the 1->0 edge latches the state into a shift
// register that $4016 reads out serially (A, B, Select, Start, Up, Down, Left, Right).
static volatile uint8_t ctrlState = 0;   // live button bits
static uint8_t ctrlShift = 0;            // serial shift register
static bool    ctrlStrobe = false;

void setController(uint8_t buttons) {
  ctrlState = buttons;
  if (ctrlStrobe) ctrlShift = buttons;   // transparent while strobe is high
}

void controllerWrite(uint8_t val) {
  ctrlStrobe = val & 1;
  if (ctrlStrobe) ctrlShift = ctrlState;
}

uint8_t controllerRead(int port) {
  if (port != 0) return 0x40;            // controller 2 not connected (open-bus high bits)
  if (ctrlStrobe) ctrlShift = ctrlState; // strobe high -> always returns button A
  uint8_t b = ctrlShift & 1;
  ctrlShift = (ctrlShift >> 1) | 0x80;   // after 8 reads a standard pad returns 1s
  return b | 0x40;                       // bit6 = open-bus high (common real-hardware value)
}

unsigned char read8(unsigned short addr) {
  if (addr < 0x2000) return cpuRam[addr & 0x07FF];
  if (addr < 0x4000) return ppuRegRead(addr & 7);
  if (addr < 0x4020) {
    if (addr == 0x4015) return apuReadStatus();
    if (addr == 0x4016) return controllerRead(0);
    if (addr == 0x4017) return controllerRead(1);
    return 0;                            // other $401x: open bus
  }
  if (addr < 0x6000) return 0;           // expansion area
  if (addr < 0x8000) return prgRam ? prgRam[addr - 0x6000] : 0;
  uint8_t *p = prgMap[(addr >> 13) & 3]; // $8000-$FFFF: one of four 8K mapper windows
  return p ? p[addr & 0x1FFF] : 0;
}

void write8(unsigned short addr, unsigned char val) {
  if (addr < 0x2000) { cpuRam[addr & 0x07FF] = val; return; }
  if (addr < 0x4000) { ppuRegWrite(addr & 7, val); return; }
  if (addr < 0x4020) {
    if (addr == 0x4014) { oamDmaWrite(val); return; }   // OAM DMA
    if (addr == 0x4016) { controllerWrite(val); return; }
    apuWrite(addr & 0x1F, val);          // $4000-$4013, $4015, $4017 -> APU
    return;
  }
  if (addr < 0x6000) return;             // expansion area
  if (addr < 0x8000) { if (prgRam) prgRam[addr - 0x6000] = val; return; }
  mapperWrite(addr, val);                // $8000-$FFFF: mapper bank registers
}

unsigned short read16(unsigned short address) {
  return (unsigned short)read8(address) | (((unsigned short)read8(address + 1)) << 8);
}

void write16(unsigned short address, unsigned short value) {
  write8(address, value & 0x00FF);
  write8(address + 1, (value >> 8) & 0x00FF);
}

} // namespace nes
