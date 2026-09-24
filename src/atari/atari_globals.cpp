#include "../../emu.h"
#include "atari.h"

// Definitions for the Atari 2600 state shared across the core translation units, plus the TIA
// NTSC master palette. CPU registers stay file-local in atari_cpu.cpp; TIA object/beam scratch
// stays in atari_tia.cpp. This file holds the cross-file state + the palette.

namespace atari {

// Framebuffer points at the shared static buffer (set in atariSetup); 160x240 = 38400 bytes.
uint8_t *framebuffer = nullptr;
volatile uint32_t atariFrameCount = 0;
uint8_t *frontBuf = nullptr;
volatile uint8_t frontState = 0;
volatile int frontRows = 192;
volatile bool wsyncStall = false;

// Cartridge (filled by atari_cart.cpp).
uint8_t  *cartRom  = nullptr;
uint32_t  cartSize = 0;
volatile bool atariResetReq = false;

// On-screen startup warning (no loadable ROM / unsupported size), shown briefly after boot.
// Heap-allocated in atariSetup (not static BSS — the static DRAM budget is full, see atari_cpu.cpp).
char *loadWarn = nullptr;

// ---- TIA NTSC master palette: 128 colours = 16 hues x 8 luminances ----
// Index = (COLUxx >> 1) & 0x7F (the TIA colour byte is hue<<4 | luma<<1). Source RGB888 from the
// canonical Stella NTSC palette, converted to RGB565 at compile time (constexpr, so no static
// init-order dependency on `tft`, same approach as the NES palette).
static constexpr uint16_t rgb565(uint32_t rgb) {
  return (uint16_t)((((rgb >> 16) & 0xFF) >> 3) << 11 |
                    (((rgb >> 8)  & 0xFF) >> 2) << 5  |
                    (((rgb)       & 0xFF) >> 3));
}
const uint16_t atariPalette[128] = {
  // hue 0 (grey)
  rgb565(0x000000), rgb565(0x404040), rgb565(0x6C6C6C), rgb565(0x909090),
  rgb565(0xB0B0B0), rgb565(0xC8C8C8), rgb565(0xDCDCDC), rgb565(0xF4F4F4),
  // hue 1 (gold)
  rgb565(0x444400), rgb565(0x646410), rgb565(0x848424), rgb565(0xA0A034),
  rgb565(0xB8B840), rgb565(0xD0D050), rgb565(0xE8E85C), rgb565(0xFCFC68),
  // hue 2 (orange)
  rgb565(0x702800), rgb565(0x844414), rgb565(0x985C28), rgb565(0xAC783C),
  rgb565(0xBC8C4C), rgb565(0xCCA05C), rgb565(0xDCB468), rgb565(0xECC878),
  // hue 3 (bright orange)
  rgb565(0x841800), rgb565(0x983418), rgb565(0xAC5030), rgb565(0xC06848),
  rgb565(0xD0805C), rgb565(0xE09470), rgb565(0xECA880), rgb565(0xFCBC94),
  // hue 4 (pink/red)
  rgb565(0x880000), rgb565(0x9C2020), rgb565(0xB03C3C), rgb565(0xC05858),
  rgb565(0xD07070), rgb565(0xE08888), rgb565(0xECA0A0), rgb565(0xFCB4B4),
  // hue 5 (purple)
  rgb565(0x78005C), rgb565(0x8C2074), rgb565(0xA03C88), rgb565(0xB0589C),
  rgb565(0xC070B0), rgb565(0xD084C0), rgb565(0xDC9CD0), rgb565(0xECB0E0),
  // hue 6 (purple-blue)
  rgb565(0x480078), rgb565(0x602090), rgb565(0x783CA4), rgb565(0x8C58B8),
  rgb565(0xA070CC), rgb565(0xB484DC), rgb565(0xC49CEC), rgb565(0xD4B0FC),
  // hue 7 (blue)
  rgb565(0x140084), rgb565(0x302098), rgb565(0x4C3CAC), rgb565(0x6858C0),
  rgb565(0x7C70D0), rgb565(0x9488E0), rgb565(0xA8A0EC), rgb565(0xBCB4FC),
  // hue 8 (blue)
  rgb565(0x000088), rgb565(0x1C209C), rgb565(0x3840B0), rgb565(0x505CC0),
  rgb565(0x6874D0), rgb565(0x7C8CE0), rgb565(0x90A4EC), rgb565(0xA4B8FC),
  // hue 9 (light blue)
  rgb565(0x00187C), rgb565(0x1C3890), rgb565(0x3854A8), rgb565(0x5070BC),
  rgb565(0x6888CC), rgb565(0x7CA0DC), rgb565(0x90B4EC), rgb565(0xA4C8FC),
  // hue 10 (turquoise)
  rgb565(0x002C5C), rgb565(0x1C4C78), rgb565(0x386890), rgb565(0x5084AC),
  rgb565(0x689CC0), rgb565(0x7CB4D4), rgb565(0x90CCE8), rgb565(0xA4E0FC),
  // hue 11 (green-blue)
  rgb565(0x003C2C), rgb565(0x1C5C48), rgb565(0x387C64), rgb565(0x509C80),
  rgb565(0x68B494), rgb565(0x7CD0AC), rgb565(0x90E4C0), rgb565(0xA4FCD4),
  // hue 12 (green)
  rgb565(0x003C00), rgb565(0x205C20), rgb565(0x407C40), rgb565(0x5C9C5C),
  rgb565(0x74B474), rgb565(0x8CD08C), rgb565(0xA4E4A4), rgb565(0xB8FCB8),
  // hue 13 (yellow-green)
  rgb565(0x143800), rgb565(0x345C1C), rgb565(0x507C38), rgb565(0x6C9850),
  rgb565(0x84B468), rgb565(0x9CCC7C), rgb565(0xB4E490), rgb565(0xC8FCA4),
  // hue 14 (orange-green)
  rgb565(0x2C3000), rgb565(0x4C501C), rgb565(0x687034), rgb565(0x848C4C),
  rgb565(0x9CA864), rgb565(0xB4C078), rgb565(0xCCD488), rgb565(0xE0EC9C),
  // hue 15 (light orange)
  rgb565(0x442800), rgb565(0x644818), rgb565(0x846830), rgb565(0xA08444),
  rgb565(0xB89C58), rgb565(0xD0B46C), rgb565(0xE8CC7C), rgb565(0xFCE08C),
};

// Grayscale version (luminance of each entry), used when the VIDEO toggle is MONO. Heap-allocated
// (not static BSS — the static DRAM budget is full with four cores resident).
uint16_t *atariPaletteGray = nullptr;
void atariBuildGrayPalette() {
  if (!atariPaletteGray) atariPaletteGray = (uint16_t *)malloc(128 * sizeof(uint16_t));
  if (!atariPaletteGray) return;
  for (int i = 0; i < 128; i++) {
    uint16_t c = atariPalette[i];
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    int y = (r * 77 + g * 75 + b * 29) >> 8;     // weighted luma -> 0..31 (g is 0..63, half weight)
    if (y > 31) y = 31;
    atariPaletteGray[i] = (uint16_t)((y << 11) | ((y << 1) << 5) | y);
  }
}

} // namespace atari
