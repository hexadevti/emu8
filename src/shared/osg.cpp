// osg.cpp - On-Screen Gamepad for the touch-only game consoles on the JC1060P470 (ESP32-P4).
//
// NES / Atari 2600 / SMS have no on-screen keyboard, so on this board a translucent virtual
// controller is overlaid on the game: a mobile-style FLOATING analog stick on the left half (it
// re-centres wherever you first touch after lifting; drag from there to pick an 8-way direction;
// it springs back to centre on release) and the console's action buttons on the right. It is drawn
// into the SAME transparent overlay buffer the keyboard uses (oskOverlayBegin/End -> blended 50/50
// in flushDSI). Touches drive the shared joyX/joyY/Pb0-3 + applyPlatformInput(), like the USB pad.
//
// To stay light it only renders the EXTREMES (the ball snaps to one of the 8 directions) and only
// redraws the overlay when the direction / buttons / stick centre actually change. Multi-touch
// (GT911) lets you hold a direction AND press a button at once. Keyboard platforms keep the OSK.

#include "../../emu.h"

#if BOARD_PANEL_DSI

int gt911ReadPoints(int16_t *xs, int16_t *ys, int maxPts);   // multi-touch reader (touchkeyboard.cpp)

// --- the floating stick lives in the LEFT region (logical 320x240); buttons are on the right ---
#define STK_ZONE_X 156     // a contact with logical x < this drives the stick (re-centres on it)
#define STK_DZ     5       // logical dead zone before a direction registers (small = very responsive)
// rendered as TRUE circles in PANEL pixels (the UI-scaled path would make them oval):
#define STK_RING_R 100     // outer ring radius
#define STK_BALL_R 46      // inner ball radius
#define STK_TRAVEL 50      // how far the ball pushes toward the held direction

// --- action buttons. pb: 0..3 -> Pb0-3 (mapped per console by applyPlatformInput); -1 = MENU; -2 = PAUSE ---
struct OsgBtn { int16_t x, y, w, h; int8_t pb; const char *label; };
#define OSG_MENU  (-1)
#define OSG_PAUSE (-2)

static const OsgBtn NES_BTNS[] = {
  { 262, 144, 50, 50, 0, "A" }, { 206, 168, 48, 48, 1, "B" },
  { 160, 218, 46, 16, 2, "SEL" }, { 212, 218, 52, 16, 3, "START" },
  { 286, 2, 32, 16, OSG_MENU, "MENU" },
};
static const OsgBtn ATARI_BTNS[] = {
  { 252, 156, 58, 58, 0, "FIRE" },
  { 160, 218, 46, 16, 1, "SEL" }, { 212, 218, 52, 16, 2, "START" },
  { 286, 2, 32, 16, OSG_MENU, "MENU" },
};
static const OsgBtn SMS_BTNS[] = {
  { 262, 144, 50, 50, 0, "1" }, { 206, 168, 48, 48, 1, "2" },
  { 178, 218, 62, 16, OSG_PAUSE, "START" },
  { 286, 2, 32, 16, OSG_MENU, "MENU" },
};

static const OsgBtn *osgBtns = nullptr;
static int osgBtnCount = 0;

static void osgLayout() {
  switch (currentPlatform) {
    case PLATFORM_NES:   osgBtns = NES_BTNS;   osgBtnCount = sizeof(NES_BTNS) / sizeof(OsgBtn);   break;
    case PLATFORM_ATARI: osgBtns = ATARI_BTNS; osgBtnCount = sizeof(ATARI_BTNS) / sizeof(OsgBtn); break;
    case PLATFORM_SMS:   osgBtns = SMS_BTNS;   osgBtnCount = sizeof(SMS_BTNS) / sizeof(OsgBtn);   break;
    default:             osgBtns = nullptr;    osgBtnCount = 0;                                   break;
  }
}

static bool osg_dirty   = true;
static bool stickActive = false;
static int  stickCX = 0, stickCY = 0;   // logical centre (set on the first touch of a press)

bool osgSupported() {
  return currentPlatform == PLATFORM_NES || currentPlatform == PLATFORM_ATARI || currentPlatform == PLATFORM_SMS;
}
bool osgActive() { return osgSupported() && !OptionsWindow && !DebugWindow; }

static int osgHit(int x, int y) {
  for (int i = 0; i < osgBtnCount; i++) {
    const OsgBtn &b = osgBtns[i];
    if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return i;
  }
  return -1;
}

// --- per-frame: read all contacts, drive the console input, flag a redraw on change ---
void osgPoll() {
  if (!osgActive()) return;
  if (!osgBtns) osgLayout();

  int16_t xs[5], ys[5];
  int n = gt911ReadPoints(xs, ys, 5);
  if (n < 0) return;                     // no fresh frame -> keep the previously applied state

  int njx = 1, njy = 1; uint8_t pbm = 0; bool menu = false, pause = false;
  bool stick = false; int stx = 0, sty = 0;
  for (int i = 0; i < n; i++) {
    int tx = xs[i], ty = ys[i];
    if (tx < STK_ZONE_X) { stick = true; stx = tx; sty = ty; continue; }   // left region -> stick
    int b = osgHit(tx, ty);
    if (b < 0) continue;
    int8_t a = osgBtns[b].pb;
    if (a == OSG_MENU)       menu = true;
    else if (a == OSG_PAUSE) pause = true;
    else                     pbm |= (uint8_t)(1 << a);
  }

  if (stick) {
    if (!stickActive) { stickActive = true; stickCX = stx; stickCY = sty; }   // re-centre on first touch
    int dx = stx - stickCX, dy = sty - stickCY;
    if (dx < -STK_DZ) njy = 0; else if (dx > STK_DZ) njy = 2;   // joyY: 0=left, 2=right
    if (dy < -STK_DZ) njx = 0; else if (dy > STK_DZ) njx = 2;   // joyX: 0=up,   2=down
  } else {
    stickActive = false;                 // released -> ball springs back to centre
  }

  joyX = njx; joyY = njy;
  Pb0 = (pbm & 1) != 0; Pb1 = (pbm & 2) != 0; Pb2 = (pbm & 4) != 0; Pb3 = (pbm & 8) != 0;
  applyPlatformInput();

  static bool prevMenu = false, prevPause = false;
  if (menu  && !prevMenu)  showHideOptionsWindow();
  if (pause && !prevPause) smsPauseButton();
  prevMenu = menu; prevPause = pause;

  // Redraw only on a real change (direction extreme / buttons / stick on-off / new centre).
  static int pjx = 1, pjy = 1, pcx = 0, pcy = 0; static uint8_t ppb = 0; static bool pact = false;
  if (njx != pjx || njy != pjy || pbm != ppb || stickActive != pact || stickCX != pcx || stickCY != pcy) {
    pjx = njx; pjy = njy; ppb = pbm; pact = stickActive; pcx = stickCX; pcy = stickCY; osg_dirty = true;
  }
}

// --- one rounded button (same style as the OSK keys) ---
static void osgDrawBtn(int x, int y, int w, int h, const char *label, bool pressed) {
  uint16_t face = pressed ? tft.color565(0, 190, 0) : tft.color565(60, 60, 60);
  tft.fillRoundRect(x + 1, y + 1, w - 2, h - 2, 6, face);
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 6, tft.color565(150, 150, 150));
  if (label && label[0]) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, face);
    tft.drawString(label, x + w / 2, y + h / 2, 2);
  }
}

// --- redraw the translucent controller into the overlay buffer (only when the state changed) ---
void osgRender() {
  if (!osgActive()) return;
  if (!osgBtns) osgLayout();
  if (!osg_dirty) return;

  tft.oskOverlayBegin();                 // clear the overlay to transparent + redirect drawing to it
  for (int i = 0; i < osgBtnCount; i++) {
    const OsgBtn &b = osgBtns[i];
    bool pressed = (b.pb == 0 && Pb0) || (b.pb == 1 && Pb1) || (b.pb == 2 && Pb2) || (b.pb == 3 && Pb3);
    osgDrawBtn(b.x, b.y, b.w, b.h, b.label, pressed);
  }
  // Floating analog stick (only while a finger holds it): TRUE circles in PANEL coords.
  if (stickActive) {
    int pcx = stickCX * PANEL_NATIVE_W / DISP_LOGICAL_W;
    int pcy = stickCY * PANEL_NATIVE_H / DISP_LOGICAL_H;
    int dxs = (joyY == 0) ? -1 : (joyY == 2) ? 1 : 0;
    int dys = (joyX == 0) ? -1 : (joyX == 2) ? 1 : 0;
    tft.oskOverlayFillCircle(pcx, pcy, STK_RING_R, tft.color565(40, 40, 40));                 // outer ring
    tft.oskOverlayFillCircle(pcx + dxs * STK_TRAVEL, pcy + dys * STK_TRAVEL, STK_BALL_R,
                             tft.color565(165, 165, 165));                                    // inner ball
  }
  tft.oskOverlayEnd();
  osg_dirty = false;
}

#endif // BOARD_PANEL_DSI
