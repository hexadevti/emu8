// This MSX translation unit is compiled out entirely on boards that clear
// BOARD_HAS_MSX_CORE (currently none -- see board.h; it is on for every board).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro;
// the guard below then empties the file, keeping the Z80 core's static RAM out of the image.
#include "../../emu.h"
#if BOARD_HAS_MSX_CORE

// msx_globals.cpp - definitions of the namespace-msx shared state declared in msx.h.
// (Kept Arduino-free so it also links into host/msx_host.cpp.)

#include "msx.h"

namespace msx {
  Z80      cpu;
  uint8_t* ram         = nullptr;   // 64 KB work RAM (slot 3)
  const uint8_t* bios  = nullptr;   // BIOS ROM (slot 0)
  int      biosLen     = 0;
  uint8_t* vram        = nullptr;   // 16 KB VDP RAM
  uint8_t* framebuffer = nullptr;   // 256x192 indexed (sharedBigBuf on device)
  bool     biosIsCbios = false;
  volatile bool frameReady = false;   // set by core 1 when a frame is rendered, cleared by core 0 after display
}

#endif  // BOARD_HAS_MSX_CORE
