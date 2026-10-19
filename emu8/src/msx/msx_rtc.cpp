// This MSX translation unit is compiled out on boards without the MSX core or without MSX2 support.
#include "../../emu.h"
#if BOARD_HAS_MSX_CORE && BOARD_HAS_MSX2

// msx_rtc.cpp - Ricoh RP5C01 real-time clock of the MSX2 (ports $B4 register select, $B5 data).
// Four blocks of 13 nibbles picked by the mode register (13): block 0 the time (read from the host
// clock; writes are ignored), block 1 alarm / 12-24h / leap year, blocks 2-3 the battery RAM the
// BIOS keeps its settings in (screen width, colours, ...). The battery RAM is not saved: it starts
// with the BIOS defaults below every power-on.

#include "msx.h"
#include <string.h>
#include <time.h>

namespace msx {

static uint8_t rtcReg;          // selected register 0-15
static uint8_t rtcMode;         // register 13
static uint8_t rtcRam[4][13];   // blocks 1-3 (block 0 is the clock)

void rtcReset() {
  rtcReg = 0; rtcMode = 0;
  memset(rtcRam, 0, sizeof(rtcRam));
  rtcRam[1][10] = 1;                                // 24-hour clock
  uint8_t* b2 = rtcRam[2];
  b2[0] = 0x0A;                                     // ID: the settings below are valid
  b2[4] = 0x08; b2[5] = 0x02;                       // WIDTH 40 (0x28) in SCREEN 0
  b2[6] = 0x0F; b2[7] = 0x04; b2[8] = 0x04;         // COLOR 15,4,4
}

void rtcWriteAddr(uint8_t v) { rtcReg = v & 0x0F; }

void rtcWriteData(uint8_t v) {
  v &= 0x0F;
  if (rtcReg == 13) { rtcMode = v; return; }
  if (rtcReg >= 13) return;                         // test / reset registers
  const int blk = rtcMode & 3;
  if (blk) rtcRam[blk][rtcReg] = v;
}

static uint8_t clockNibble(int reg) {
  time_t t = time(nullptr);
  struct tm tmv;
  struct tm* tp = localtime(&t);
  if (tp && tp->tm_year >= 100) tmv = *tp;
  else { memset(&tmv, 0, sizeof(tmv)); tmv.tm_year = 126; tmv.tm_mday = 1; tmv.tm_wday = 4; }  // 2026-01-01
  const int yr = (tmv.tm_year + 1900 - 1980) % 100;
  switch (reg) {
    case 0:  return tmv.tm_sec % 10;
    case 1:  return tmv.tm_sec / 10;
    case 2:  return tmv.tm_min % 10;
    case 3:  return tmv.tm_min / 10;
    case 4:  return tmv.tm_hour % 10;
    case 5:  return tmv.tm_hour / 10;
    case 6:  return tmv.tm_wday;
    case 7:  return tmv.tm_mday % 10;
    case 8:  return tmv.tm_mday / 10;
    case 9:  return (tmv.tm_mon + 1) % 10;
    case 10: return (tmv.tm_mon + 1) / 10;
    case 11: return yr % 10;
    default: return yr / 10;
  }
}

uint8_t rtcReadData() {
  if (rtcReg == 13) return (uint8_t)(0xF0 | rtcMode);
  if (rtcReg > 13) return 0xFF;
  const int blk = rtcMode & 3;
  return (uint8_t)(0xF0 | (blk ? rtcRam[blk][rtcReg] : clockNibble(rtcReg)));
}

} // namespace msx

#endif  // BOARD_HAS_MSX_CORE && BOARD_HAS_MSX2
