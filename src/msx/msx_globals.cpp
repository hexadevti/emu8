// msx_globals.cpp - definitions of the namespace-msx shared state declared in msx.h.
// (Kept Arduino-free so it also links into host/msx_host.cpp.)

#include "msx.h"

namespace msx {
  Z80      cpu;
  uint8_t* ram         = nullptr;   // 64 KB work RAM (slot 3)
  uint8_t* bios        = nullptr;   // BIOS ROM (slot 0)
  int      biosLen     = 0;
  uint8_t* vram        = nullptr;   // 16 KB VDP RAM
  uint8_t* framebuffer = nullptr;   // 256x192 indexed (sharedBigBuf on device)
  bool     biosIsCbios = false;
  volatile bool frameReady = false;   // set by core 1 when a frame is rendered, cleared by core 0 after display
}
