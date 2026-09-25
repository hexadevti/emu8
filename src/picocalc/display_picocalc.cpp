// display_picocalc.cpp -- ClockworkPi PicoCalc (RP2350) display backend: ST7365P/ILI9488 320x320
// panel on spi1, driven with the raw pico-sdk SPI + one dedicated DMA channel. Text is rendered
// with the bundled FreeSans9pt7b GFX font (same as the desktop backend) so this file needs NO
// graphics library at all -- no Adafruit_GFX, no Arduino_GFX, no TFT_eSPI.
//
// The whole file is inside `#if defined(BOARD_PICOCALC)`: arduino-cli compiles every .cpp under
// src/ for every board, so the guard is what keeps it out of the ESP32 binaries (the same pattern
// as src/desktop/display_sdl.cpp).
//
// THREE decisions worth knowing before reading the code:
//
//  1. RAW pico-sdk, not the Arduino SPI1 object. The hot path writes the SPI data register through
//     DMA and switches the PL022 between 8- and 16-bit frames, which needs the hardware handle
//     anyway; going through SPIClass on top of that would only add a second owner of the same
//     peripheral. The Arduino SPI1 object is never begin()-ed for this board.
//
//  2. CS IS HELD LOW FOR THE LIFETIME OF THE PROGRAM (asserted once in begin()). The panel is the
//     only device on spi1 -- the SD card is on spi0 and the PSRAM is bit-banged on plain GPIOs --
//     and ClockworkPi's own driver does the same. This removes any chance of a CS edge landing
//     between a RAMWR and its pixel data, which some ILI948x units treat as a command abort.
//     (It is also why board.h says not to use SPI1.setCS(): the PL022 hardware SSn would toggle
//     once per FRAME, i.e. once per 8 or 16 bits, which breaks every multi-byte write.)
//
//  3. 16-BIT SPI FRAMES REMOVE THE BYTE SWAP. In 16-bit mode the PL022 shifts a uint16_t out
//     MSB-first, which is exactly RGB565 big-endian on the wire -- so an RGB565 buffer can be
//     DMA'd verbatim with DMA_SIZE_16 and no pre-pass. Commands and their 8-bit parameters switch
//     the format back (fmt8/fmt16 below); the switch is cheap and happens once per window setup.
#include "../../board.h"

#if defined(BOARD_PICOCALC)

#include "display_picocalc.h"
#include <hardware/spi.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <string.h>

// Pixel format. 0 (default) = COLMOD 0x55, RGB565, 2 bytes/px -- community-confirmed working on the
// PicoCalc ST7365P and a 33% bandwidth saving. 1 = COLMOD 0x66, 18-bit, 3 bytes/px, which is what
// ClockworkPi own firmware hardcodes: slower, but guaranteed on any ILI9488. Flip this if the
// image comes up as garbage. Only the transport layer below differs; the hot path is identical.
#ifndef PICOCALC_LCD_18BIT
#define PICOCALC_LCD_18BIT 0
#endif

// --- GFX font structs (Adafruit layout) so the bundled font header compiles standalone ---
typedef struct {
  uint16_t bitmapOffset;
  uint8_t  width, height, xAdvance;
  int8_t   xOffset, yOffset;
} GFXglyph;
typedef struct {
  uint8_t  *bitmap;
  GFXglyph *glyph;
  uint16_t  first, last;
  uint8_t   yAdvance;
} GFXfont;
#include "../shared/fonts/FreeSans9pt7b.h"
static const GFXfont *UIFONT = &FreeSans9pt7b;

extern DisplayGFX tft;
extern void picocalcPumpInput();   // input_picocalc.cpp: drains the I2C keyboard into usbKeyboardReport()
extern void picocalcBacklightOn(); // input_picocalc.cpp: LCD backlight lives on the STM32, not a GPIO

#define LCD_SPI spi1

// ============================== staging buffers (declared in the header) ======================
namespace pcd {
  uint16_t stage[2][STAGE_LEN];
  uint32_t stageFill = 0;
  uint8_t  stageHalf = 0;
}

// ============================== low-level panel I/O ===========================================
static int  s_dma = -1;
static dma_channel_config s_cfgInc;    // read-increment ON  (a pixel buffer)
static dma_channel_config s_cfgSolid;  // read-increment OFF (one color repeated)
static bool s_fmt16 = false;           // PL022 currently in 16-bit frame mode?
static uint16_t s_solid = 0;           // DMA source for solid fills (must outlive the transfer)

static inline void dcCmd()  { gpio_put(LCD_DC_PIN, 0); }
static inline void dcData() { gpio_put(LCD_DC_PIN, 1); }

// Wait for the shifter to go idle and drain/clear the RX side. Required before spi_set_format(),
// which must not be called while a frame is in flight.
static void spiIdle() {
  while (spi_is_readable(LCD_SPI)) (void)spi_get_hw(LCD_SPI)->dr;
  while (spi_is_busy(LCD_SPI)) tight_loop_contents();
  while (spi_is_readable(LCD_SPI)) (void)spi_get_hw(LCD_SPI)->dr;
  spi_get_hw(LCD_SPI)->icr = SPI_SSPICR_RORIC_BITS;   // clear any RX overrun from the discarded reads
}

// ---------- pixel transport ----------
// 16bpp: DMA straight out of the RGB565 buffer, and transfers are ASYNC so the HGR decoder keeps
// filling the other staging half while SPI drains this one. txWait() joins the previous transfer.
#if !PICOCALC_LCD_18BIT

static inline void txWait() { if (s_dma >= 0) dma_channel_wait_for_finish_blocking(s_dma); }
static inline void txBuf(const uint16_t *src, uint32_t n) {
  dma_channel_configure(s_dma, &s_cfgInc, &spi_get_hw(LCD_SPI)->dr, src, n, true);
}
static inline void txSolid(uint16_t c, uint32_t n) {
  s_solid = c;
  dma_channel_configure(s_dma, &s_cfgSolid, &spi_get_hw(LCD_SPI)->dr, &s_solid, n, true);
}
static void fmt16() {
  if (s_fmt16) return;
  txWait(); spiIdle();
  spi_set_format(LCD_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  s_fmt16 = true;
}
static void fmt8() {
  if (!s_fmt16) return;
  txWait(); spiIdle();
  spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  s_fmt16 = false;
}

#else

// 18bpp fallback: the panel wants 3 bytes/px, so each chunk is expanded into a byte buffer first
// and pushed with an 8-bit DMA. Necessarily SYNCHRONOUS (the scratch buffer is reused), which is
// the real cost of this path -- there is no decode/transfer overlap. Frames only, never commands.
static uint8_t s_stage8[3 * pcd::STAGE_LEN];
static inline void txWait() { if (s_dma >= 0) dma_channel_wait_for_finish_blocking(s_dma); }
static void txBytes(uint32_t nbytes) {
  dma_channel_configure(s_dma, &s_cfgInc, &spi_get_hw(LCD_SPI)->dr, s_stage8, nbytes, true);
  txWait();
}
static inline void put888(uint8_t *d, uint16_t c) {
  d[0] = (uint8_t)((c >> 8) & 0xF8);                            // R5 -> R8
  d[1] = (uint8_t)((c >> 3) & 0xFC);                            // G6 -> G8
  d[2] = (uint8_t)((c << 3) & 0xF8);                            // B5 -> B8
}
static void txBuf(const uint16_t *src, uint32_t n) {
  while (n) {
    uint32_t k = (n > pcd::STAGE_LEN) ? pcd::STAGE_LEN : n;
    for (uint32_t i = 0; i < k; i++) put888(s_stage8 + i * 3, src[i]);
    txBytes(k * 3);
    src += k; n -= k;
  }
}
static void txSolid(uint16_t c, uint32_t n) {
  uint32_t k = (n > pcd::STAGE_LEN) ? pcd::STAGE_LEN : n;
  for (uint32_t i = 0; i < k; i++) put888(s_stage8 + i * 3, c);
  while (n) { uint32_t j = (n > k) ? k : n; txBytes(j * 3); n -= j; }
}
static inline void fmt16() {}   // always 8-bit frames on this path
static inline void fmt8()  { txWait(); }

#endif

// Command / 8-bit-parameter writes. Always in 8-bit frame mode, always blocking: these are a few
// bytes each and only happen at init and at window setup.
static void wrCmd(uint8_t c) {
  fmt8();
  dcCmd();
  spi_write_blocking(LCD_SPI, &c, 1);   // returns only once the byte has actually shifted out
  dcData();
}
static void wrArgs(const uint8_t *d, size_t n) {
  if (n) spi_write_blocking(LCD_SPI, d, n);
}

// CASET / RASET / RAMWR in RAW PANEL coordinates (inclusive bounds). Leaves DC high and the panel
// waiting for pixel data; the caller then calls fmt16() and pushes.
static void panelWindow(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  uint8_t b[4];
  wrCmd(0x2A);                                                  // CASET
  b[0] = (uint8_t)(x0 >> 8); b[1] = (uint8_t)x0;
  b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
  wrArgs(b, 4);
  wrCmd(0x2B);                                                  // RASET
  b[0] = (uint8_t)(y0 >> 8); b[1] = (uint8_t)y0;
  b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
  wrArgs(b, 4);
  wrCmd(0x2C);                                                  // RAMWR
}

// Solid fill / buffer push at RAW PANEL coordinates, already clipped by the callers.
static void panelFillRaw(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  panelWindow(x, y, x + w - 1, y + h - 1);
  fmt16();
  txSolid(color, (uint32_t)w * (uint32_t)h);
  txWait();
}
static void panelPushRaw(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *src) {
  if (w <= 0 || h <= 0 || !src) return;
  panelWindow(x, y, x + w - 1, y + h - 1);
  fmt16();
  txBuf(src, (uint32_t)w * (uint32_t)h);
  txWait();                       // callers reuse their scratch band immediately after this returns
}

// ---------- the writeColor() staging engine (the inline half lives in the header) ----------
namespace pcd {
// A staging half just filled up: hand it to DMA and flip. txWait() joins the transfer started by
// the PREVIOUS kick, which was on the other half -- that is the whole point of the ping-pong, the
// decoder gets to refill one half while SPI is still draining the other.
void stageKick() {
  txWait();
  txBuf(stage[stageHalf], stageFill);
  stageHalf ^= 1;
  stageFill  = 0;
}
// Flush a partially-filled half, in order, and join. Used before anything that reconfigures DMA
// or moves the address window.
void stageDrain() {
  if (stageFill) stageKick();
  txWait();
}
// writeColor() with a large len: skip staging entirely and let DMA repeat one source word. Makes
// the full-screen clear (writeColor(color, 320*240) in video.cpp) a single zero-CPU descriptor.
void bulkFill(uint16_t color, uint32_t len) {
  stageDrain();
  fmt16();
  txSolid(color, len);
  txWait();
}
}  // namespace pcd

// ---------- logical (320 x logH) -> panel clipping helper ----------
static bool clipLogical(int32_t &x, int32_t &y, int32_t &w, int32_t &h, int32_t logH) {
  if (w <= 0 || h <= 0) return false;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > DISP_LOGICAL_W) w = DISP_LOGICAL_W - x;
  if (y + h > logH) h = logH - y;
  return w > 0 && h > 0;
}

// ============================== panel bring-up ================================================
// ClockworkPi init sequence (Code/picocalc_helloworld/lcdspi/lcdspi.c, pico_lcd_init()), byte for
// byte, with COLMOD swapped to RGB565 unless PICOCALC_LCD_18BIT. Encoded as {cmd, argc, args...}.
static const uint8_t kInit[] = {
  0xE0, 15, 0x00,0x03,0x09,0x08,0x16,0x0A,0x3F,0x78,0x4C,0x09,0x0A,0x08,0x16,0x1A,0x0F,  // + gamma
  0xE1, 15, 0x00,0x16,0x19,0x03,0x0F,0x05,0x32,0x45,0x46,0x04,0x0E,0x0D,0x35,0x37,0x0F,  // - gamma
  0xC0,  2, 0x17,0x15,                       // power control 1
  0xC1,  1, 0x41,                            // power control 2
  0xC5,  3, 0x00,0x12,0x80,                  // VCOM control
  0x36,  1, 0x48,                            // MADCTL: MX | BGR  (ILI9341_Portrait in their header)
#if PICOCALC_LCD_18BIT
  0x3A,  1, 0x66,                            // COLMOD: 18 bit/px over SPI (ClockworkPi default)
#else
  0x3A,  1, 0x55,                            // COLMOD: 16 bit/px (RGB565) -- see the note at the top
#endif
  0xB0,  1, 0x00,                            // interface mode control
  0xB1,  1, 0xA0,                            // frame rate control
  0x21,  0,                                  // INVON -- this panel is display-inverted; required
  0xB4,  1, 0x02,                            // display inversion control
  0xB6,  3, 0x02,0x02,0x3B,                  // display function control
  0xB7,  1, 0xC6,                            // entry mode set
  0xE9,  1, 0x00,
  0xF7,  4, 0xA9,0x51,0x2C,0x82,             // adjust control 3
};

void DisplayGFX::begin() {
  // Called TWICE by design: the boot console brings the panel up as the first thing setup()
  // does, and videoSetup() calls begin() again once the emulator owns the screen. The second
  // call is NOT redundant -- it re-runs spi_init() and the pin mux and, crucially, puts the
  // PL022 back into 8-bit frames with s_fmt16 in agreement. Anything that ran in between and
  // disturbed spi1 or left the frame width out of sync is repaired right there, which is why
  // the panel drew before this was ever guarded. Do NOT short-circuit it.
  //
  // The one thing that must not repeat is the DMA claim -- a second claim would leak the first
  // channel -- so that, and only that, is guarded below.

  // --- GPIO: CS/DC/RST are plain outputs (see note 2 at the top); SCK/TX/RX go to the SPI mux ---
  gpio_init(LCD_CS_PIN);  gpio_set_dir(LCD_CS_PIN,  GPIO_OUT); gpio_put(LCD_CS_PIN,  1);
  gpio_init(LCD_DC_PIN);  gpio_set_dir(LCD_DC_PIN,  GPIO_OUT); gpio_put(LCD_DC_PIN,  1);
  gpio_init(LCD_RST_PIN); gpio_set_dir(LCD_RST_PIN, GPIO_OUT); gpio_put(LCD_RST_PIN, 1);

  spi_init(LCD_SPI, LCD_SPI_HZ);
  spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  s_fmt16 = false;
  gpio_set_function(LCD_SCK_PIN,  GPIO_FUNC_SPI);
  gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);
  gpio_set_function(LCD_MISO_PIN, GPIO_FUNC_SPI);

  // --- DMA channel: pace on the SPI TX DREQ, write the data register without incrementing ---
  if (s_dma < 0) s_dma = dma_claim_unused_channel(true);   // once only; see begin()'s note
  s_cfgInc = dma_channel_get_default_config(s_dma);
#if PICOCALC_LCD_18BIT
  channel_config_set_transfer_data_size(&s_cfgInc, DMA_SIZE_8);
#else
  channel_config_set_transfer_data_size(&s_cfgInc, DMA_SIZE_16);
#endif
  channel_config_set_dreq(&s_cfgInc, spi_get_dreq(LCD_SPI, true));
  channel_config_set_read_increment(&s_cfgInc, true);
  channel_config_set_write_increment(&s_cfgInc, false);
  s_cfgSolid = s_cfgInc;
  channel_config_set_read_increment(&s_cfgSolid, false);

  // --- hardware reset (ClockworkPi timings: 10ms low, 200ms settle) ---
  gpio_put(LCD_RST_PIN, 1); delay(10);
  gpio_put(LCD_RST_PIN, 0); delay(10);
  gpio_put(LCD_RST_PIN, 1); delay(200);

  gpio_put(LCD_CS_PIN, 0);                   // asserted from here on, for good (note 2)

  for (size_t i = 0; i < sizeof(kInit); ) {
    uint8_t cmd = kInit[i++], argc = kInit[i++];
    wrCmd(cmd);
    wrArgs(&kInit[i], argc);
    i += argc;
  }
  wrCmd(0x11); delay(120);                   // SLPOUT
  wrCmd(0x29); delay(120);                   // DISPON

  panelFillRaw(0, 0, PANEL_NATIVE_W, PANEL_NATIVE_H, TFT_BLACK);   // whole panel, bars included

  // The panel has no backlight GPIO on the Pico side: brightness is a register on the STM32
  // keyboard MCU, which remembers it across resets. It normally comes up lit, but if a previous
  // firmware (or the user) dimmed it to zero the panel is black no matter how well we drive it,
  // so drive it up here -- the one place that runs before anything is drawn.
  picocalcBacklightOn();
}

// ============================== primitives ====================================================
// Full-panel mode: the settings menu gets the whole 320x320 instead of the letterboxed 320x240.
// Everything logical below goes through _offY/_logH, so flipping them is the entire switch.
bool DisplayGFX::setFullPanel(bool on) {
  if (on == fullPanel()) return false;
  pxFlush();
  _offY = on ? 0 : DISP_OFFSET_Y;
  _logH = on ? PANEL_NATIVE_H : DISP_LOGICAL_H;
  return true;
}

void DisplayGFX::fillScreen(uint16_t color) {
  // The LOGICAL screen only. The letterbox bars are fillPanelBlack's job -- the same split the
  // Arduino_GFX backend uses, and video.cpp always calls the two together when clearing.
  pxFlush();
  panelFillRaw(0, _offY, DISP_LOGICAL_W, _logH, color);
}

void DisplayGFX::fillPanelBlack() {
  pxFlush();
  panelFillRaw(0, 0, PANEL_NATIVE_W, DISP_OFFSET_Y, TFT_BLACK);                       // top bar
  panelFillRaw(0, DISP_OFFSET_Y + DISP_LOGICAL_H, PANEL_NATIVE_W,
               PANEL_NATIVE_H - DISP_OFFSET_Y - DISP_LOGICAL_H, TFT_BLACK);           // bottom bar
}

// RAW panel coordinates, so this can reach the letterbox bars that fillRect() clips away.
// The Apple II drive light is drawn with it (src/shared/video.cpp).
void DisplayGFX::fillPanelRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  pxFlush();
  if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
  if (x + w > PANEL_NATIVE_W || y + h > PANEL_NATIVE_H) return;
  panelFillRaw(x, y, w, h, color);
}

void DisplayGFX::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  pxFlush();
  if (!clipLogical(x, y, w, h, _logH)) return;
  panelFillRaw(x, y + _offY, w, h, color);
}

void DisplayGFX::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  // good-enough rounded rect: a full rect minus the four corner pixels (matches the other backends).
  fillRect(x, y, w, h, color);
  if (r <= 0) return;
  for (int i = 0; i < r; i++) {
    for (int j = 0; j < r; j++) {
      if (i * i + j * j <= r * r) continue;
      pxRun(x + (r - 1 - i), y + (r - 1 - j), TFT_BLACK);
      pxRun(x + w - r + i,   y + (r - 1 - j), TFT_BLACK);
      pxRun(x + (r - 1 - i), y + h - r + j,   TFT_BLACK);
      pxRun(x + w - r + i,   y + h - r + j,   TFT_BLACK);
    }
  }
  pxFlush();
}

void DisplayGFX::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  fillRect(x + r,     y,         w - 2 * r, 1, color);
  fillRect(x + r,     y + h - 1, w - 2 * r, 1, color);
  fillRect(x,         y + r,     1, h - 2 * r, color);
  fillRect(x + w - 1, y + r,     1, h - 2 * r, color);
}

// 8x8 glyph nearest-scaled into a pw x ph cell (the PC-XT/CGA text renderer entry point).
void DisplayGFX::drawGlyph8(int px, int py, int pw, int ph, const uint8_t *g, uint16_t fg, uint16_t bg, bool transparentBg) {
  if (!g || pw <= 0 || ph <= 0) return;
  for (int oy = 0; oy < ph; oy++) {
    uint8_t bits = g[(oy * 8) / ph];                 // nearest-neighbour source row
    for (int ox = 0; ox < pw; ox++) {
      bool on = bits & (0x80 >> ((ox * 8) / pw));
      if (on)                  pxRun(px + ox, py + oy, fg);
      else if (!transparentBg) pxRun(px + ox, py + oy, bg);
    }
  }
  pxFlush();
}

// ---------- bitmaps ----------
// `_swap` is deliberately ignored: 16-bit SPI frames already leave MSB-first, so an RGB565 buffer
// goes out correctly as-is. (The Arduino_GFX and SDL backends ignore it too -- only TFT_eSPI, which
// pushes bytes, needs the flag.)
void DisplayGFX::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) {
  pxFlush();
  if (!data || w <= 0 || h <= 0) return;
  int32_t sx = 0, sy = 0, cx = x, cy = y, cw = w, ch = h;
  if (cx < 0) { sx = -cx; cw -= sx; cx = 0; }
  if (cy < 0) { sy = -cy; ch -= sy; cy = 0; }
  if (cx + cw > DISP_LOGICAL_W) cw = DISP_LOGICAL_W - cx;
  if (cy + ch > _logH) ch = _logH - cy;
  if (cw <= 0 || ch <= 0) return;
  if (sx == 0 && cw == w) {                          // untouched rows are contiguous -> one transfer
    panelPushRaw(cx, cy + _offY, cw, ch, data + (size_t)sy * w);
  } else {                                           // horizontally clipped -> row at a time
    for (int32_t r = 0; r < ch; r++)
      panelPushRaw(cx, cy + _offY + r, cw, 1, data + (size_t)(sy + r) * w + sx);
  }
}
// Same geometry as pushImage on this board (logical == panel apart from the letterbox offset).
void DisplayGFX::pushPanelBand(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) {
  pushImage(x, y, w, h, data);
}
// RAW panel rows, no letterbox offset (the tiny386 renderer draws full-panel).
void DisplayGFX::drawCanvasRGB565(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) {
  pxFlush();
  if (!data || x < 0 || y < 0 || w <= 0 || h <= 0) return;
  if (x + w > PANEL_NATIVE_W || y + h > PANEL_NATIVE_H) return;
  panelPushRaw(x, y, w, h, data);
}

// ---------- the Apple II scanline window ----------
// video.cpp calls setAddrWindow() ONCE, then startWrite(), then writeColor() per pixel, then
// endWrite() -- the TFT_eSPI order. So the window is programmed here as its own 8-bit transaction
// and startWrite() only has to flip the PL022 into 16-bit frames.
void DisplayGFX::setAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h) {
  pxFlush();
  pcd::stageDrain();
  panelWindow(x, y + _offY, x + w - 1, y + _offY + h - 1);
}
void DisplayGFX::startWrite() {
  pcd::stageFill = 0;                                // nothing carried over from a previous window
  pcd::stageHalf = 0;
  fmt16();
}
void DisplayGFX::endWrite() {
  pcd::stageDrain();
}

// ---------- single pixels, coalesced into horizontal runs ----------
// Text and the rounded-rect corners are the only per-pixel callers. Emitting each one as its own
// CASET/RASET/RAMWR would cost ~5us per pixel; batching a row of consecutive same-colour pixels
// into one panel write is what keeps the settings menu feeling instant. Every public method that
// can start a run also calls pxFlush() before it returns, so no state leaks between calls.
void DisplayGFX::pxRun(int32_t x, int32_t y, uint16_t color) {
  const int32_t clipH = _rawText ? PANEL_NATIVE_H : _logH;
  if ((uint32_t)x >= (uint32_t)DISP_LOGICAL_W || (uint32_t)y >= (uint32_t)clipH) return;
  if (_spanY == y && _spanColor == color && _spanX + _spanLen == x) { _spanLen++; return; }
  pxFlush();
  _spanX = x; _spanY = y; _spanLen = 1; _spanColor = color;
}
void DisplayGFX::pxFlush() {
  if (_spanY < 0 || _spanLen <= 0) { _spanY = -1; _spanLen = 0; return; }
  int32_t x = _spanX, y = _spanY, len = _spanLen; uint16_t c = _spanColor;
  _spanY = -1; _spanLen = 0;                         // clear first: panelFillRaw must not re-enter
  panelFillRaw(x, y + (_rawText ? 0 : _offY), len, 1, c);
}

// ============================== text ==========================================================
// FreeSans9pt7b for all sizes, ported from the desktop backend so the two look identical. Three
// sizes are picked by the TFT_eSPI font number:
//   font 2 (and up, except 3) -- titles/buttons, 1:1;
//   font 3                    -- the settings file list, 4/5 (a size between the other two that
//                                only this board has; the CYD's TFT_eSPI has no font 3);
//   font 1                    -- the dense option/label rows that use the device 6x8, 2/3.
// Scaled glyphs are box-filtered: each output pixel gets the fraction of its area that the source
// glyph covers. With a known background (setTextColor(fg, bg)) that fraction blends fg into bg,
// which keeps strokes one pixel thin and smooth instead of the fat 2px strokes an OR-downsample
// leaves. Without a background there is nothing to blend into, so any coverage paints fg (the old
// OR-downsample, so transparent text never loses a thin stroke).
static inline void fontScale(uint8_t font, int &num, int &den) {
  if (font == 3)     { num = 4; den = 5; }
  else if (font >= 2) { num = 1; den = 1; }
  else               { num = 2; den = 3; }
}
static inline int scN(int v, int num, int den) { return (v * num) / den; }

// Is the glyph pixel (sx,sy) set? (Adafruit GFX bitmaps stream row-major, MSB first.)
static inline bool glyphBit(const uint8_t *bmp, int gw, int sx, int sy) {
  int idx = sy * gw + sx;
  return (bmp[idx >> 3] & (0x80 >> (idx & 7))) != 0;
}

// fg over bg at alpha a/256, per RGB565 channel.
static inline uint16_t blend565(uint16_t fg, uint16_t bg, int a) {
  const int b = 256 - a;
  uint16_t r = (uint16_t)((((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * b) >> 8);
  uint16_t g = (uint16_t)((((fg >> 5)  & 0x3F) * a + ((bg >> 5)  & 0x3F) * b) >> 8);
  uint16_t l = (uint16_t)((( fg        & 0x1F) * a + ( bg        & 0x1F) * b) >> 8);
  return (uint16_t)((r << 11) | (g << 5) | l);
}

static void strMetrics(const char *s, int num, int den, int &bw, int &minYO, int &maxYExt) {
  bw = 0; minYO = 0; maxYExt = 0; bool any = false;
  for (const char *p = s; *p; p++) {
    uint8_t c = (uint8_t)*p;
    if (c < UIFONT->first || c > UIFONT->last) { bw += scN(UIFONT->yAdvance / 2, num, den); continue; }
    const GFXglyph *g = &UIFONT->glyph[c - UIFONT->first];
    bw += scN(g->xAdvance, num, den);
    int yo = scN(g->yOffset, num, den), ext = scN(g->yOffset + g->height, num, den);
    if (!any || yo  < minYO)   minYO   = yo;
    if (!any || ext > maxYExt) maxYExt = ext;
    any = true;
  }
  if (!any) { minYO = scN(-(int)UIFONT->yAdvance + 4, num, den); maxYExt = 0; }
}

int16_t DisplayGFX::textWidth(const char *s, uint8_t font) {
  if (!s) return 0;
  int num, den, bw, minYO, maxYExt;
  fontScale(font, num, den);
  strMetrics(s, num, den, bw, minYO, maxYExt);
  return (int16_t)bw;
}

int16_t DisplayGFX::drawString(const char *s, int32_t x, int32_t y, uint8_t font) {
  if (!s) return 0;
  int num, den;
  fontScale(font, num, den);

  int bw, minYO, maxYExt;
  strMetrics(s, num, den, bw, minYO, maxYExt);
  int bh = maxYExt - minYO;
  const uint8_t horiz = _datum % 3, vert = _datum / 3;
  int boxLeft = x, boxTop = y;
  if (horiz == 1) boxLeft = x - bw / 2; else if (horiz == 2) boxLeft = x - bw;
  if (vert  == 1) boxTop  = y - bh / 2; else if (vert  == 2) boxTop  = y - bh;
  if (_textHasBg) fillRect(boxLeft, boxTop, bw, bh > 0 ? bh : 1, _textBg);

  // Box filter in "sub-units": output pixel o spans [o*den, (o+1)*den), source pixel s spans
  // [s*num, (s+1)*num), so overlaps are exact integers and a fully covered pixel sums to den*den.
  const int full = den * den;
  int baseline = boxTop - minYO;   // output rows = baseline + scaled(yOffset) + outRow
  int cursorX = boxLeft;
  for (const char *p = s; *p; p++) {
    uint8_t c = (uint8_t)*p;
    if (c < UIFONT->first || c > UIFONT->last) { cursorX += scN(UIFONT->yAdvance / 2, num, den); continue; }
    const GFXglyph *g = &UIFONT->glyph[c - UIFONT->first];
    const uint8_t *bmp = UIFONT->bitmap + g->bitmapOffset;
    int ow  = (g->width  * num + den - 1) / den;   // scaled glyph box (round up so nothing is lost)
    int oh  = (g->height * num + den - 1) / den;
    int ox0 = cursorX  + scN(g->xOffset, num, den);
    int oy0 = baseline + scN(g->yOffset, num, den);
    for (int oy = 0; oy < oh; oy++) {
      const int y0 = oy * den, y1 = y0 + den;
      int sy1 = (y1 + num - 1) / num; if (sy1 > g->height) sy1 = g->height;
      for (int ox = 0; ox < ow; ox++) {
        const int x0 = ox * den, x1 = x0 + den;
        int sx1 = (x1 + num - 1) / num; if (sx1 > g->width) sx1 = g->width;
        int cov = 0;
        for (int sy = y0 / num; sy < sy1; sy++) {
          const int vy = (y1 < (sy + 1) * num ? y1 : (sy + 1) * num) - (y0 > sy * num ? y0 : sy * num);
          for (int sx = x0 / num; sx < sx1; sx++) {
            if (!glyphBit(bmp, g->width, sx, sy)) continue;
            cov += vy * ((x1 < (sx + 1) * num ? x1 : (sx + 1) * num) - (x0 > sx * num ? x0 : sx * num));
          }
        }
        if (cov <= 0) continue;
        if (!_textHasBg || cov >= full) { pxRun(ox0 + ox, oy0 + oy, _textFg); continue; }
        const int a = cov * 256 / full;
        if (a < 24) continue;       // a sliver of a neighbouring stroke: leave the background
        pxRun(ox0 + ox, oy0 + oy, blend565(_textFg, _textBg, a));
      }
    }
    cursorX += scN(g->xAdvance, num, den);
  }
  pxFlush();
  return (int16_t)bw;
}

// Same glyph walk, but addressing the raw panel so it can write into the letterbox bars. The
// text background is forced off for the duration: that fill goes through fillRect(), which is
// logical-space and would land 40 rows off. Callers that want a backdrop use fillPanelRect().
int16_t DisplayGFX::drawPanelString(const char *s, int32_t x, int32_t y, uint8_t font) {
  const bool hadBg = _textHasBg;
  _textHasBg = false;
  _rawText   = true;
  int16_t w = drawString(s, x, y, font);
  _rawText   = false;      // after drawString, whose trailing pxFlush() still needed it set
  _textHasBg = hadBg;
  return w;
}

// ============================== per-frame hook ================================================
// Nothing to present: every primitive already wrote to the panel. The frame boundary is still the
// right place to drain the I2C keyboard, which is what desktopPumpInput() does for the SDL build.
void DisplayGFX::flush() {
  pxFlush();
  picocalcPumpInput();
}

// --- free-function shims (declared in display_picocalc.h / proto.h) ---
void displayFlush() { tft.flush(); }
void displaySetUiMode(bool ui) { tft.setUiMode(ui); }
// The cores report their active video rect each frame so a SCREEN FILL mode could zoom it. This
// board has no canvas to zoom: SCREEN: FILL is done by the MSX/SMS renderers (msx.cpp, sms.cpp)
// scaling straight into the 320x240 window, so there is nothing to record.
void displaySetVideoRect(int, int) {}
void displaySetVideoFill(int, int, bool) {}

#endif // BOARD_PICOCALC
