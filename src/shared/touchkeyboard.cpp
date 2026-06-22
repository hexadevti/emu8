#include "../../emu.h"

// touchkeyboard.ino - On-screen virtual keyboard driven by the CYD touch panel.
//
// The XPT2046 touch controller shares the TFT's SPI bus (TOUCH_CS is defined in
// User_Setup.h), so we read it with TFT_eSPI's built-in getTouchRaw()/getTouchRawZ().
// All touch reads and keyboard drawing run on core 0 from inside renderLoop()
// (see video.ino) because only that task is allowed to drive the TFT/SPI bus.
//
// Behaviour:
//   * Tap anywhere while the keyboard is hidden  -> show it.
//   * Tap a key                                  -> inject that Apple II keystroke.
//   * Tap CTL / SHF                              -> sticky modifier toggles.
//   * Tap the "X" key, or anywhere above the keyboard -> hide it.
//
// Keystrokes are delivered exactly like the PS/2 path: we set the global keymem
// to an Apple keycode with the high ("key ready") bit set; the CPU reads it at
// $C000 and clears it via the $C010 strobe.
//
// While the keyboard is open the Apple raster is squeezed into the top OSK_Y rows
// (see oskRasterTop()/oskRasterHeight(), used by renderLoop) so the emulated
// screen stays live above the keyboard. The keyboard owns the bottom rows, which
// the raster never touches, so it is drawn once per change with no flicker.

// ---------------------------------------------------------------------------
// Touch calibration (XPT2046 raw -> screen, for tft.setRotation(3), 320x240).
// If taps land in the wrong spot: uncomment OSK_TOUCH_DEBUG, open the serial
// monitor, touch each screen corner and note the raw x/y, then update the
// MIN/MAX values. If the axes feel rotated/mirrored, flip the SWAP/INV flags.
// ---------------------------------------------------------------------------
#if BOARD_TOUCH_VIA_TFT
// ESP32 CYD: XPT2046 read via TFT_eSPI getTouchRaw() (rotation 3, 320x240 panel).
#define OSK_TS_MINX     350
#define OSK_TS_MAXX     3900
#define OSK_TS_MINY     280
#define OSK_TS_MAXY     3850
#define OSK_TS_SWAP_XY  0      // XPT2046 X/Y axes are swapped relative to the display
#define OSK_TS_INVX     1      // mirror horizontally
#define OSK_TS_INVY     1      // mirror vertically
#define OSK_TS_ZTHRESH  350    // minimum pressure to count as a touch
#else
// JC4827W543: raw XPT2046 on the shared HSPI bus, mapped onto the centered 320x240 logical
// area of the 480x272 panel. Calibrated from the raw values at the four physical panel corners:
// rawX 205..3870 maps RIGHT..LEFT and rawY 400..3725 maps BOTTOM..TOP, i.e. both axes are
// mirrored relative to the display (INVX/INVY=1). (pressure here is z1 + 4095 - z2.)
// Re-run with OSK_TOUCH_DEBUG uncommented if a future panel differs.
#define OSK_TS_MINX     205
#define OSK_TS_MAXX     3870
#define OSK_TS_MINY     400
#define OSK_TS_MAXY     3725
#define OSK_TS_SWAP_XY  0
#define OSK_TS_INVX     1
#define OSK_TS_INVY     1
#define OSK_TS_ZTHRESH  400
#endif
// #define OSK_TOUCH_DEBUG      // uncomment to print raw + mapped touch coords (calibration aid)

// ---------------------------------------------------------------------------
// Layout geometry (screen is 320 wide x 240 tall in rotation 3)
// ---------------------------------------------------------------------------
#define OSK_Y      112         // top of the keyboard overlay (Apple: 5 rows 112..237)
#define OSK_C64_TOP 87         // C64 keyboard top: an extra function-key row above OSK_Y
#define OSK_ROWH   25
#define OSK_GAP    1

// Key actions
#define OSK_ACT_CHAR   0
#define OSK_ACT_SHIFT  1
#define OSK_ACT_CTRL   2
#define OSK_ACT_HIDE   3
#define OSK_ACT_RETURN 4
#define OSK_ACT_SPACE  5
#define OSK_ACT_LEFT   6
#define OSK_ACT_RIGHT  7
#define OSK_ACT_ESC    8
#define OSK_ACT_TAB    9
#define OSK_ACT_DEL    10
#define OSK_ACT_UP     11
#define OSK_ACT_DOWN   12
#define OSK_ACT_RUNSTOP 13   // C64 RUN/STOP
#define OSK_ACT_MENU    14   // open the settings window (C64)
#define OSK_ACT_F1      15   // C64 function keys (matrix col0; SHIFT gives F2/F4/F6/F8)
#define OSK_ACT_F3      16
#define OSK_ACT_F5      17
#define OSK_ACT_F7      18
#define OSK_ACT_RESET   19   // Apple II RESET key (soft reset only with CTRL held)
#define OSK_ACT_USR1    20   // Apple II right-side panel buttons reserved for future actions
#define OSK_ACT_USR2    21
#define OSK_ACT_USR3    22

#define OSK_MAX_KEYS 80

struct OskKey {
  int16_t x, y, w, h;
  char    norm;   // character when un-shifted (OSK_ACT_CHAR only)
  char    shft;   // character when shifted
  uint8_t act;
  int8_t  mcol, mrow;  // C64 keyboard-matrix position (PA col, PB row); -1 = none/Apple
};

static OskKey oskKeys[OSK_MAX_KEYS];
static int    oskKeyCount   = 0;

static bool osk_visible     = false;
static bool osk_dirty       = false;
static bool osk_prevDown    = false;
static bool osk_waitRelease = false;
static bool osk_shift       = false;
static bool osk_ctrl        = false;
static int  osk_pressedIdx  = -1;

// ---------------------------------------------------------------------------
// Layout construction
// ---------------------------------------------------------------------------
static void oskAddKey(int16_t x, int16_t y, int16_t w, int16_t h,
                      char n, char s, uint8_t act)
{
  if (oskKeyCount >= OSK_MAX_KEYS) return;
  oskKeys[oskKeyCount++] = { x, y, w, h, n, s, act, -1, -1 };
}

// C64: add a key carrying its keyboard-matrix position (col=PA line, row=PB line).
static void oskAddKeyM(int16_t x, int16_t y, int16_t w, int16_t h,
                       char n, char s, uint8_t act, int8_t col, int8_t row)
{
  if (oskKeyCount >= OSK_MAX_KEYS) return;
  oskKeys[oskKeyCount++] = { x, y, w, h, n, s, act, col, row };
}

// C64: tile a row of character keys, each with its matrix (col,row), across [x0,x0+totalW).
static void oskAddRowC64(const char *labels, const int8_t *cols, const int8_t *rows,
                         int16_t y, int16_t x0, int16_t totalW)
{
  int n = 0; while (labels[n]) n++;
  for (int i = 0; i < n; i++) {
    int16_t kx = x0 + (int16_t)((long)totalW * i / n);
    int16_t kw = (int16_t)(x0 + (long)totalW * (i + 1) / n) - kx;
    oskAddKeyM(kx, y, kw, OSK_ROWH, labels[i], labels[i], OSK_ACT_CHAR, cols[i], rows[i]);
  }
}

// Tile strlen(norm) character keys exactly across [x0, x0+totalW) so the row
// always fills the width regardless of key count (widths vary by +/-1 px).
static void oskAddRowFit(const char *norm, const char *shft, int16_t y, int16_t x0, int16_t totalW)
{
  int n = 0;
  while (norm[n]) n++;
  for (int i = 0; i < n; i++) {
    int16_t kx = x0 + (int16_t)((long)totalW * i / n);
    int16_t kw = (int16_t)(x0 + (long)totalW * (i + 1) / n) - kx;
    oskAddKey(kx, y, kw, OSK_ROWH, norm[i], shft[i], OSK_ACT_CHAR);
  }
}

// Top of the on-screen keyboard. Both Apple and C64 are 5 rows starting at OSK_Y now (the C64's
// function keys moved into the right-hand button column, freeing the old top row for more raster).
static int oskTopY() { return OSK_Y; }

// C64 keyboard, mapped to the CIA1 matrix (col=PA line, row=PB line). SHIFT is a sticky
// modifier applied to the next key; cursor LEFT/UP send SHIFT+CRSR. RUN/STOP, RETURN,
// DEL(=INST/DEL), SPACE are matrix keys. Like the Apple layout, the keys are squeezed into the
// left KBW px and a right-hand button column holds MENU + the function keys F1/F3/F5/F7 (all in
// matrix column 0; SHIFT turns them into F2/F4/F6/F8). Close the keyboard by tapping above it.
static void oskBuildLayoutC64()
{
  oskKeyCount = 0;
  const int16_t r1y = OSK_Y, r2y = OSK_Y + OSK_ROWH, r3y = OSK_Y + 2 * OSK_ROWH,
                r4y = OSK_Y + 3 * OSK_ROWH, r5y = OSK_Y + 4 * OSK_ROWH;

  const int16_t KBW  = 264;                 // keyboard area width (right column gets the rest)
  const int16_t COLX = KBW;
  const int16_t COLW = 320 - KBW;           // 56 px

  static const int8_t c1[] = {7,7,1,1,2,2,3,3,4,4}, q1[] = {0,3,0,3,0,3,0,3,0,3};
  oskAddRowC64("1234567890", c1, q1, r1y, 0, 238);
  oskAddKeyM(238, r1y, 26, OSK_ROWH, 0, 0, OSK_ACT_DEL, 0, 0);

  static const int8_t c2[] = {7,1,1,2,2,3,3,4,4,5}, q2[] = {6,1,6,1,6,1,6,1,6,1};
  oskAddRowC64("QWERTYUIOP", c2, q2, r2y, 0, 264);

  static const int8_t c3[] = {1,1,2,2,3,3,4,4,5}, q3[] = {2,5,2,5,2,5,2,5,2};
  oskAddRowC64("ASDFGHJKL", c3, q3, r3y, 0, 229);
  oskAddKeyM(229, r3y, 35, OSK_ROWH, 0, 0, OSK_ACT_RETURN, 0, 1);

  static const int8_t c4[] = {1,2,2,3,3,4,4,5,5,6}, q4[] = {4,7,4,7,4,7,4,7,4,7};
  oskAddRowC64("ZXCVBNM,./", c4, q4, r4y, 0, 264);

  // Row 5: SHIFT, RUN/STOP, a roomy SPACE, cursors (MENU + the X close key are gone).
  int16_t x = 0;
  oskAddKey (x, r5y, 36, OSK_ROWH, 0,   0,   OSK_ACT_SHIFT);            x += 36;
  oskAddKeyM(x, r5y, 36, OSK_ROWH, 0,   0,   OSK_ACT_RUNSTOP, 7, 7);   x += 36;
  oskAddKeyM(x, r5y, 88, OSK_ROWH, ' ', ' ', OSK_ACT_SPACE,   7, 4);   x += 88;
  oskAddKeyM(x, r5y, 26, OSK_ROWH, 0,   0,   OSK_ACT_LEFT,    0, 2);   x += 26;
  oskAddKeyM(x, r5y, 26, OSK_ROWH, 0,   0,   OSK_ACT_DOWN,    0, 7);   x += 26;
  oskAddKeyM(x, r5y, 26, OSK_ROWH, 0,   0,   OSK_ACT_UP,      0, 7);   x += 26;
  oskAddKeyM(x, r5y, 26, OSK_ROWH, 0,   0,   OSK_ACT_RIGHT,   0, 2);   x += 26;

  // Right-hand button column: MENU + function keys (one per row).
  oskAddKey (COLX, r1y, COLW, OSK_ROWH, 0, 0, OSK_ACT_MENU);
  oskAddKeyM(COLX, r2y, COLW, OSK_ROWH, 0, 0, OSK_ACT_F1, 0, 4);
  oskAddKeyM(COLX, r3y, COLW, OSK_ROWH, 0, 0, OSK_ACT_F3, 0, 5);
  oskAddKeyM(COLX, r4y, COLW, OSK_ROWH, 0, 0, OSK_ACT_F5, 0, 6);
  oskAddKeyM(COLX, r5y, COLW, OSK_ROWH, 0, 0, OSK_ACT_F7, 0, 3);
}

void oskBuildLayout()
{
  if (currentPlatform == PLATFORM_C64) { oskBuildLayoutC64(); return; }

  oskKeyCount = 0;

  const int16_t r1y = OSK_Y;                // 112
  const int16_t r2y = OSK_Y + OSK_ROWH;     // 137
  const int16_t r3y = OSK_Y + 2 * OSK_ROWH; // 162
  const int16_t r4y = OSK_Y + 3 * OSK_ROWH; // 187
  const int16_t r5y = OSK_Y + 4 * OSK_ROWH; // 212

  // The character keys are squeezed into the left KBW px so a column of side buttons (Menu, Reset,
  // and three reserved B1/B2/B3) fills the rest on the right, one button per keyboard row.
  const int16_t KBW    = 264;               // keyboard area width (was the full 320)
  const int16_t COLX   = KBW;               // left edge of the right-hand button column
  const int16_t COLW   = 320 - KBW;         // 56 px

  // Row 1: digits / symbols, then DEL
  oskAddRowFit("1234567890-=", "!@#$%^&*()_+", r1y, 0, 238);
  oskAddKey(238, r1y, 26, OSK_ROWH, 0, 0, OSK_ACT_DEL);

  // Row 2: TAB, then QWERTY row with [ ]
  oskAddKey(0, r2y, 26, OSK_ROWH, 0, 0, OSK_ACT_TAB);
  oskAddRowFit("QWERTYUIOP[]", "QWERTYUIOP{}", r2y, 26, 238);

  // Row 3: ASDF row with ; ' `
  oskAddRowFit("ASDFGHJKL;'`", "ASDFGHJKL:\"~", r3y, 0, KBW);

  // Row 4: \ then ZXCV row with , . /
  oskAddRowFit("\\ZXCVBNM,./", "|ZXCVBNM<>?", r4y, 0, KBW);

  // Row 5: modifiers / specials (explicit widths summing to KBW=264). MENU/RESET/RETURN/ESC all
  // live in the right column now, so this row carries only CTRL/SHIFT plus a big SPACE and cursors.
  int16_t x = 0;
  oskAddKey(x, r5y, 30, OSK_ROWH, 0,   0,   OSK_ACT_CTRL);   x += 30;
  oskAddKey(x, r5y, 30, OSK_ROWH, 0,   0,   OSK_ACT_SHIFT);  x += 30;
  oskAddKey(x, r5y, 96, OSK_ROWH, ' ', ' ', OSK_ACT_SPACE);  x += 96;   // roomy spacebar
  oskAddKey(x, r5y, 27, OSK_ROWH, 0,   0,   OSK_ACT_LEFT);   x += 27;
  oskAddKey(x, r5y, 27, OSK_ROWH, 0,   0,   OSK_ACT_DOWN);   x += 27;
  oskAddKey(x, r5y, 27, OSK_ROWH, 0,   0,   OSK_ACT_UP);     x += 27;
  oskAddKey(x, r5y, 27, OSK_ROWH, 0,   0,   OSK_ACT_RIGHT);  x += 27;

  // Right-hand button column: one per row -> MENU, RESET, RETURN, ESC are live; B3 is a reserved
  // placeholder (shows a press for now — wire it up in oskHandleKey later).
  // Hide the keyboard by tapping above it (as before).
  oskAddKey(COLX, r1y, COLW, OSK_ROWH, 0, 0, OSK_ACT_MENU);
  oskAddKey(COLX, r2y, COLW, OSK_ROWH, 0, 0, OSK_ACT_RESET);
  oskAddKey(COLX, r3y, COLW, OSK_ROWH, 0, 0, OSK_ACT_RETURN);
  oskAddKey(COLX, r4y, COLW, OSK_ROWH, 0, 0, OSK_ACT_ESC);
  oskAddKey(COLX, r5y, COLW, OSK_ROWH, 0, 0, OSK_ACT_USR3);
}

void oskSetup()
{
  oskBuildLayout();
  Serial.printf("OSK build v3 (modern options UI): %d keys\n", oskKeyCount);
  printLog("Touch keyboard ready (tap the screen to open).");
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void oskKeyLabel(int i, char *out)
{
  const OskKey &k = oskKeys[i];
  switch (k.act) {
    case OSK_ACT_SHIFT:  strcpy(out, "SHF");   break;
    case OSK_ACT_CTRL:   strcpy(out, "CTL");   break;
    case OSK_ACT_HIDE:   strcpy(out, "X");     break;
    case OSK_ACT_RETURN: strcpy(out, "RET");   break;
    case OSK_ACT_SPACE:  strcpy(out, "SPACE"); break;
    case OSK_ACT_LEFT:   strcpy(out, "<");     break;
    case OSK_ACT_RIGHT:  strcpy(out, ">");     break;
    case OSK_ACT_UP:     strcpy(out, "^");     break;
    case OSK_ACT_DOWN:   strcpy(out, "v");     break;
    case OSK_ACT_ESC:    strcpy(out, "ESC");   break;
    case OSK_ACT_TAB:    strcpy(out, "TAB");   break;
    case OSK_ACT_DEL:    strcpy(out, "DEL");   break;
    case OSK_ACT_RUNSTOP: strcpy(out, "R/S");  break;
    case OSK_ACT_MENU:   strcpy(out, "MENU");  break;
    case OSK_ACT_F1:     strcpy(out, "F1/2");  break;
    case OSK_ACT_F3:     strcpy(out, "F3/4");  break;
    case OSK_ACT_F5:     strcpy(out, "F5/6");  break;
    case OSK_ACT_F7:     strcpy(out, "F7/8");  break;
    case OSK_ACT_RESET:  strcpy(out, "RESET"); break;
    case OSK_ACT_USR1:   strcpy(out, "B1");    break;
    case OSK_ACT_USR2:   strcpy(out, "B2");    break;
    case OSK_ACT_USR3:   strcpy(out, "B3");    break;
    default:             out[0] = osk_shift ? k.shft : k.norm; out[1] = 0; break;
  }
}

static void oskDrawKey(int i, bool pressed)
{
  const OskKey &k = oskKeys[i];
  uint16_t face;
  if (pressed)                                       face = tft.color565(0, 180, 0);
  else if (k.act == OSK_ACT_SHIFT && osk_shift)      face = tft.color565(0, 120, 200);
  else if (k.act == OSK_ACT_CTRL  && osk_ctrl)       face = tft.color565(0, 120, 200);
  else if (k.act == OSK_ACT_HIDE)                    face = tft.color565(140, 30, 30);
  else if (k.act == OSK_ACT_RESET)                   face = tft.color565(140, 30, 30);
  else                                               face = tft.color565(45, 45, 45);

  int16_t x = k.x + OSK_GAP, y = k.y + OSK_GAP;
  int16_t w = k.w - 2 * OSK_GAP, h = k.h - 2 * OSK_GAP;
  tft.fillRoundRect(x, y, w, h, 4, face);
  tft.drawRoundRect(x, y, w, h, 4, tft.color565(90, 90, 90));

  char lbl[8];
  oskKeyLabel(i, lbl);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, face);
  tft.drawString(lbl, k.x + k.w / 2, k.y + k.h / 2, 2);
}

void oskRender()
{
  if (!osk_dirty) return;
  int top = oskTopY();
  tft.fillRect(0, top, 320, 240 - top, TFT_BLACK);
  for (int i = 0; i < oskKeyCount; i++)
    oskDrawKey(i, false);
  osk_dirty = false;
}

bool oskActive()
{
  return osk_visible;
}

// Raster geometry used by renderLoop (video.ino). When the keyboard is open the
// 192-line Apple raster is scaled into the top OSK_Y rows; otherwise it keeps its
// normal centred 192-row layout. The vertical scaler produces exactly this many
// output lines, so the setAddrWindow height must match.
int oskRasterTop()
{
  return osk_visible ? 0 : 24;
}

int oskRasterHeight()
{
  return osk_visible ? oskTopY() : 192;   // C64 flattens its screen into the rows above the kbd
}

// ---------------------------------------------------------------------------
// Touch reading
// ---------------------------------------------------------------------------
// Shared, calibrated touch reader (also used by the options UI, optionsui.ino).
// Returns false when nothing is pressed; otherwise fills screen coords (0..319, 0..239).
#if !BOARD_TOUCH_VIA_TFT
// Raw XPT2046 read over the shared HSPI bus (JC4827W543). The touch controller shares the SD
// card's SCK/MISO/MOSI and has its own CS (TOUCH_CS_PIN); SPI transactions serialize access so
// it coexists with the SD card. Each channel is a single-shot read: send the control byte, then
// clock out the 12-bit result (standard XPT2046 sequence).
static int xptChan(uint8_t ctrl)
{
  hspi.transfer(ctrl);
  uint8_t hi = hspi.transfer(0), lo = hspi.transfer(0);
  return ((hi << 8) | lo) >> 3;   // 12-bit result
}

static bool xptRead(uint16_t *x, uint16_t *y, int *z)
{
  static bool csInit = false;
  if (!csInit) { pinMode(TOUCH_CS_PIN, OUTPUT); digitalWrite(TOUCH_CS_PIN, HIGH); csInit = true; }

  busTake();   // exclusive HSPI bus access vs any in-flight SD read/write
  hspi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS_PIN, LOW);
  int z1 = xptChan(0xB1);
  int z2 = xptChan(0xC1);
  int zz = z1 + (4095 - z2);     // touch pressure proxy (higher = harder press)
  uint16_t rx = 0, ry = 0;
  if (zz >= OSK_TS_ZTHRESH) {
    xptChan(0xD1);              // first reading is noisy; discard
    rx = (xptChan(0xD1) + xptChan(0xD1)) / 2;   // X channel
    ry = (xptChan(0x91) + xptChan(0x91)) / 2;   // Y channel
  }
  xptChan(0x90);                // power-down conversion
  digitalWrite(TOUCH_CS_PIN, HIGH);
  hspi.endTransaction();
  busGive();

  *x = rx; *y = ry; *z = zz;
  return zz >= OSK_TS_ZTHRESH;
}
#endif

bool touchRead(int16_t *sx, int16_t *sy)
{
  uint16_t rx, ry;
#if BOARD_TOUCH_VIA_TFT
  if (tft.getTouchRawZ() < OSK_TS_ZTHRESH) return false;
  tft.getTouchRaw(&rx, &ry);
#else
  // Throttle XPT2046 reads: they share the SD HSPI bus, and polling every render frame (~60 Hz)
  // disrupted SD / the USB host. ~25 Hz is ample for a touch UI; return the cached state between.
  static uint32_t touchLastMs = 0; static bool touchLastDown = false;
  static int16_t touchLastSx = 0, touchLastSy = 0;
  uint32_t touchNow = millis();
  if (touchNow - touchLastMs < 40) { *sx = touchLastSx; *sy = touchLastSy; return touchLastDown; }
  touchLastMs = touchNow;
  int rz;
  if (!xptRead(&rx, &ry, &rz)) { touchLastDown = false; return false; }
#endif

#ifdef OSK_TOUCH_DEBUG
  Serial.printf("touch raw x=%u y=%u\n", rx, ry);
#endif

  float fx = (constrain((int)rx, OSK_TS_MINX, OSK_TS_MAXX) - OSK_TS_MINX) /
             (float)(OSK_TS_MAXX - OSK_TS_MINX);
  float fy = (constrain((int)ry, OSK_TS_MINY, OSK_TS_MAXY) - OSK_TS_MINY) /
             (float)(OSK_TS_MAXY - OSK_TS_MINY);
  if (OSK_TS_INVX) fx = 1.0f - fx;
  if (OSK_TS_INVY) fy = 1.0f - fy;

  float u = OSK_TS_SWAP_XY ? fy : fx;   // along the wide axis
  float v = OSK_TS_SWAP_XY ? fx : fy;   // along the tall axis

  // Touch maps to the 320x240 logical UI space. On the JC4827W543 the UI is drawn SCALED to fill
  // the 480x272 panel but still hit-tests in 320x240, so the same 0..319/0..239 mapping is used on
  // both boards (the full-panel calibration already spread u/v across the whole touch area).
  *sx = (int16_t)constrain((int)(u * 320.0f), 0, 319);
  *sy = (int16_t)constrain((int)(v * 240.0f), 0, 239);

#ifdef OSK_TOUCH_DEBUG
  Serial.printf("touch screen x=%d y=%d\n", *sx, *sy);
#endif
#if !BOARD_TOUCH_VIA_TFT
  touchLastDown = true; touchLastSx = *sx; touchLastSy = *sy;   // cache for the throttle window
#endif
  return true;
}

static int oskHitTest(int16_t sx, int16_t sy)
{
  for (int i = 0; i < oskKeyCount; i++) {
    const OskKey &k = oskKeys[i];
    if (sx >= k.x && sx < k.x + k.w && sy >= k.y && sy < k.y + k.h)
      return i;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Keystroke injection (same mechanism as the PS/2 path: write keymem)
// ---------------------------------------------------------------------------
static void oskInject(int i)
{
  const OskKey &k = oskKeys[i];
  uint8_t code;
  switch (k.act) {
    case OSK_ACT_RETURN: code = 0x8D; break;
    case OSK_ACT_SPACE:  code = 0xA0; break;
    case OSK_ACT_LEFT:   code = 0x88; break;  // also acts as backspace
    case OSK_ACT_RIGHT:  code = 0x95; break;
    case OSK_ACT_UP:     code = 0x8B; break;
    case OSK_ACT_DOWN:   code = 0x8A; break;
    case OSK_ACT_ESC:    code = 0x9B; break;
    case OSK_ACT_TAB:    code = 0x89; break;
    case OSK_ACT_DEL:    code = 0xFF; break;
    default: {
      char c = osk_shift ? k.shft : k.norm;
      if (c >= 'a' && c <= 'z') c -= 32;       // Apple keyboard is upper-case
      code = (uint8_t)c | 0x80;
      if (osk_ctrl && ((c >= '@' && c <= '_'))) // Ctrl masks to 0x00-0x1F
        code = ((uint8_t)c & 0x1F) | 0x80;
      break;
    }
  }
  keymem = (char)code;
}

static void oskHide()
{
  osk_visible     = false;
  osk_dirty       = false;
  osk_pressedIdx  = -1;
  osk_shift       = false;
  osk_ctrl        = false;
  osk_waitRelease = false;
  // Wipe the overlay; renderLoop repaints the Apple raster from the next frame.
  tft.fillScreen(TFT_BLACK);
}

// C64: press a key into the CIA1 matrix (+ SHIFT when the sticky shift is on, or for the
// LEFT/UP cursor keys which are SHIFT+CRSR). Released in oskPoll via oskC64Up().
static void oskC64Down(int i)
{
  const OskKey &k = oskKeys[i];
  if (k.act == OSK_ACT_HIDE)  { oskHide(); return; }
  if (k.act == OSK_ACT_MENU)  { oskHide(); showHideOptionsWindow(); return; }
  if (k.act == OSK_ACT_SHIFT) { osk_shift = !osk_shift; osk_dirty = true; oskRender(); return; }
  if (k.mcol >= 0) c64KeyMatrix(k.mrow, k.mcol, true);
  bool autoShift = (k.act == OSK_ACT_LEFT || k.act == OSK_ACT_UP);
  if (osk_shift || autoShift) c64KeyMatrix(7, 1, true);   // left SHIFT (PA1,PB7)
  osk_pressedIdx = i;
  oskDrawKey(i, true);
}

static void oskC64Up(int i)
{
  const OskKey &k = oskKeys[i];
  if (k.mcol >= 0) c64KeyMatrix(k.mrow, k.mcol, false);
  c64KeyMatrix(7, 1, false);   // release SHIFT (re-applied per key while sticky-shift is on)
}

static void oskHandleKey(int i)
{
  if (currentPlatform == PLATFORM_C64) { oskC64Down(i); return; }

  const OskKey &k = oskKeys[i];
  switch (k.act) {
    case OSK_ACT_HIDE:
      oskHide();
      break;
    case OSK_ACT_MENU:                 // open the settings window from the keyboard
      oskHide();
      showHideOptionsWindow();
      return;
    case OSK_ACT_SHIFT:
      osk_shift = !osk_shift;
      osk_dirty = true;          // labels (number row symbols) change with shift
      oskRender();
      break;
    case OSK_ACT_CTRL:
      osk_ctrl = !osk_ctrl;
      oskDrawKey(i, false);
      break;
    case OSK_ACT_RESET:
      // Like a real Apple II: RESET alone does nothing; CTRL+RESET is a soft reset.
      if (osk_ctrl) { oskHide(); cpuReset(); }
      else          { oskDrawKey(i, true); osk_pressedIdx = i; }   // just show the press
      break;
    case OSK_ACT_USR1:                 // reserved side-panel buttons: no action wired up yet
    case OSK_ACT_USR2:
    case OSK_ACT_USR3:
      oskDrawKey(i, true);
      osk_pressedIdx = i;
      break;
    case OSK_ACT_ESC:
      if (osk_ctrl) {                 // Ctrl+Esc opens the settings menu (like PS/2)
        oskHide();
        showHideOptionsWindow();
        return;
      }
      oskInject(i);
      osk_pressedIdx = i;
      oskDrawKey(i, true);
      break;
    default:
      oskInject(i);
      osk_pressedIdx = i;
      oskDrawKey(i, true);
      break;
  }
}

// ---------------------------------------------------------------------------
// Per-frame service (called from renderLoop on core 0)
// ---------------------------------------------------------------------------
void oskPoll()
{
  if (OptionsWindow || DebugWindow) {         // the menu/debugger owns the screen
    osk_visible    = false;                   // dismiss the keyboard; it repaints fully
    osk_pressedIdx = -1;
    osk_shift      = false;
    osk_ctrl       = false;
    return;
  }

  int16_t sx = 0, sy = 0;
  bool down = touchRead(&sx, &sy);

  // NES/Atari have no use for the on-screen keyboard (the buttons are game controls), so a
  // screen tap opens the settings menu directly. oskIgnoreCurrentTouch() on close stops the
  // lingering finger from immediately reopening it.
  if (currentPlatform == PLATFORM_NES || currentPlatform == PLATFORM_ATARI) {
    if (down && !osk_prevDown) showHideOptionsWindow();
    osk_prevDown = down;
    return;
  }

  if (!osk_visible) {
    if (down && !osk_prevDown) {              // first contact opens the keyboard
      osk_visible     = true;
      osk_dirty       = true;
      osk_waitRelease = true;                 // ignore presses until this tap is released
    }
    osk_prevDown = down;
    return;
  }

  if (osk_waitRelease) {                       // wait for the opening tap to lift
    if (!down) osk_waitRelease = false;
    osk_prevDown = down;
    return;
  }

  if (down && !osk_prevDown) {                 // rising edge = a fresh tap
    if (sy < oskTopY()) {                      // tapped above the keyboard -> close
      oskHide();
      osk_prevDown = down;
      return;
    }
    int i = oskHitTest(sx, sy);
    if (i >= 0) oskHandleKey(i);
  }

  if (!down && osk_pressedIdx >= 0) {          // released -> clear the press highlight
    int p = osk_pressedIdx;
    if (currentPlatform == PLATFORM_C64) oskC64Up(p);   // release the matrix bits
    osk_pressedIdx = -1;
    oskDrawKey(p, false);
  }

  osk_prevDown = down;
}

// Called when another full-screen UI (e.g. the settings window) closes via a touch.
// The finger is usually still on the glass, so mark the touch as already-seen: that
// stops the lingering contact from being read as a fresh tap that opens the keyboard.
void oskIgnoreCurrentTouch()
{
  osk_visible  = false;
  osk_prevDown = true;   // require a release before the next tap can open the keyboard
}
