#include "../../emu.h"
#include "nes.h"
#include "esp_heap_caps.h"   // heap_caps_malloc / MALLOC_CAP_INTERNAL (force NES hot RAM internal)

// Definitions for the NES state shared across the NES translation units (cpu/memory/ppu/cart).
// CPU registers + decode tables stay file-local in nes_cpu.cpp; PPU scratch stays in
// nes_ppu.cpp. This file holds the cross-file state + the master palette.

// Allocate from INTERNAL DRAM when it fits, falling back to the default heap (PSRAM on the S3)
// only when internal is exhausted. The NES hot path is brutally latency-sensitive — every opcode
// fetch reads prgRom, every rendered pixel reads chrData — and on the ESP32-S3 the default malloc
// steers allocations >=16K (e.g. a 32K PRG) into slow PSRAM, stalling the core-1 interpreter on
// the external-memory bus. Forcing them internal keeps the emulation off PSRAM. (The CYD has no
// PSRAM, so this is identical to malloc there.) Big ROMs that don't fit internal fall back to PSRAM.
void *nesAllocFast(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);  // non-null => guaranteed internal
  sprintf(buf, "NES alloc %6uB -> %s", (unsigned)n,
          p ? "INTERNAL DRAM (fast)" : "PSRAM (internal full, fallback)");
  printLog(buf);
  return p ? p : malloc(n);
}

namespace nes {

// 2K internal CPU RAM (malloc'd in nesSetup; pointer, not static BSS — dram0_0_seg is tight)
uint8_t *cpuRam = nullptr;

// Cartridge (filled by nes_cart.cpp). The full PRG/CHR ROM is malloc'd on the NES path only;
// the mapper windows (prgMap/chrMap) index into them.
uint8_t  *prgRom     = nullptr;
uint32_t  prgRomSize = 0;
uint8_t  *chrData    = nullptr;
uint32_t  chrSize    = 0;
bool      chrIsRam   = false;
uint8_t  *prgRam     = nullptr;
uint8_t   mirrorMode = MIRROR_HORIZONTAL;
uint8_t   mapperNum  = 0;

// Mapper banking windows: 4 x 8K PRG ($8000/$A000/$C000/$E000), 8 x 1K CHR ($0000..$1FFF).
uint8_t *prgMap[4] = { nullptr, nullptr, nullptr, nullptr };
uint8_t *chrMap[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

// PPU memory (vram malloc'd in nesSetup; oam/palette are tiny, kept static)
uint8_t *vram = nullptr;
uint8_t paletteRam[0x20];
uint8_t oam[0x100];
uint8_t *framebuffer = nullptr;          // points at sharedBigBuf (set in nesSetup)
volatile bool frameReady = false;
volatile bool nmiPending = false;
volatile bool irqLine = false;           // mapper IRQ line (MMC3 scanline counter), level-held
volatile bool nesResetReq = false;       // settings: load a new ROM, then reset CPU/PPU on resume
int dmaStallCycles = 0;
volatile uint32_t nesFrameCount = 0;     // bumped each completed PPU frame (FPS diagnostic)

// On-screen startup warning: one short line per ROM skipped at load time (unsupported mapper /
// too big for RAM). Shown by the render loop for a few seconds after boot, then cleared.
char *loadWarn = nullptr;                // malloc'd in nesSetup (not static BSS — DRAM budget is full)

// ---- NES master palette (NTSC 2C02), source RGB888 -> RGB565 at compile time ----
// constexpr so there is no static-init-order dependency on `tft` (the C64 palette uses literal
// hex for the same reason; here we keep the readable RGB source and convert with constexpr).
static constexpr uint16_t rgb565(uint32_t rgb) {
  return (uint16_t)((((rgb >> 16) & 0xFF) >> 3) << 11 |
                    (((rgb >> 8)  & 0xFF) >> 2) << 5  |
                    (((rgb)       & 0xFF) >> 3));
}
const uint16_t nesPalette[64] = {
  rgb565(0x666666), rgb565(0x002A88), rgb565(0x1412A7), rgb565(0x3B00A4),
  rgb565(0x5C007E), rgb565(0x6E0040), rgb565(0x6C0600), rgb565(0x561D00),
  rgb565(0x333500), rgb565(0x0B4800), rgb565(0x005200), rgb565(0x004F08),
  rgb565(0x00404D), rgb565(0x000000), rgb565(0x000000), rgb565(0x000000),
  rgb565(0xADADAD), rgb565(0x155FD9), rgb565(0x4240FF), rgb565(0x7527FE),
  rgb565(0xA01ACC), rgb565(0xB71E7B), rgb565(0xB53120), rgb565(0x994E00),
  rgb565(0x6B6D00), rgb565(0x388700), rgb565(0x0C9300), rgb565(0x008F32),
  rgb565(0x007C8D), rgb565(0x000000), rgb565(0x000000), rgb565(0x000000),
  rgb565(0xFFFEFF), rgb565(0x64B0FF), rgb565(0x9290FF), rgb565(0xC676FF),
  rgb565(0xF36AFF), rgb565(0xFE6ECC), rgb565(0xFE8170), rgb565(0xEA9E22),
  rgb565(0xBCBE00), rgb565(0x88D800), rgb565(0x5CE430), rgb565(0x45E082),
  rgb565(0x48CDDE), rgb565(0x4F4F4F), rgb565(0x000000), rgb565(0x000000),
  rgb565(0xFFFEFF), rgb565(0xC0DFFF), rgb565(0xD3D2FF), rgb565(0xE8C8FF),
  rgb565(0xFBC2FF), rgb565(0xFEC4EA), rgb565(0xFECCC5), rgb565(0xF7D8A5),
  rgb565(0xE4E594), rgb565(0xCFEF96), rgb565(0xBDF4AB), rgb565(0xB3F3CC),
  rgb565(0xB5EBF2), rgb565(0xB8B8B8), rgb565(0x000000), rgb565(0x000000),
};

// Grayscale version of the master palette (luminance of each entry), used when the VIDEO toggle
// is MONO. Filled at boot by nesBuildGrayPalette() from nesPalette (RGB565 -> luma -> RGB565).
uint16_t nesPaletteGray[64];
void nesBuildGrayPalette() {
  for (int i = 0; i < 64; i++) {
    uint16_t c = nesPalette[i];
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    int y = (r * 77 + g * 75 + b * 29) >> 8;       // weighted luma -> 0..31 (g is 0..63, half weight)
    if (y > 31) y = 31;
    nesPaletteGray[i] = (uint16_t)((y << 11) | ((y << 1) << 5) | y);
  }
}

} // namespace nes
