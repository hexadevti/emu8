// This ZX Spectrum translation unit is compiled out entirely on boards that clear
// BOARD_HAS_ZX_CORE (see board.h). emu.h must be included FIRST because it is what pulls in
// board.h and defines the macro.
#include "../../emu.h"
#if BOARD_HAS_ZX_CORE

// zx_snapshot.cpp - ZX Spectrum 48K save state (see src/shared/snapshot.h). Stream after the header:
//   "ZX  " identity: ROM length + hash. Read into locals and compared before anything is touched
//          (the ROM is not in the file).
//   "Z80 " CPU (z80_snapshot.h)
//   "RAM " 48 KB (0x4000-0xFFFF)
//   "ULA " border, port 0xFE, frame start / counter, tape position (zx::machineSnapshot)
// A loaded .TAP / .TZX stays mounted as it is (the image is not saved). The Z80 parks at a frame
// boundary while the settings window is open.

#include "zx.h"
#include "../z80/z80_snapshot.h"

bool zxSnapshot(SnapIO &io)
{
  if (!zx::ram || !zx::rom || zx::romLen <= 0 || !zx::screen || !zx::borderLine) {   // no ROM: nothing runs
    if (io.loading) io.reject("ZX Spectrum is not running"); else io.fail("ZX Spectrum is not running");
    return false;
  }

  // ---- identity -----------------------------------------------------------------------------------
  int32_t romLen = zx::romLen;
  uint32_t romSum = snapRomSum(zx::rom, zx::romLen);
  const int32_t romLen0 = romLen;
  const uint32_t romSum0 = romSum;

  io.tag("ZX  ");
  io.val(romLen); io.val(romSum);
  if (!io.ok) return false;
  if (io.loading && (romLen != romLen0 || romSum != romSum0)) { io.reject("Snapshot uses another ZX ROM"); return false; }

  // ---- machine --------------------------------------------------------------------------------------
  z80Snapshot(io, zx::cpu);
  io.tag("RAM ");
  io.bytes(zx::ram, zx::RAM_SIZE);
  zx::machineSnapshot(io);
  if (!io.ok) return false;

  if (io.loading) {
    zxCancelReset();               // a reset queued before the load (snapshot unload) must not wipe it
    zx::snapshotRedraw();          // the restored picture, shown when the settings window closes
  }
  return io.ok;
}

#endif  // BOARD_HAS_ZX_CORE
