// msx_cart.h - cartridge loader / mapper helper for the MSX1 core. The slot read/write entry points
// live in namespace msx (declared in msx.h); this header adds the load helpers used by both the
// device loader (msxLoadSelected from SD) and the host harness.
#pragma once
#include <stdint.h>

// Load a cartridge image already in memory into a slot (1 or 2). Detects the mapper from size +
// signature. The pointer must stay valid (it is referenced live; for big ROMs the device path
// streams from SD instead - see msx_cart.cpp).
void msxCartLoadImage(int slot, const uint8_t* data, int len);
void msxCartEject(int slot);
