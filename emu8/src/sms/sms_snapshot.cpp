// This SMS translation unit is compiled out entirely on boards that clear
// BOARD_HAS_SMS_CORE (currently none -- see board.h; it is on for every board).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro;
// the guard below then empties the file, keeping the Z80 core's static RAM out of the image.
#include "../../emu.h"
#if BOARD_HAS_SMS_CORE

// sms_snapshot.cpp - Master System save state (see src/shared/snapshot.h). Stream after the header:
//   "SMS " identity: cartridge length + sample hash, cart RAM present (u8). Read into locals and
//          compared before anything is touched (the ROM is not in the file). A snapshot with cart
//          RAM allocates it here through the same lazy path a $FFFC write uses; no heap -> reject.
//   "Z80 " CPU (z80_snapshot.h)
//   "WRAM" 8 KB work RAM     "VRAM" 16 KB     "CRAM" 32 KB cart RAM (only when present)
//   "SVDP" VDP registers/CRAM/latches   "SN  " PSG   "MAPR" bank registers   "SIO " $3E/$3F
//   "PAUS" the PAUSE NMI latched for the next frame boundary
// The Z80 parks at a frame boundary while the settings window is open; runFrame() always starts at
// line 0, so there is no frame-position state. The page pointers are rebuilt from the registers.

#include "sms.h"
#include "../z80/z80_snapshot.h"
#include <string.h>

bool smsSnapshot(SnapIO &io)
{
  if (!sms::ram || !sms::vram || !sms::rom || sms::romLen <= 0) {   // no cartridge: nothing runs
    if (io.loading) io.reject("Load the same cartridge first"); else io.fail("No cartridge running");
    return false;
  }

  // ---- identity -----------------------------------------------------------------------------------
  int32_t romLen = sms::romLen;
  uint32_t romSum = snapRomSum(sms::rom, sms::romLen);
  bool cartRam = sms::cartRamData() != nullptr;
  const int32_t romLen0 = romLen;
  const uint32_t romSum0 = romSum;

  io.tag("SMS ");
  io.val(romLen); io.val(romSum);
  io.flag(cartRam);
  if (!io.ok) return false;
  if (io.loading) {
    if (romLen != romLen0 || romSum != romSum0) { io.reject("Load the same cartridge first"); return false; }
    if (cartRam && !sms::cartRamEnsure()) { io.reject("No memory for the cartridge RAM"); return false; }
  }

  // ---- machine --------------------------------------------------------------------------------------
  z80Snapshot(io, sms::cpu);
  io.tag("WRAM");
  io.bytes(sms::ram, sms::WRAM_SIZE);
  io.tag("VRAM");
  io.bytes(sms::vram, sms::VRAM_SIZE);
  if (cartRam) {
    io.tag("CRAM");
    io.bytes(sms::cartRamData(), sms::cartRamSize());
  } else if (io.loading && sms::cartRamData()) {
    memset(sms::cartRamData(), 0, sms::cartRamSize());   // never mapped in the snapshot: as fresh
  }
  sms::vdpSnapshot(io);
  sms::psgSnapshot(io);
  sms::cartSnapshot(io);
  sms::ioSnapshot(io);
  io.tag("PAUS");
  sms::pauseSnapshot(io);
  if (!io.ok) return false;

  if (io.loading) {
    smsCancelReset();              // a reset queued before the load must not wipe it on resume
    sms::vdpRedraw();              // the restored picture, shown when the settings window closes
    sms::frameReady = true;        //   (closing it sets clearScr -> full repaint)
  }
  return io.ok;
}

#endif  // BOARD_HAS_SMS_CORE
