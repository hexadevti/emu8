// sms_io.cpp - SMS I/O-port decode and the two controller ports. The SMS decodes I/O by address
// RANGE (not a single port like the MSX):
//   0x00-0x3F : write -> even $3E memory-control, odd $3F I/O-control (nationalisation / TH);
//               read  -> 0xFF
//   0x40-0x7F : write -> SN76489 PSG;  read -> even $7E V-counter, odd $7F H-counter
//   0x80-0xBF : even $BE VDP data, odd $BF VDP control/status
//   0xC0-0xFF : read  -> even $DC controller A, odd $DD controller B; writes ignored
// Controllers are active-LOW (a 0 bit = pressed). This file is Arduino-free (links into the harness).

#include "sms.h"

namespace sms {

static uint8_t memCtrl = 0;        // $3E
static uint8_t ioCtrl  = 0xFF;     // $3F (TH-pin direction/output for region detection)
static uint8_t pad1    = 0xFF;     // controller 1, active-LOW: b0 up b1 down b2 left b3 right b4 TL b5 TR
static uint8_t pad2    = 0xFF;     // controller 2, same bit order

void ioReset() { memCtrl = 0; ioCtrl = 0xFF; pad1 = 0xFF; pad2 = 0xFF; }
void setInput1(uint8_t mask) { pad1 = mask; }
void setInput2(uint8_t mask) { pad2 = mask; }

// Controller port A ($DC): P1 d-pad + 2 buttons in b0-5, P2 up/down in b6-7.
static uint8_t portA() { return (uint8_t)((pad1 & 0x3F) | ((pad2 & 0x03) << 6)); }
// Controller port B ($DD): P2 left/right/TL/TR in b0-3; b4 RESET (1=released); TH pins b6-7 read high.
static uint8_t portB() { return (uint8_t)(((pad2 >> 2) & 0x0F) | 0xF0); }

uint8_t ioIn(uint16_t port) {
  uint8_t p = (uint8_t)(port & 0xFF);
  if (p <= 0x3F) return 0xFF;
  if (p <= 0x7F) return (p & 1) ? vdpHCounter() : vdpVCounter();
  if (p <= 0xBF) return (p & 1) ? vdpReadStatus() : vdpReadData();
  return (p & 1) ? portB() : portA();
}

void ioOut(uint16_t port, uint8_t v) {
  uint8_t p = (uint8_t)(port & 0xFF);
  if (p <= 0x3F)      { if (p & 1) ioCtrl = v; else memCtrl = v; return; }
  if (p <= 0x7F)      { psgWrite(v); return; }
  if (p <= 0xBF)      { if (p & 1) vdpWriteCtrl(v); else vdpWriteData(v); return; }
  // 0xC0-0xFF: controller ports are read-only
}

} // namespace sms
