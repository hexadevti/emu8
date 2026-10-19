// This MSX translation unit is compiled out entirely on boards that clear
// BOARD_HAS_MSX_CORE (currently none -- see board.h; it is on for every board).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro;
// the guard below then empties the file, keeping the Z80 core's static RAM out of the image.
#include "../../emu.h"
#if BOARD_HAS_MSX_CORE

// msx_snapshot.cpp - MSX1 save state (see src/shared/snapshot.h). Stream after the file header:
//   "MSX " identity: BIOS length + sample hash, slot-1 cartridge length + hash + detected mapper,
//          disk ROM in slot 2 (u8). Read into locals and compared before anything is touched:
//          the ROMs are not in the file (they live in flash / PSRAM).
//   "Z80 " CPU (z80_snapshot.h)
//   "RAM " 64 KB slot-3 RAM        "VRAM" 16 KB VDP RAM
//   "TMS " VDP registers/latches   "AY  " PSG    "PPI " slot select + port C
//   "CART" bank registers (only when a cartridge is in)
//   "FDC " WD2793 state + sector buffer (the .dsk image is not saved: what is mounted stays)
// The Z80 parks at a frame boundary while the settings window is open, and runFrame() keeps no
// frame-position state of its own, so nothing else is needed. The 8 KB page map (msx_machine.cpp)
// is rebuilt at the top of every frame from the slot / bank registers restored here.

#include "msx.h"
#include "msx_disk.h"
#include "../z80/z80_snapshot.h"

extern volatile bool msxResetReq;   // msx.cpp

bool msxSnapshot(SnapIO &io)
{
  if (!msx::ram || !msx::vram || msx::biosLen == 0) {   // no BIOS / out of memory: nothing runs
    if (io.loading) io.reject("MSX is not running"); else io.fail("MSX is not running");
    return false;
  }
#if BOARD_HAS_MSX2
  if (msx::isMsx2) {                                    // V9938 / mapper / RTC state is not serialised
    if (io.loading) io.reject("Save states not supported in MSX2 mode"); else io.fail("Save states not supported in MSX2 mode");
    return false;
  }
#endif

  // ---- identity -----------------------------------------------------------------------------------
  int32_t biosLen = msx::biosLen;
  uint32_t biosSum = snapRomSum(msx::bios, msx::biosLen);
  int cl = 0;
  const uint8_t *cart = msx::cartImage(1, &cl);
  int32_t cartLen = cl;
  uint32_t cartSum = snapRomSum(cart, cl);
  int32_t mapper = cart ? msx::cartMapper(1) : -1;
  bool diskRom = msx::diskRomInstalled();

  const int32_t biosLen0 = biosLen, cartLen0 = cartLen, mapper0 = mapper;
  const uint32_t biosSum0 = biosSum, cartSum0 = cartSum;
  const bool diskRom0 = diskRom;

  io.tag("MSX ");
  io.val(biosLen); io.val(biosSum);
  io.val(cartLen); io.val(cartSum); io.val(mapper);
  io.flag(diskRom);
  if (!io.ok) return false;
  if (io.loading) {
    if (biosLen != biosLen0 || biosSum != biosSum0) { io.reject("Snapshot uses another MSX BIOS"); return false; }
    if (cartLen != cartLen0 || cartSum != cartSum0 || mapper != mapper0) {
      io.reject(cartLen ? "Load the same cartridge first" : "Snapshot has no cartridge - eject it first");
      return false;
    }
    if (diskRom != diskRom0) {
      io.reject(diskRom ? "Snapshot needs the disk ROM (DISK ROM setting)" : "Snapshot has no disk ROM (DISK ROM setting)");
      return false;
    }
  }

  // ---- machine --------------------------------------------------------------------------------------
  z80Snapshot(io, msx::cpu);
  io.tag("RAM ");
  io.bytes(msx::ram, 0x10000);
  io.tag("VRAM");
  io.bytes(msx::vram, msx::VRAM_SIZE);
  msx::vdpSnapshot(io);
  msx::psgSnapshot(io);
  msx::ppiSnapshot(io);
  if (cartLen) msx::cartSnapshot(io, 1);
  msx::diskSnapshot(io);
  if (!io.ok) return false;

  if (io.loading) {
    msxResetReq = false;           // a reset queued before the load must not wipe it on resume
    msx::vdpRedraw();              // the restored picture, shown when the settings window closes
    msx::frameReady = true;        //   (closing it sets clearScr -> full repaint, no stale bands)
  }
  return io.ok;
}

#endif  // BOARD_HAS_MSX_CORE
