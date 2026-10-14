// sms_cart.h - cartridge loader helper for the SMS core. The mapper read/write entry points live in
// namespace sms (declared in sms.h); this header just adds the load helper used by both the device
// loader (smsLoadSelected from SD) and the host harness.
#pragma once
#include <stdint.h>

// Point the SMS core at an in-memory ROM image. The pointer must stay valid (referenced live; on the
// device the image is resident in PSRAM). Detects bank count and resets the Sega mapper registers.
void smsCartLoadImage(const uint8_t* data, int len);
