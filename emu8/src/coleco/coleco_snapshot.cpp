// This ColecoVision translation unit is compiled out entirely on boards that clear
// BOARD_HAS_COLECO_CORE (it needs both the MSX VDP and the SMS PSG -- see board.h).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro.
#include "../../emu.h"
#if BOARD_HAS_COLECO_CORE

// coleco_snapshot.cpp - ColecoVision save state (see src/shared/snapshot.h). Stream after the header:
//   "COL " identity: BIOS length + hash, cartridge length + sample hash (0 = no cartridge). Read into
//          locals and compared before anything is touched (neither image is in the file).
//   "Z80 " CPU (z80_snapshot.h)
//   "RAM " 1 KB work RAM     "VRAM" 16 KB
//   "TMS " VDP (msx::vdpSnapshot)   "SN  " PSG (sms::psgSnapshot)
//   "CMAC" MegaCart bank, controller mode, NMI line level
// The Z80 parks at a frame boundary while the settings window is open; runFrame() always starts at
// line 0, so there is no frame-position state.

#include "coleco.h"
#include "../msx/msx.h"
#include "../sms/sms.h"
#include "../z80/z80_snapshot.h"

bool colecoSnapshot(SnapIO &io)
{
  if (!coleco::ram || !msx::vram || !coleco::bios || coleco::biosLen <= 0) {   // no BIOS: nothing runs
    if (io.loading) io.reject("ColecoVision is not running"); else io.fail("ColecoVision is not running");
    return false;
  }

  // ---- identity -----------------------------------------------------------------------------------
  int32_t biosLen = coleco::biosLen;
  uint32_t biosSum = snapRomSum(coleco::bios, coleco::biosLen);
  int32_t cartLen = coleco::rom ? coleco::romLen : 0;
  uint32_t cartSum = snapRomSum(coleco::rom, cartLen);
  const int32_t biosLen0 = biosLen, cartLen0 = cartLen;
  const uint32_t biosSum0 = biosSum, cartSum0 = cartSum;

  io.tag("COL ");
  io.val(biosLen); io.val(biosSum);
  io.val(cartLen); io.val(cartSum);
  if (!io.ok) return false;
  if (io.loading) {
    if (biosLen != biosLen0 || biosSum != biosSum0) { io.reject("Snapshot uses another ColecoVision BIOS"); return false; }
    if (cartLen != cartLen0 || cartSum != cartSum0) {
      io.reject(cartLen ? "Load the same cartridge first" : "Snapshot has no cartridge - eject it first");
      return false;
    }
  }

  // ---- machine --------------------------------------------------------------------------------------
  z80Snapshot(io, coleco::cpu);
  io.tag("RAM ");
  io.bytes(coleco::ram, coleco::RAM_SIZE);
  io.tag("VRAM");
  io.bytes(msx::vram, coleco::VRAM_SIZE);
  msx::vdpSnapshot(io);
  sms::psgSnapshot(io);
  coleco::machineSnapshot(io);
  if (!io.ok) return false;

  if (io.loading) {
    colecoCancelReset();           // a reset queued before the load must not wipe it on resume
    msx::vdpRedraw();              // the restored picture, shown when the settings window closes
    coleco::frameReady = true;     //   (closing it sets clearScr -> full repaint)
  }
  return io.ok;
}

#endif  // BOARD_HAS_COLECO_CORE
