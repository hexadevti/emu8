// This ColecoVision translation unit is compiled out entirely on boards that clear
// BOARD_HAS_COLECO_CORE (it needs both the MSX VDP and the SMS PSG -- see board.h).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro.
#include "../../emu.h"
#if BOARD_HAS_COLECO_CORE

// coleco_globals.cpp - definitions of the namespace-coleco shared state declared in coleco.h.
// (Kept Arduino-free so it also links into host/coleco_host.cpp.)

#include "coleco.h"

namespace coleco {
  Z80            cpu;
  uint8_t*       ram        = nullptr;
  const uint8_t* bios       = nullptr;
  int            biosLen    = 0;
  const uint8_t* rom        = nullptr;
  int            romLen     = 0;
  volatile bool  frameReady = false;
}

#endif  // BOARD_HAS_COLECO_CORE
