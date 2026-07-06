// sms_globals.cpp - definitions of the namespace-sms shared state declared in sms.h.
// (Kept Arduino-free so it also links into host/sms_host.cpp.)

#include "sms.h"

namespace sms {
  Z80      cpu;
  uint8_t* ram         = nullptr;   // 8 KB work RAM (0xC000-0xDFFF, mirror 0xE000-0xFFFF)
  uint8_t* rom         = nullptr;   // cartridge ROM image (resident)
  int      romLen      = 0;
  uint8_t* vram        = nullptr;   // 16 KB VDP RAM
  uint8_t* framebuffer = nullptr;   // 256x192 indexed 0..31 (sharedBigBuf on device)
  volatile bool frameReady = false; // set by core 1 when a frame is rendered, cleared by core 0 after display
}
