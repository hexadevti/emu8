// msx_machine.cpp - MSX1 machine: primary-slot memory map, Z80<->VDP/PSG/PPI I/O routing, and the
// per-frame run loop (Z80 time + VDP VBlank interrupt). Arduino-free so it also links into
// host/msx_host.cpp. See msx.h.

#include "msx.h"
#include "msx_disk.h"
#include <string.h>
#if defined(BOARD_DESKTOP)
#include "../desktop/debug_bridge.h"   // desktop debugger: pause/step/breakpoints/watchpoints/heat (no-op on device)
#endif

namespace msx {

// ---- memory: 4 x 16 KB pages, each mapped to one of four primary slots by the PPI ($A8) ---------
// Standard MSX1 layout: slot 0 = BIOS (pages 0-1), slot 1/2 = cartridge, slot 3 = 64 KB RAM.
uint8_t memRead8(uint16_t a) {
#if defined(BOARD_DESKTOP)
  dbgBusTouch(a, DBG_HEAT_R);              // heat map (reads) + read watchpoints
#endif
  int slot = ppiPageSlot(a >> 14);
  switch (slot) {
    case 0:  return (a < (uint16_t)biosLen) ? bios[a] : 0xFF;   // BIOS ROM (0xFF above its size)
    case 1:  return cartRead(1, a);                             // cartridge
    case 2:  return diskRead(a);                                // disk-interface ROM + .dsk window
    default: return ram[a];                                     // slot 3 = RAM
  }
}
void memWrite8(uint16_t a, uint8_t v) {
#if defined(BOARD_DESKTOP)
  dbgBusTouch(a, DBG_HEAT_W);              // heat map (writes) + write watchpoints
#endif
  int slot = ppiPageSlot(a >> 14);
  switch (slot) {
    case 0:  break;                       // BIOS ROM: writes ignored
    case 1:  cartWrite(1, a, v); break;    // mapper bank-select (and any cart RAM)
    case 2:  diskWrite(a, v); break;       // disk window bank-select
    default: ram[a] = v;                   // slot 3 = RAM
  }
}

// ---- I/O: MSX decodes only the low 8 port bits -------------------------------------------------
uint8_t ioIn(uint16_t port) {
  switch (port & 0xFF) {
    case 0x98: return vdpReadData();
    case 0x99: return vdpReadStatus();
    case 0xA2: return psgReadData();
    case 0xA8: case 0xA9: case 0xAA: return ppiRead(port & 0xFF);
    default:   return 0xFF;
  }
}
void ioOut(uint16_t port, uint8_t v) {
  switch (port & 0xFF) {
    case 0x98: vdpWriteData(v); break;
    case 0x99: vdpWriteCtrl(v); break;
    case 0xA0: psgWriteAddr(v); break;
    case 0xA1: psgWriteData(v); break;
    case 0xA8: case 0xAA: case 0xAB: ppiWrite(port & 0xFF, v); break;
    default:   break;
  }
}

void machineWire() {
  cpu.rd  = memRead8;
  cpu.wr  = memWrite8;
  cpu.in  = ioIn;
  cpu.out = ioOut;
}

void machineReset() {
  vdpReset();
  ppiReset();
  psgReset();
  kbReset();
  cpu.reset();          // PC = 0x0000 -> BIOS entry
}

// ---- per-frame execution -----------------------------------------------------------------------
// NTSC MSX1: 3.579545 MHz / 59.92 Hz ~= 59736 T-states per frame. step() advances cpu.cycles
// (handling HALT internally), so this loop terminates even when the CPU is halted waiting for the
// VBlank interrupt that we raise at the end of the frame.
void runFrame() {
  const uint64_t TPF = 59736;
  uint64_t target = cpu.cycles + TPF;
  while (cpu.cycles < target) {
#if defined(BOARD_DESKTOP)
    // Desktop debugger: break at the upcoming PC (breakpoint / run-to-cursor / step-over / step-out).
    if (dbgBpShouldBreak(cpu.PC)) paused = true;                                                      // breakpoint
    if (g_dbgRunToPC >= 0 && cpu.PC == (uint16_t)g_dbgRunToPC) { paused = true; g_dbgRunToPC = -1; }  // run-to / step-over
    if (g_dbgRunUntilSP >= 0 && cpu.SP > (uint16_t)g_dbgRunUntilSP) { paused = true; g_dbgRunUntilSP = -1; } // step-out
    if (paused) {
      g_dbgBreakArmed = true;                       // re-arm so the next Resume passes this PC once
      if (dbgStepReq > 0) dbgStepReq--;             // single-step: fall through and run exactly one instruction
      else return;                                  // paused: leave the frame (msxLoop keeps the panel + last frame)
    } else {
      g_dbgBreakArmed = true;
    }
    dbgBusTouch(cpu.PC, DBG_HEAT_X);                 // heat map (executed opcode byte)
#endif
    cpu.step();
  }
  // Render on THIS core (the CPU core) so the VDP reads the sprite/VRAM tables race-free, at a
  // consistent point - the end of the active frame, before the VBlank ISR updates them for the
  // next one. Only render into the shared framebuffer once core 0 has finished DISPLAYING the
  // previous frame (frameReady == false), so the two cores never touch the buffer at the same time
  // -> no torn frames (the green-field squares over the white lines). The CPU steps + VBlank IRQ
  // still run every frame regardless, so emulation timing is unaffected.
  if (!frameReady) { vdpRender(); frameReady = true; }
  vdpEndFrame();
  if (vdpIrqActive()) cpu.irq(0xFF);    // MSX uses IM 1; the data-bus byte is ignored
}

} // namespace msx

// ===================== host harness entry points (off-device boot test) ==========================
#ifdef MSX_HOST_BOOT
#include <cstdio>
#include "msx_cart.h"

static uint8_t  hb_ram[0x10000];
static uint8_t  hb_vram[0x4000];
static uint8_t  hb_fb[msx::FB_SIZE];
static uint8_t  hb_bios[0x8000];
static uint8_t  hb_cart[0x20000];

void msxHostInit(const uint8_t* biosP, int bl, const uint8_t* cartP, int cl) {
  if (bl > (int)sizeof(hb_bios)) bl = sizeof(hb_bios);
  memcpy(hb_bios, biosP, bl);
  msx::bios = hb_bios; msx::biosLen = bl;
  msx::ram = hb_ram; msx::vram = hb_vram; msx::framebuffer = hb_fb;
  msx::biosIsCbios = false;
  memset(hb_ram, 0, sizeof(hb_ram));
  if (cartP && cl) { if (cl > (int)sizeof(hb_cart)) cl = sizeof(hb_cart); memcpy(hb_cart, cartP, cl); msxCartLoadImage(1, hb_cart, cl); }
  msx::machineWire();
  msx::machineReset();
}

void msxHostRunFrames(int frames) { for (int i = 0; i < frames; i++) msx::runFrame(); }

void msxHostDumpText() {
  // Read the VDP name table and print it as ASCII (MSX BASIC stores char codes there directly).
  int r0 = msx::vdpRegister(0), r1 = msx::vdpRegister(1), r2 = msx::vdpRegister(2);
  bool textMode = (r1 & 0x10) != 0;          // M1 = Text Mode 1 (40 cols); else Graphic 1 (32 cols)
  int cols = textMode ? 40 : 32;
  uint16_t base = (uint16_t)((r2 & 0x0F) << 10);
  printf("\n--- VDP name table (mode: %s, R0=%02X R1=%02X R2=%02X, base=%04X) ---\n",
         textMode ? "TEXT1 40x24" : "GRAPHIC1 32x24", r0, r1, r2, base);
  for (int row = 0; row < 24; row++) {
    char line[41];
    for (int c = 0; c < cols; c++) {
      uint8_t ch = msx::vram[(base + row * cols + c) & 0x3FFF];
      line[c] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
    line[cols] = 0;
    printf("|%s|\n", line);
  }
}
#endif
