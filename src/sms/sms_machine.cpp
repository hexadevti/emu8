// sms_machine.cpp - SMS memory map (Sega mapper + 8 KB work RAM), Z80<->VDP/PSG/IO routing, and the
// per-frame run loop (per-scanline Z80 time + VDP line/VBlank interrupts). Arduino-free so it also
// links into host/sms_host.cpp. See sms.h.

#include "sms.h"
#include <string.h>

namespace sms {

// ---- memory: 0x0000-0xBFFF = cartridge (Sega mapper), 0xC000-0xFFFF = 8 KB work RAM (mirrored) -----
// The mapper registers $FFFC-$FFFF also occupy the top of work RAM, so a write there hits BOTH the RAM
// byte and the mapper (cartWrite); reads of $FFFC-$FFFF return the RAM byte.
uint8_t memRead8(uint16_t a) {
  if (a < 0xC000) return cartRead(a);
  return ram[a & 0x1FFF];
}
void memWrite8(uint16_t a, uint8_t v) {
  if (a < 0xC000) { cartWrite(a, v); return; }   // cart RAM at 0x8000 if enabled, else ignored (ROM)
  ram[a & 0x1FFF] = v;                            // work RAM + 0xE000 mirror
  if (a >= 0xFFFC) cartWrite(a, v);               // mapper registers live at the top of RAM
}

void machineWire() {
  cpu.rd  = memRead8;
  cpu.wr  = memWrite8;
  cpu.in  = ioIn;
  cpu.out = ioOut;
}

static bool pauseLatch = false;
void smsPause() { pauseLatch = true; }            // SMS PAUSE button -> NMI (edge-triggered)

void machineReset() {
  vdpReset();
  psgReset();
  ioReset();
  cartReset();
  pauseLatch = false;
  cpu.reset();          // PC = 0x0000 -> cartridge entry (no BIOS)
}

// ---- per-frame execution -----------------------------------------------------------------------
// NTSC SMS: Z80 @ 3.579545 MHz, 262 scanlines, ~59.92 Hz -> ~227.7 T-states/scanline. We run the CPU
// one scanline at a time so the VDP line-interrupt counter (split-screen status bars) and the VBlank
// interrupt land at the right rasters. The frame is rendered once at the end using the per-line
// horizontal-scroll snapshot captured during the active display.
void runFrame() {
  const int LINES = 262;
  const int ACTIVE = VDP_H;          // 192 active scanlines
  const uint64_t TPL = 228;          // T-states per scanline (228*262 = 59736, matches MSX frame)
  for (int line = 0; line < LINES; line++) {
    uint64_t target = cpu.cycles + TPL;
    while (cpu.cycles < target) cpu.step();
    vdpLineTick(line);                          // line counter + VBlank flag at line 192
    if (line < ACTIVE) vdpSnapshotLine(line);   // capture R8 hscroll for this raster
    if (vdpIrqActive()) cpu.irq(0xFF);          // IM 1 -> RST 38h (data-bus byte ignored)
  }
  // Render race-free at the frame boundary, only once core 0 has displayed the previous frame.
  if (!frameReady) { vdpRender(); frameReady = true; }
  if (pauseLatch) { pauseLatch = false; cpu.nmi(); }   // PAUSE -> NMI between frames
}

} // namespace sms

// ===================== host harness entry points (off-device boot test) ==========================
#ifdef SMS_HOST_BOOT
#include <cstdio>
#include <cstring>
#include "sms_cart.h"

static uint8_t  hb_ram[0x2000];
static uint8_t  hb_vram[0x4000];
static uint8_t  hb_fb[sms::FB_SIZE];
static uint8_t  hb_rom[0x100000];

void smsHostInit(const uint8_t* romP, int rl) {
  if (rl > (int)sizeof(hb_rom)) rl = sizeof(hb_rom);
  memcpy(hb_rom, romP, rl);
  sms::ram = hb_ram; sms::vram = hb_vram; sms::framebuffer = hb_fb;
  memset(hb_ram, 0, sizeof(hb_ram));
  memset(hb_vram, 0, sizeof(hb_vram));
  smsCartLoadImage(hb_rom, rl);
  sms::machineWire();
  sms::machineReset();
}

void smsHostRunFrames(int frames) { for (int i = 0; i < frames; i++) sms::runFrame(); }

void smsHostDumpRegs() {
  printf("\n--- VDP registers ---\n");
  for (int r = 0; r < 11; r++) printf("R%-2d=%02X ", r, sms::vdpRegister(r));
  printf("\nname base=%04X  sprite attr=%04X  sprite pat=%04X  hscroll=%02X vscroll=%02X lineCnt=%02X\n",
         (sms::vdpRegister(2) & 0x0E) << 10, (sms::vdpRegister(5) & 0x7E) << 7,
         (sms::vdpRegister(6) & 0x04) << 11, sms::vdpRegister(8), sms::vdpRegister(9), sms::vdpRegister(10));
  printf("--- CRAM (BG pal 0-15 / sprite pal 16-31), 6-bit --BBGGRR ---\n");
  for (int i = 0; i < 32; i++) { printf("%02X ", sms::vdpCram(i)); if ((i & 15) == 15) printf("\n"); }
}

// Dump the framebuffer as a binary PPM (P6) so the title screen can be eyeballed off-device.
void smsHostDumpPPM(const char* path) {
  uint16_t lut[32]; sms::vdpBuildPalette(lut);
  FILE* f = fopen(path, "wb"); if (!f) { printf("cannot write %s\n", path); return; }
  fprintf(f, "P6\n%d %d\n255\n", sms::VDP_W, sms::VDP_H);
  for (int i = 0; i < sms::FB_SIZE; i++) {
    uint16_t c = lut[sms::framebuffer[i] & 0x1F];
    uint8_t r = ((c >> 11) & 0x1F) << 3, g = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
    fputc(r, f); fputc(g, f); fputc(b, f);
  }
  fclose(f);
  printf("wrote %s (%dx%d)\n", path, sms::VDP_W, sms::VDP_H);
}
#endif
