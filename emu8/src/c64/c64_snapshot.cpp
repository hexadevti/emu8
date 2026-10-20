#include "../../emu.h"
#include "c64.h"
#include "../shared/snapshot.h"

// C64 snapshots (save states; the file and its header are snapshot.cpp's, see snapshot.h). One
// function moves the whole machine both ways, in this order:
//
//   "C64 "  u16 format version, u32 RAM size                          -- identity, checked first
//   "CART"  cartridge identity (c64CartSnapCheck): none, or type + CHIP table + ROM samples
//   "CPU "  PC, lastPC, S, A, X, Y, P, cpuLoop's raster-cycle accumulator + IRQ/NMI edge state,
//           the processor port ($01 latch; $00 is in RAM), a pending reset request
//   "RAM "  the 64K RAM
//   "CRAM"  the 1K colour RAM
//   "CIA "  both CIAs: ports, data directions, timers + latches, control, ICR data / mask, TOD
//   "VIC "  $D000-$D03F, the $D011/$D012 latches, the bank, the raster line and the border /
//           badline / character-row state of the frame in progress
//   "SID "  the registers, each voice's oscillator / noise / envelope, the filter integrators
//   "CRTS"  the cartridge's banking state (only with a cartridge)
//
// Multi-byte fields are the machine's own little-endian; every field has a fixed width, so the
// desktop build and the device read each other's files. Not saved: the ROMs (BASIC / KERNAL /
// CHARGEN come from /roms), the cartridge ROM (the same .crt must be mounted; RAM-buffer builds do
// save the two mapped 8K windows), the mounted .d64 (LOAD is trapped and keeps no state between
// calls), the keys held on the host (released on load) and the video records / panel caches
// (dropped on load; the next whole frame is drawn and sent in full).
//
// Taken with cpuLoop idling in its paused spin -- at an instruction boundary, anywhere in a frame:
// the raster position and the cycles towards the next line go with it, so the frame carries on.

// 2: CIA1 Timer B, CIA2 port B and both TOD clocks (a version 1 file still loads; those reset).
#define C64_SNAP_VERSION 2

bool c64Snapshot(SnapIO &io)
{
  // ---- identity: on load, nothing is changed until all of this has matched ----
  io.tag("C64 ");
  uint16_t ver = C64_SNAP_VERSION;
  uint32_t ramSize = 0x10000;
  io.val(ver);
  io.val(ramSize);
  if (io.ok && io.loading && ((ver != 1 && ver != C64_SNAP_VERSION) || ramSize != 0x10000))
    io.reject("Snapshot from another C64 version");
  if (io.ok && (!c64::ram || !c64::colormap || !c64::cpuWaitParked(500))) {
    if (io.loading) io.reject("C64 not running - try again");
    else            io.fail("C64 not running - try again");
  }
  if (io.ok) {
    io.tag("CART");
    c64CartSnapCheck(io);
  }
  if (!io.ok) {
    if (io.loading) io.rejected = true;     // only locals were read: the machine is untouched
    return false;
  }

  // ---- CPU ----
  io.tag("CPU ");
  c64::cpuSnapshot(io);
  uint8_t port = c64::register1;
  bool resetReq = c64::c64ResetReq;
  io.val(port);
  io.flag(resetReq);

  // ---- memory (straight from / into the live buffers) ----
  io.tag("RAM ");
  io.bytes(c64::ram, 0x10000);
  io.tag("CRAM");
  io.bytes(c64::colormap, 1024);

  // ---- chips ----
  io.tag("CIA ");
  c64::ciaSnapshot(io, ver);
  io.tag("VIC ");
  c64::vicSnapshot(io);
  io.tag("SID ");
  c64SidSnapshot(io);
  if (c64::cartActive) {
    io.tag("CRTS");
    c64CartSnapState(io);
  }

  if (io.loading && io.ok) {
    c64::register1 = port;                    // $01: rebuild the banking from the latch + DDR ($00)
    c64::decodeRegister1((port | ~c64::ram[0]) & 7);
    c64::c64ResetReq = resetReq;
    c64::kbReset();                           // nothing held: the host keys are not the snapshot's
    ::c64AutoloadPending = false;             // a queued boot load must not land on top of it
    c64VideoInvalidate();
  }
  return io.ok;
}
