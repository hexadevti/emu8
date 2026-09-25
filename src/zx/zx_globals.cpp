// emu.h must be included FIRST because it is what pulls in board.h and defines the macro.
#include "../../emu.h"
#if BOARD_HAS_ZX_CORE

// zx_globals.cpp - definitions of the namespace-zx shared state declared in zx.h.

#include "zx.h"

namespace zx {
  Z80            cpu;
  uint8_t*       ram        = nullptr;
  const uint8_t* rom        = nullptr;
  int            romLen     = 0;
  uint8_t*       screen     = nullptr;
  uint8_t*       borderLine = nullptr;
  bool           flashPhase = false;
  volatile bool  frameReady = false;
  uint8_t*       beepRing   = nullptr;

  const uint8_t* tapeData      = nullptr;
  TapeBlock*     tapeBlocks    = nullptr;
  int            tapeCount     = 0;
  int            tapeMaxBlocks = 0;
  int            tapePos       = 0;
}

#endif  // BOARD_HAS_ZX_CORE
