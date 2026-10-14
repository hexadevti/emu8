#include "../../emu.h"
#include "atari.h"

// 6507 CPU bus. The 6507 only drives 13 address lines (A0-A12), so the whole map repeats every
// $2000. Minimal chip-select decode (the same scheme Stella uses):
//   A12 (0x1000) set            -> cartridge ROM ($1000-$1FFF, 4K window)
//   A7  (0x0080) set, A9 clear  -> RIOT RAM (128 bytes; also the stack at $0180-$01FF)
//   A7  set, A9 (0x0200) set    -> RIOT I/O + timer ($0280-$029F)
//   A12 clear, A7 clear         -> TIA (writes $00-$2C, reads collisions/inputs $00-$0D)
//
// The hot path (opcode/operand fetch from ROM, zero-page/stack RAM) is inlined in atari_cpu.cpp;
// these full-decode versions back read16/reset-vectors and any non-inlined access.

namespace atari {

unsigned char read8(unsigned short addr) {
  if (addr & 0x1000) return cartRead(addr);          // cartridge ROM (+ bank/Superchip hotspots)
  if (addr & 0x0080) {                               // RIOT chip selected
    if (addr & 0x0200) return riotRead(addr);        // I/O ports + interval timer
    return riotRam[addr & 0x7F];                      // 128-byte RAM (mirrored)
  }
  return tiaRead(addr & 0x0F);                        // TIA read registers ($00-$0D, mirrored /16)
}

void write8(unsigned short addr, unsigned char val) {
  if (addr & 0x1000) { cartWrite(addr, val); return; }   // bank hotspots / Superchip RAM
  if (addr & 0x0080) {                                    // RIOT chip selected
    if (addr & 0x0200) { riotWrite(addr, val); return; }  // I/O ports + timer
    riotRam[addr & 0x7F] = val; return;                   // 128-byte RAM
  }
  tiaWrite(addr & 0x3F, val);                             // TIA write registers ($00-$2C)
}

unsigned short read16(unsigned short addr) {
  return (unsigned short)read8(addr) | (((unsigned short)read8(addr + 1)) << 8);
}

} // namespace atari
