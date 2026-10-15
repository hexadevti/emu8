// z80_snapshot.h - save-state helpers shared by the Z80 machines (MSX, SMS, ColecoVision, ZX 48K).
// See src/shared/snapshot.h for SnapIO and the contract (one symmetric function per machine, same
// field order both ways, nothing overwritten before the identity check).
//
// Z80 record ("Z80 " tag, 41 bytes):
//   A F B C D E H L, A' F' B' C' D' E' H' L', IXH IXL IYH IYL, I R R7   (u8 each)
//   SP PC MEMPTR (u16), IFF1 IFF2 (u8 0/1), IM (u8), halted (u8), eiPending (i32), eiBlock (u8),
//   cycles (u32)
// The rd/wr/in/out hooks are pointers and are NOT saved: every machine wires them at setup and the
// frame loops use their own Z80Core<Bus> anyway.
//
// The chip helpers live next to the chips (they reach file-static state):
//   msx::vdpSnapshot   TMS9918A   (msx_vdp.cpp; MSX and ColecoVision)
//   msx::psgSnapshot   AY-3-8910  (msx_psg.cpp)
//   sms::vdpSnapshot   315-5124   (sms_vdp.cpp)
//   sms::psgSnapshot   SN76489    (sms_psg.cpp; SMS and ColecoVision)
#pragma once
#include "z80.h"
#include "../shared/snapshot.h"

// An int / enum through a fixed 32-bit field, so the file is the same on x64 and the device.
inline void snapInt(SnapIO &io, int &v)
{
  int32_t t = (int32_t)v;
  io.val(t);
  if (io.loading) v = (int)t;
}

// Identity of a ROM image that is not saved (cartridge, BIOS): its length and an FNV-1a hash of
// eight 512-byte samples spread over it (the whole image when it is 4K or less). Cheap enough to
// read through flash XIP / PSRAM on every save and load.
inline uint32_t snapRomSum(const uint8_t *p, int len)
{
  uint32_t h = 2166136261u ^ (uint32_t)len;
  if (!p || len <= 0) return h;
  auto mix = [&](int off, int n) { for (int i = 0; i < n; i++) { h ^= p[off + i]; h *= 16777619u; } };
  if (len <= 4096) { mix(0, len); return h; }
  for (int k = 0; k < 8; k++) {
    int off = (int)((int64_t)(len - 512) * k / 7);
    mix(off, 512);
  }
  return h;
}

inline void z80Snapshot(SnapIO &io, Z80 &c)
{
  io.tag("Z80 ");
  io.val(c.A);  io.val(c.F);  io.val(c.B);  io.val(c.C);  io.val(c.D);  io.val(c.E);  io.val(c.H);  io.val(c.L);
  io.val(c.A_); io.val(c.F_); io.val(c.B_); io.val(c.C_); io.val(c.D_); io.val(c.E_); io.val(c.H_); io.val(c.L_);
  io.val(c.IXH); io.val(c.IXL); io.val(c.IYH); io.val(c.IYL);
  io.val(c.I); io.val(c.R); io.val(c.R7);
  io.val(c.SP); io.val(c.PC); io.val(c.MEMPTR);
  io.flag(c.IFF1); io.flag(c.IFF2);
  io.val(c.IM);
  io.flag(c.halted);
  snapInt(io, c.eiPending);
  io.flag(c._eiBlock);
  io.val(c.cycles);
}
