#include "../../emu.h"
#include "atari.h"
#include "atari_snap.h"
#include "../shared/snapshot.h"

// Atari 2600 save state (see src/shared/snapshot.h). Taken with the 6507 parked at an instruction
// boundary (cpuLoop's `while (paused)`, or the no-cart idle), so no instruction or WSYNC drain is
// in flight. Stream layout -- all fixed-width, little-endian, ints as int32, bools as one byte:
//
//   "A26C" cart id   : size u32, scheme u8 (0 raw, 1 F8, 2 F6, 3 F4), Superchip u8, FNV-1a u32 of
//                      the ROM. The ROM itself is NOT saved: on load a different (or no) cart is
//                      rejected before anything changes.
//   "CPU " 6507      : PC u16, S, A, X, Y, P
//   "RIOT" 6532      : 128B RAM, OUTA DDRA OUTB DDRB, timer value u8 / interval u16 / cycle acc i32,
//                      INSTAT, then the input latches SWCHA SWCHB INPT4 INPT5
//   "TIA " video     : beam (colorClock, scanline, outRow, spanX, VSYNC, VBLANK), colours, CTRLPF,
//                      PF0-2, GRP0/1 new+old, player/missile/ball positions, NUSIZ/REFP/VDEL/ENAM/
//                      RESMP/ENABL, HM* motion, 8 collision latches, then the framebuffer rows the
//                      current field has drawn so far (0..outRow), so the field finishes intact
//   "AUD " audio     : per voice AUDC AUDF AUDV, noise flag, LFSR length/state, phase, step
//   "CART" banking   : bank number i32 (rebuilt into the window by cartRead), Superchip 128B RAM
//                      when the cart has one

bool atariSnapshot(SnapIO &io)
{
  io.tag("A26C");
  if (!atari::cartSnapCheck(io)) return false;
  io.tag("CPU ");
  if (io.loading && !io.ok) { io.rejected = true; return false; }   // nothing overwritten yet
  atari::cpuSnap(io);
  io.tag("RIOT");
  atari::riotSnap(io);
  io.tag("TIA ");
  atari::tiaSnap(io);
  io.tag("AUD ");
  atari::audioSnap(io);
  io.tag("CART");
  atari::cartSnap(io);

  if (io.loading && io.ok) {
    atari::atariResetReq = false;      // a ROM picked in this pause must not reset the loaded state
    atari::wsyncStall = false;
    // Drop a completed field still queued from before the load (it would flash the old picture);
    // the renderer shows the next field the loaded state completes (~17ms after resume). We run on
    // the render task, which owns the 1 -> 0 transition.
    if (atari::frontBuf) { __sync_synchronize(); atari::frontState = 0; }
  }
  return io.ok;
}
