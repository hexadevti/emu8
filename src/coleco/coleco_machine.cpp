// This ColecoVision translation unit is compiled out entirely on boards that clear
// BOARD_HAS_COLECO_CORE (it needs both the MSX VDP and the SMS PSG -- see board.h).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro.
#include "../../emu.h"
#if BOARD_HAS_COLECO_CORE

// coleco_machine.cpp - ColecoVision memory map, I/O decode, controllers and the per-frame run loop.
// Arduino-free (links into the off-device harness). See coleco.h.
//
// Memory:
//   0x0000-0x1FFF  8K BIOS
//   0x2000-0x5FFF  expansion port (nothing fitted -> open bus)
//   0x6000-0x7FFF  1K RAM, mirrored 8 times
//   0x8000-0xFFFF  cartridge. Up to 32K maps linearly. Larger images are MegaCarts: 0x8000-0xBFFF
//                  is fixed to the LAST 16K bank (it holds the header), and 0xC000-0xFFFF shows the
//                  bank picked by the low address bits of any access to 0xFFC0-0xFFFF.
// I/O (decoded on A7-A5; A0 picks the register inside a chip, A1 the controller):
//   0x80-0x9F  write: controllers -> keypad mode
//   0xA0-0xBF  VDP: even = data, odd = control / status
//   0xC0-0xDF  write: controllers -> joystick mode
//   0xE0-0xFF  write: SN76489;  read: controller 1 (A1=0) / controller 2 (A1=1)

#include "coleco.h"
#include "../msx/msx.h"   // the TMS9918A (msx::vdp*) and its vram/framebuffer pointers
#include "../sms/sms.h"   // the SN76489 (sms::psgWrite)

namespace coleco {

// ---- cartridge ------------------------------------------------------------------------------------
static int      megaBanks = 0;       // 16K banks in a MegaCart image; 0 = plain cartridge (<= 32K)
static uint32_t megaFixed = 0;       // ROM offset of the fixed bank at 0x8000 (the last one)
static uint32_t megaBase  = 0;       // ROM offset of the bank currently at 0xC000

void cartSetImage(const uint8_t* data, int len) {
  rom = data; romLen = len;
  megaBanks = (len > 0x8000) ? (len + 0x3FFF) / 0x4000 : 0;
  megaFixed = megaBanks ? (uint32_t)(megaBanks - 1) * 0x4000 : 0;
  megaBase  = 0;
}

static inline void megaSelect(uint16_t a) {
  if (megaBanks) megaBase = (uint32_t)((a & 0x3F) % megaBanks) * 0x4000;
}

static inline uint8_t cartRead(uint16_t a) {
  uint32_t off;
  if (!megaBanks)       off = a - 0x8000;
  else if (a < 0xC000)  off = megaFixed + (a - 0x8000);
  else {
    if (a >= 0xFFC0) megaSelect(a);                       // the access itself switches the bank
    off = megaBase + (a - 0xC000);
  }
  return (rom && off < (uint32_t)romLen) ? rom[off] : 0xFF;
}

// ---- memory ---------------------------------------------------------------------------------------
uint8_t memRead8(uint16_t a) {
  if (a < 0x2000) return (a < biosLen) ? bios[a] : 0xFF;
  if (a < 0x6000) return 0xFF;
  if (a < 0x8000) return ram[a & (RAM_SIZE - 1)];
  return cartRead(a);
}

void memWrite8(uint16_t a, uint8_t v) {
  if (a >= 0x6000 && a < 0x8000) { ram[a & (RAM_SIZE - 1)] = v; return; }
  if (a >= 0xFFC0) megaSelect(a);                         // some MegaCart code switches with a write
}

// ---- controllers ----------------------------------------------------------------------------------
// Keypad codes as the hardware returns them in bits 0-3 (0x0F = no key), indexed 0-9, '*', '#'.
static const uint8_t KEYPAD_CODE[12] = { 0x0A, 0x0D, 0x07, 0x0C, 0x02, 0x03, 0x0E, 0x05, 0x01, 0x0B, 0x09, 0x06 };

static uint8_t padMask[2] = { 0xFF, 0xFF };   // active-LOW: b0 up b1 down b2 left b3 right b4 fireL b5 fireR
static int     padKey[2]  = { -1, -1 };
static bool    keypadMode = false;            // selected by writes to 0x80 (keypad) / 0xC0 (joystick)

void setInput(int pad, uint8_t mask) { padMask[pad & 1] = mask; }
void setKeypad(int pad, int key)     { padKey[pad & 1] = (key >= 0 && key < 12) ? key : -1; }

// Unused bits read high. Joystick mode: b0 up, b1 right, b2 down, b3 left, b6 left fire.
// Keypad mode: b0-3 keypad code, b6 right fire.
static uint8_t readPad(int pad) {
  uint8_t m = padMask[pad], r = 0xFF;
  if (keypadMode) {
    r = (uint8_t)((r & 0xF0) | (padKey[pad] >= 0 ? KEYPAD_CODE[padKey[pad]] : 0x0F));
    if (!(m & 0x20)) r &= ~0x40;
  } else {
    if (!(m & 0x01)) r &= ~0x01;
    if (!(m & 0x08)) r &= ~0x02;
    if (!(m & 0x02)) r &= ~0x04;
    if (!(m & 0x04)) r &= ~0x08;
    if (!(m & 0x10)) r &= ~0x40;
  }
  return r;
}

// ---- I/O ------------------------------------------------------------------------------------------
uint8_t ioIn(uint16_t port) {
  switch (port & 0xE0) {
    case 0xA0: return (port & 1) ? msx::vdpReadStatus() : msx::vdpReadData();
    case 0xE0: return readPad((port >> 1) & 1);
  }
  return 0xFF;
}

void ioOut(uint16_t port, uint8_t v) {
  switch (port & 0xE0) {
    case 0x80: keypadMode = true;  break;
    case 0xA0: if (port & 1) msx::vdpWriteCtrl(v); else msx::vdpWriteData(v); break;
    case 0xC0: keypadMode = false; break;
    case 0xE0: sms::psgWrite(v);   break;
  }
}

void machineWire() {
  cpu.rd  = memRead8;
  cpu.wr  = memWrite8;
  cpu.in  = ioIn;
  cpu.out = ioOut;
}

static bool nmiLine = false;          // last sampled level of the VDP interrupt output

void machineReset() {
  msx::vdpReset();
  sms::psgReset();
  keypadMode = false;
  nmiLine = false;
  megaBase = 0;
  cpu.reset();                        // PC = 0x0000 -> BIOS (title screen, then the cartridge)
}

// ---- per-frame execution --------------------------------------------------------------------------
// NTSC: Z80 @ 3.579545 MHz, 262 lines of 228 T-states (59736/frame, ~59.92 Hz) -- the same timing as
// the SMS/MSX loops. The VDP's INT pin drives the Z80 NMI, which is EDGE-triggered: a new NMI only
// fires when the line goes from inactive to active (VBlank flag set while R1 bit5 enables it, or the
// enable bit set while the flag is already pending). Reading the status register drops the line. The
// level is sampled once per scanline, which is well inside the VBlank window games expect.
void runFrame() {
  const int LINES = 262;
  const uint64_t TPL = 228;
  for (int line = 0; line < LINES; line++) {
    uint64_t target = cpu.cycles + TPL;
    while (cpu.cycles < target) cpu.step();
    if (line == VDP_H - 1) {
      // End of the active display: render race-free (only once core 0 has shown the last frame),
      // then raise the VBlank flag. Sprite collision / 5th-sprite status come out of the render.
      if (!frameReady) { msx::vdpRender(); frameReady = true; }
      msx::vdpEndFrame();
    }
    bool level = msx::vdpIrqActive();
    if (level && !nmiLine) cpu.nmi();
    nmiLine = level;
  }
}

} // namespace coleco

#endif  // BOARD_HAS_COLECO_CORE
