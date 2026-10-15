// display_picocalc.cpp -- ClockworkPi PicoCalc (RP2350) display backend: ST7365P/ILI9488 320x320
// panel on spi1, driven with the raw pico-sdk SPI + one dedicated DMA channel. Text is rendered
// with the bitmap fonts in src/shared/uitext.h (same as the desktop backend) so this file needs NO
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
#include <hardware/clocks.h>
#include <string.h>

// Pixel format. 0 (default) = COLMOD 0x55, RGB565, 2 bytes/px -- community-confirmed working on the
// PicoCalc ST7365P and a 33% bandwidth saving. 1 = COLMOD 0x66, 18-bit, 3 bytes/px, which is what
// ClockworkPi own firmware hardcodes: slower, but guaranteed on any ILI9488. Flip this if the
// image comes up as garbage. Only the transport layer below differs; the hot path is identical.
#ifndef PICOCALC_LCD_18BIT
#define PICOCALC_LCD_18BIT 0
#endif

#include "../shared/uitext.h"      // menu fonts + renderer, shared with the desktop backend

extern DisplayGFX tft;
extern void picocalcPumpInput();   // input_picocalc.cpp: drains the I2C keyboard into usbKeyboardReport()
extern void picocalcBacklightOn(); // input_picocalc.cpp: LCD backlight lives on the STM32, not a GPIO

#define LCD_SPI spi1

// ============================== staging buffers (declared in the header) ======================
namespace pcd {
  uint16_t stage[2][STAGE_LEN];
  uint32_t stageFill = 0;
  uint8_t  stageHalf = 0;
  uint8_t  hsc = 0;
  uint8_t  rowBlend = 0;
  bool     smooth = false;      // setSmooth() armed for this window
  bool     prevValid = false;   // stage[stageHalf ^ 1] holds the previous full row of this window
  bool     holding = false;     // ...or a row marked setRowBlend(2), waiting for the next one
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
static inline uint16_t avg565(uint16_t a, uint16_t b) {
  return (uint16_t)((((a ^ b) & 0xF7DE) >> 1) + (a & b));
}
// SCREEN: SMOOTH (setSmooth): a full row is blended before it goes out. Out of line, so the inline
// writeColor() stays a plain store.
static void smoothKick() {
  uint16_t *cur = stage[stageHalf], *oth = stage[stageHalf ^ 1];
  if (hsc)                                           // 7:8: the inserted pixel of each 8-pixel group
    for (uint32_t i = 7; i < STAGE_LEN - 1; i += 8) cur[i] = avg565(cur[i - 1], cur[i + 1]);
  const uint8_t mode = rowBlend;
  rowBlend = 0;
  if (holding) {                                     // the held row (oth) goes first, blended with this one
    for (uint32_t i = 0; i < STAGE_LEN; i++) oth[i] = avg565(oth[i], cur[i]);
    txWait();
    txBuf(oth, STAGE_LEN);
    holding = false;
  } else if (mode == 1 && prevValid) {               // oth is the row above, still (only) being read by DMA
    for (uint32_t i = 0; i < STAGE_LEN; i++) cur[i] = avg565(cur[i], oth[i]);
  } else if (mode == 2) {                            // hold it; the next row lands in oth once it is idle
    txWait();
    holding = true;
    stageHalf ^= 1;
    stageFill  = 0;
    return;
  }
  txWait();
  txBuf(cur, STAGE_LEN);
  stageHalf ^= 1;
  stageFill  = 0;
  prevValid  = true;
}
void stageKick() {
  if (smooth && stageFill == STAGE_LEN) { smoothKick(); return; }
  txWait();
  txBuf(stage[stageHalf], stageFill);
  stageHalf ^= 1;
  stageFill  = 0;
}
// Flush a partially-filled half, in order, and join. Used before anything that reconfigures DMA
// or moves the address window.
void stageDrain() {
  if (holding) {                                     // nothing followed it: send it unblended
    txWait();
    txBuf(stage[stageHalf ^ 1], STAGE_LEN);
    holding = false;
  }
  if (stageFill) stageKick();
  txWait();
  prevValid = false;
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

static uint32_t s_spiHz;                // the LCD SPI clock spi_init() actually set
uint32_t lcdSpiHz() { return s_spiHz; }

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

  // clk_peri (the SPI clock) runs from clk_sys. arduino-pico's set_sys_clock_khz() leaves it on the
  // 48MHz USB PLL, which caps SPI at 24MHz: a full C64 frame then took ~45ms on the wire and the
  // panel showed one frame in three. From clk_sys (250MHz) LCD_SPI_HZ gets its 62.5MHz. This runs
  // first thing in setup(), before the SD card's spi0 is set up; Arduino SPI computes its divider
  // from clock_get_hz(clk_peri) at each transaction anyway.
  if (clock_get_hz(clk_peri) != clock_get_hz(clk_sys))
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));
  s_spiHz = spi_init(LCD_SPI, LCD_SPI_HZ);
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

// Rounded corners. Both calls below walk the same midpoint circle (the one Adafruit_GFX and
// TFT_eSPI draw), so an outline laid over a fill of the same box lands exactly on its edge.
// dx[d] = how far the arc reaches sideways from the corner's centre, d rows above/below it:
// dx[0] = r (the straight side), dx[r] = the start of the straight top/bottom edge.
static const int32_t kMaxCornerR = 31;

static int32_t roundRectRadius(int32_t w, int32_t h, int32_t r) {
  int32_t m = (w < h ? w : h) / 2;
  if (r > m) r = m;
  if (r > kMaxCornerR) r = kMaxCornerR;
  return r < 0 ? 0 : r;
}

static void cornerReach(int32_t r, int8_t *dx) {
  for (int32_t i = 0; i <= r; i++) dx[i] = 0;
  dx[0] = (int8_t)r;
  int32_t f = 1 - r, ddx = 1, ddy = -2 * r, px = 0, py = r;
  while (px < py) {
    if (f >= 0) { py--; ddy += 2; f += ddy; }
    px++; ddx += 2; f += ddx;
    if (px > dx[py]) dx[py] = (int8_t)px;      // octant point (px, py)...
    if (py > dx[px]) dx[px] = (int8_t)py;      // ...and its mirror (py, px)
  }
}

void DisplayGFX::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  r = roundRectRadius(w, h, r);
  if (r == 0) { fillRect(x, y, w, h, color); return; }
  int8_t dx[kMaxCornerR + 1];
  cornerReach(r, dx);
  const int32_t cl = x + r, cr = x + w - 1 - r;      // corner centres, left/right
  const int32_t ct = y + r, cb = y + h - 1 - r;      //                 top/bottom
  // The corner pixels outside the arc are left alone: they belong to whatever is behind the box.
  for (int32_t d = r; d >= 1; d--)
    fillRect(cl - dx[d], ct - d, (cr + dx[d]) - (cl - dx[d]) + 1, 1, color);
  if (cb >= ct) fillRect(x, ct, w, cb - ct + 1, color);
  for (int32_t d = 1; d <= r; d++)
    fillRect(cl - dx[d], cb + d, (cr + dx[d]) - (cl - dx[d]) + 1, 1, color);
}

void DisplayGFX::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  r = roundRectRadius(w, h, r);
  const int32_t cl = x + r, cr = x + w - 1 - r;
  const int32_t ct = y + r, cb = y + h - 1 - r;
  fillRect(cl,        y,         cr - cl + 1, 1, color);   // straight edges
  fillRect(cl,        y + h - 1, cr - cl + 1, 1, color);
  fillRect(x,         ct,        1, cb - ct + 1, color);
  fillRect(x + w - 1, ct,        1, cb - ct + 1, color);
  if (r == 0) return;
  int8_t dx[kMaxCornerR + 1];
  cornerReach(r, dx);
  // Row d of a corner runs from just past where the next row out stops (dx[d+1] + 1) to dx[d],
  // so the curve is gap-free even where it runs flat.
  for (int32_t d = 1; d <= r; d++) {
    int32_t hi = dx[d];
    int32_t lo = (d < r) ? dx[d + 1] + 1 : 0;
    if (lo > hi) lo = hi;
    int32_t n = hi - lo + 1;
    fillRect(cl - hi, ct - d, n, 1, color);
    fillRect(cr + lo, ct - d, n, 1, color);
    fillRect(cl - hi, cb + d, n, 1, color);
    fillRect(cr + lo, cb + d, n, 1, color);
  }
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
// pushImage without the wait: the DMA is still sending `data` when this returns, so the caller must
// leave it alone until pushBusy() is false (pushWait()). Anything else that talks to the panel waits
// for it first. Full rows only (no horizontal clipping).
void DisplayGFX::pushImageAsync(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) {
  pxFlush();
  if (!data || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > DISP_LOGICAL_W || y + h > _logH) {
    pushImage(x, y, w, h, data);
    return;
  }
  panelWindow(x, y + _offY, x + w - 1, y + _offY + h - 1);
  fmt16();
  txBuf(data, (uint32_t)w * (uint32_t)h);
}
bool DisplayGFX::pushBusy() { return s_dma >= 0 && dma_channel_is_busy(s_dma); }
void DisplayGFX::pushWait() { txWait(); }
// Same geometry as pushImage on this board (logical == panel apart from the letterbox offset).
void DisplayGFX::pushPanelBand(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) {
  pushImage(x, y, w, h, data);
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
// RAW panel rows, no letterbox offset: the NES SCREEN: FILL picture (320x300) is taller than the
// logical 320x240, so it streams into the bars too.
void DisplayGFX::setPanelAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h) {
  pxFlush();
  pcd::stageDrain();
  panelWindow(x, y, x + w - 1, y + h - 1);
}
void DisplayGFX::startWrite() {
  pcd::stageFill = 0;                                // nothing carried over from a previous window
  pcd::stageHalf = 0;
  pcd::hsc = _hscale ? 7 : 0;                        // groups restart at the window origin
  pcd::smooth = _smooth;
  pcd::prevValid = pcd::holding = false;
  pcd::rowBlend = 0;
  fmt16();
}
void DisplayGFX::endWrite() {
  pcd::stageDrain();
  pcd::hsc = 0;
  pcd::smooth = false;
  _hscale = _smooth = false;
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
// Fixed-cell bitmap fonts drawn 1:1 by the renderer in src/shared/uitext.h (the desktop backend
// uses the same one, so the two look identical). Font 1 = 6x10, font 3 = 7x13 (the settings file
// list), font 2 = 7x13 bold. Lit pixels go through pxRun(), which merges each glyph row into one
// panel write per horizontal run.
int16_t DisplayGFX::textWidth(const char *s, uint8_t font) {
  return (int16_t)uiTextWidth(s, font);
}

int16_t DisplayGFX::drawString(const char *s, int32_t x, int32_t y, uint8_t font) {
  if (!s) return 0;
  int32_t left, top;
  uiTextOrigin(s, x, y, _datum, font, left, top);
  const int w = uiTextWidth(s, font);
  if (_textHasBg && w > 0) fillRect(left, top, w, uiTextCellH(font), _textBg);
  const uint16_t fg = _textFg;
  uiTextDraw(s, left, top, font, [this, fg](int32_t px, int32_t py) { pxRun(px, py, fg); });
  pxFlush();
  return (int16_t)w;
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

// ============================== read-back =====================================================
// The emulator video area as it is on the panel, for the snapshot thumbnails: always the
// letterboxed 320x240 (rows DISP_OFFSET_Y on), whatever the menu set the logical offset to.
// RAMRD runs at a read clock far below the write one, so the clock drops for it and comes back.
// What it answers -- dummy bytes, 2 or 3 bytes per pixel, RGB or BGR -- differs between ILI948x /
// ST7365 units, so the first read probes it: three known pixels go into the black top bar, are read
// back and matched against each layout, and the bar is painted black again.
#define LCD_READ_HZ 5000000
void printLog(const char *txt);              // interface.cpp
static int8_t s_rdSkip = -1, s_rdBpp = 0;   // dummy bytes, bytes per pixel (0 = not probed / unknown)
static bool s_rdBgr = false;

static void rdBegin(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  uint8_t b[4];
  wrCmd(0x2A);                                                  // CASET
  b[0] = (uint8_t)(x0 >> 8); b[1] = (uint8_t)x0; b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
  wrArgs(b, 4);
  wrCmd(0x2B);                                                  // RASET
  b[0] = (uint8_t)(y0 >> 8); b[1] = (uint8_t)y0; b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
  wrArgs(b, 4);
  spiIdle();
  spi_set_baudrate(LCD_SPI, LCD_READ_HZ);
  wrCmd(0x2E);                                                  // RAMRD
  spiIdle();
}
static void rdEnd() {
  spiIdle();
  spi_set_baudrate(LCD_SPI, s_spiHz);
  // A read only ends on a CS rising edge (a NOP does not), and CS is otherwise held low for good
  // (note 2 at the top): without this pulse the panel stays in read mode and ignores every draw.
  gpio_put(LCD_CS_PIN, 1);
  busy_wait_us_32(2);
  gpio_put(LCD_CS_PIN, 0);
  busy_wait_us_32(1);
  wrCmd(0x00);                                                  // NOP
}
static bool rdProbe() {
  if (s_rdSkip >= 0) return s_rdBpp != 0;
  static const uint16_t pat[3] = { 0xF800, 0x07E0, 0x001F };    // red, green, blue
  for (int i = 0; i < 3; i++) panelFillRaw(i, 0, 1, 1, pat[i]);
  uint8_t r[12];
  rdBegin(0, 0, 2, 0);
  spi_read_blocking(LCD_SPI, 0, r, sizeof(r));
  rdEnd();
  panelFillRaw(0, 0, 3, 1, TFT_BLACK);
  s_rdSkip = 0; s_rdBpp = 0;
  for (int d = 0; d <= 3 && !s_rdBpp; d++) {
    const uint8_t *p = r + d;
    uint8_t m[9];
    for (int i = 0; i < 9; i++) m[i] = p[i] & 0xF8;
    if      (m[0] == 0xF8 && !m[1] && !m[2] && !m[3] && m[4] == 0xF8 && !m[5] && !m[6] && !m[7] && m[8] == 0xF8) { s_rdBpp = 3; s_rdBgr = false; }
    else if (!m[0] && !m[1] && m[2] == 0xF8 && !m[3] && m[4] == 0xF8 && !m[5] && m[6] == 0xF8 && !m[7] && !m[8]) { s_rdBpp = 3; s_rdBgr = true; }
    else if (p[0] == 0xF8 && p[1] == 0x00 && p[2] == 0x07 && p[3] == 0xE0 && p[4] == 0x00 && p[5] == 0x1F)        { s_rdBpp = 2; s_rdBgr = false; }
    else if (p[0] == 0x00 && p[1] == 0x1F && p[2] == 0x07 && p[3] == 0xE0 && p[4] == 0xF8 && p[5] == 0x00)        { s_rdBpp = 2; s_rdBgr = true; }
    if (s_rdBpp) s_rdSkip = (int8_t)d;
  }
  char t[112];
  snprintf(t, sizeof(t), "LCD read probe: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X -> %s",
           r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11],
           !s_rdBpp ? "unknown" : s_rdBpp == 3 ? (s_rdBgr ? "3B BGR" : "3B RGB") : (s_rdBgr ? "2B BGR" : "2B RGB"));
  printLog(t);
  if (s_rdBpp) { snprintf(t, sizeof(t), "LCD read probe: %d dummy byte(s)", s_rdSkip); printLog(t); }
  return s_rdBpp != 0;
}

bool DisplayGFX::readVideo(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t *buf) {
  pxFlush();
  if (!buf || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > DISP_LOGICAL_W || y + h > DISP_LOGICAL_H) return false;
  pcd::stageDrain();
  txWait();
  if (!rdProbe()) return false;                                 // unknown answer: no picture beats a wrong one
  const int32_t py = y + DISP_OFFSET_Y;
  rdBegin(x, py, x + w - 1, py + h - 1);
  uint8_t c[96];
  if (s_rdSkip) spi_read_blocking(LCD_SPI, 0, c, (size_t)s_rdSkip);
  const uint32_t n = (uint32_t)w * (uint32_t)h, bpp = (uint32_t)s_rdBpp, per = sizeof(c) / 6;
  for (uint32_t i = 0; i < n; ) {
    const uint32_t k = n - i > per ? per : n - i;
    spi_read_blocking(LCD_SPI, 0, c, k * bpp);
    for (uint32_t j = 0; j < k; j++, i++) {
      const uint8_t *q = c + j * bpp;
      uint16_t v;
      if (bpp == 3) {
        const uint8_t r8 = s_rdBgr ? q[2] : q[0], b8 = s_rdBgr ? q[0] : q[2];
        v = (uint16_t)(((r8 & 0xF8) << 8) | ((q[1] & 0xFC) << 3) | (b8 >> 3));
      } else {
        v = (uint16_t)((q[0] << 8) | q[1]);
        if (s_rdBgr) v = (uint16_t)((v >> 11) | (v & 0x07E0) | (v << 11));
      }
      buf[i] = v;
    }
  }
  rdEnd();
  return true;
}
bool displayReadVideo(int x, int y, int w, int h, uint16_t *buf) { return tft.readVideo(x, y, w, h, buf); }

#endif // BOARD_PICOCALC
