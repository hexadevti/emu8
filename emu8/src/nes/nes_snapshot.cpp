#include "../../emu.h"
#include "nes.h"
#include "../shared/snapshot.h"

// NES snapshot (save state) -- the core's half of src/shared/snapshot.cpp. One function, both
// directions: every field goes through SnapIO in the same order for save and load.
//
// Stream layout (all little-endian, fixed-width on every build):
//   "NCRT"  cartridge identity: u32 PRG size, u32 CHR size, u8 mapper, u8 CHR-is-RAM,
//           u8 has-PRG-RAM, u32 fingerprint (FNV-1a over the first/last 4K of PRG and CHR-ROM,
//           cartChecksum in nes_cart.cpp). Checked BEFORE anything is overwritten: a snapshot of
//           another cart is refused and the running game carries on.
//   "CPU "  u16 PC, u8 SP/A/X/Y/P, u8 NMI pending, u8 IRQ line, i32 OAM-DMA stall, 2K work RAM
//   "CRAM"  8K PRG-RAM (battery / work RAM, when present), 8K CHR-RAM (CHR-RAM carts only)
//   "MAPR"  every mapper's registers + mirroring (nes_mapper.cpp); bank windows rebuilt on load
//   "PPU "  registers, loopy v/t/x/w, read buffer, scanline/dot position, OAM, palette, 2K VRAM
//   "APU "  channel registers, counters and phases (nes_apu.cpp)
//   "PAD "  controller shift register + strobe (nes_memory.cpp)
//   "FB  "  the 256x240 indexed picture, so the restored frame is on screen straight away
//
// The cartridge ROM itself is never saved, and nor is any pointer (prgMap/chrMap are rebuilt by
// the mapper's own setters). Everything streams from the buffers it lives in -- no allocation.
//
// Runs on the render task with the CPU parked between instructions in cpuLoop's pause spin (or in
// the no-cart idle), so the state is consistent; the PPU is advanced by the CPU loop, so it is
// parked too. Only the audio task keeps going, and it only reads the APU.
//
// The body sits inside namespace nes so PC/A/X/... bind to the NES registers, not the Apple II
// globals of the same name (see the linkage note in nes.h).

namespace nes {

static bool snapshot(SnapIO &io)
{
  // ---- cartridge identity (into locals; nothing in the machine changes before this passes) ----
  io.tag("NCRT");
  uint32_t sum = 0;
  const bool haveCart = prgRomSize && prgMap[0] && cpuRam && vram && framebuffer;
  const bool summed = haveCart && cartChecksum(sum);
  uint32_t prgSz = prgRomSize, chrSz = chrSize, fp = sum;
  uint8_t mapper = mapperNum, chrRam = chrIsRam ? 1 : 0, hasPrgRam = prgRam ? 1 : 0;
  if (!io.loading && !haveCart) { io.fail("No cartridge loaded"); return false; }
  if (!io.loading && !summed)   { io.fail("Cannot read the cartridge"); return false; }
  io.val(prgSz); io.val(chrSz); io.val(mapper); io.val(chrRam); io.val(hasPrgRam); io.val(fp);
  if (io.loading) {
    if (!io.ok)                    { io.reject(io.err); return false; }   // nothing read in yet
    if (!haveCart)                 { io.reject("No cartridge loaded"); return false; }
    if (!summed)                   { io.reject("Cannot read the cartridge"); return false; }
    if (prgSz != prgRomSize || chrSz != chrSize || mapper != mapperNum ||
        chrRam != (chrIsRam ? 1 : 0) || fp != sum)
                                   { io.reject("Load the same cartridge first"); return false; }
    if (hasPrgRam && !prgRam)      { io.reject("No memory for the cartridge RAM"); return false; }
  }
  if (!io.ok) return false;

  // ---- CPU ----
  io.tag("CPU ");
  uint16_t pc = PC;
  uint8_t sp = STP, a = A, x = X, y = Y, p = SR;
  bool nmi = nmiPending, irq = irqLine;
  int32_t stall = dmaStallCycles;
  io.val(pc); io.val(sp); io.val(a); io.val(x); io.val(y); io.val(p);
  io.flag(nmi); io.flag(irq);
  io.val(stall);
  io.bytes(cpuRam, 0x800);

  // ---- cartridge RAM ----
  io.tag("CRAM");
  if (hasPrgRam) io.bytes(prgRam, 8192);                   // validated above: prgRam exists
  else if (io.loading && prgRam) memset(prgRam, 0, 8192);  // saved without one: start it blank
  if (chrIsRam) io.bytes(chrData, chrSize);

  // ---- mapper, PPU, APU, controller (each next to the state it owns) ----
  mapperSnapshot(io);
  ppuSnapshot(io);
  apuSnapshot(io);
  ctrlSnapshot(io);

  // ---- the picture ----
  io.tag("FB  ");
  io.bytes(framebuffer, 256 * 240);

  if (io.loading && io.ok) {
    PC = pc; STP = sp; A = a; X = x; Y = y; SR = p | 0x20;   // bit 5 always reads set
    nmiPending = nmi;
    irqLine = irq;
    dmaStallCycles = (stall >= 0 && stall <= 1024) ? (int)stall : 0;
    // The PPU sits out the rest of the frame in progress (it may only start drawing at line 0 with
    // a fresh grant), and the restored picture is published so the render task pushes it at once
    // instead of the one that was on screen before the load.
    fbRendering = false;
    fbFrames++;
    cpuSnapRestored();                 // no pending ROM-load reset may wipe what was just loaded
  }
  return io.ok;
}

} // namespace nes

bool nesSnapshot(SnapIO &io) { return nes::snapshot(io); }

