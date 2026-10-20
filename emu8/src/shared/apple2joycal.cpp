// apple2joycal.cpp - calibration for the Apple II's analog joystick (PDL(0) = X, PDL(1) = Y).
//
// A stick rarely rests at the middle of its raw range or reaches both ends of it, so a game reading
// PDL(0) can see a centred stick as off-centre and never get 0 or 255. The calibration is three raw
// readings per axis: the leftmost (top), the rest position and the rightmost (bottom). The left
// half, lo..mid, then covers paddle 0..127 and the right half, mid..hi, 127..255; a reading past
// an end is that end, and a small dead zone round the centre reads exactly 127.
//
// Recorded from the Apple menu (CONFIG > CALIBRATE): let the stick rest and press Enter / fire,
// then move it to every edge and press again. Saved to /apple2/joycal.txt:
//
//     X = <lo> <center> <hi>
//     Y = <lo> <center> <hi>
//
// in the input's raw units (CYD ADC 0..4095, USB pad 0..255, desktop SDL -32768..32767). With no
// file the input's own full range is used, which is what the Apple had before calibration existed.

#include "../../emu.h"
#include "apple2joycal.h"

#if A2CAL_ANALOG

#define DEAD_PCT   4      // dead zone round the centre, % of each half
#define MIN_PCT    20     // a recorded half must span this % of the default half, or it is refused

struct Axis { int lo, mid, hi; };
static Axis    gDef[2] = { {0, 2048, 4095}, {0, 2048, 4095} };
static Axis    gCal[2];
static bool    gHaveCal = false;
static Axis    gRec[2];          // the wizard's recording
static volatile int gStep = A2CAL_IDLE;
static volatile int gRawX = 0, gRawY = 0;
static volatile bool gDirty = false;

static const Axis &axisInUse(int a) { return gHaveCal ? gCal[a] : gDef[a]; }

void apple2JoycalDefaults(int lo, int midX, int midY, int hi)
{
  gDef[0] = {lo, midX, hi};
  gDef[1] = {lo, midY, hi};
  gRawX = midX; gRawY = midY;
}

void apple2JoycalFeed(int rawX, int rawY)
{
  if (rawX != gRawX || rawY != gRawY) gDirty = true;
  gRawX = rawX; gRawY = rawY;
  if (gStep == A2CAL_EDGES) {
    const int r[2] = {rawX, rawY};
    for (int a = 0; a < 2; a++) {
      if (r[a] < gRec[a].lo) gRec[a].lo = r[a];
      if (r[a] > gRec[a].hi) gRec[a].hi = r[a];
    }
  }
}

float apple2JoycalPaddle(int axis, int raw)
{
  const Axis &c = axisInUse(axis);
  const int lo = c.lo, mid = c.mid, hi = c.hi;
  if (raw <= mid) {
    const int span = mid - lo;
    if (span <= 0) return JOY_MID;
    const int dead = span * DEAD_PCT / 100;
    if (raw >= mid - dead) return JOY_MID;
    if (raw <= lo) return JOY_MIN;
    return (float)JOY_MID * (raw - lo) / (span - dead);
  }
  const int span = hi - mid;
  if (span <= 0) return JOY_MID;
  const int dead = span * DEAD_PCT / 100;
  if (raw <= mid + dead) return JOY_MID;
  if (raw >= hi) return JOY_MAX;
  return JOY_MID + (float)(JOY_MAX - JOY_MID) * (raw - mid - dead) / (span - dead);
}

// ---- the file ---------------------------------------------------------------------------------
static const char *const kPath = "/apple2/joycal.txt";

void apple2JoycalLoad()
{
  gHaveCal = false;
  static const char *const kPaths[] = { "/apple2/joycal.txt", "/apple2joycal.txt" };
  for (const char *path : kPaths) {
    File f;
    try { f = FSTYPE.open(path, FILE_READ); }
    catch (const std::bad_alloc &) { printLog("Apple joycal: out of heap opening the file"); return; }
    if (!f) continue;
    Axis a[2];
    bool got[2] = {false, false};
    char line[64];
    int len = 0;
    for (;;) {
      int c = f.read();
      if (c < 0 || c == '\n') {
        line[len] = 0;
        if (char *h = strchr(line, '#')) *h = 0;
        char ax; int lo, mid, hi;
        if (sscanf(line, " %c = %d %d %d", &ax, &lo, &mid, &hi) == 4 && lo < mid && mid < hi) {
          ax = (char)toupper((unsigned char)ax);
          if (ax == 'X' || ax == 'Y') { a[ax == 'Y'] = {lo, mid, hi}; got[ax == 'Y'] = true; }
        }
        len = 0;
        if (c < 0) break;
      } else if (len < (int)sizeof(line) - 1) {
        line[len++] = (char)c;
      }
    }
    f.close();
    if (got[0] && got[1]) { gCal[0] = a[0]; gCal[1] = a[1]; gHaveCal = true; }
    snprintf(buf, sizeof(buf), "Apple joycal: %s, %s", path,
             gHaveCal ? "calibrated" : "needs an X and a Y line - using the default range");
    printLog(buf);
    return;
  }
}

bool apple2JoycalSave()
{
  // Timed lock, not busTake(): this runs in the render task (the menu).
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  bool ok = false;
  try {
    if (!FSTYPE.exists("/apple2")) FSTYPE.mkdir("/apple2");
    if (FSTYPE.exists(kPath)) FSTYPE.remove(kPath);     // FILE_WRITE appends on arduino-pico
    File f = FSTYPE.open(kPath, FILE_WRITE);
    if (f) {
      char text[256];
      int len = snprintf(text, sizeof(text),
        "# emu8 Apple II analog joystick calibration, saved from the Apple menu (CONFIG > CALIBRATE).\n"
        "# <axis> = <left/top> <center> <right/bottom>, in the input's raw units.\n"
        "X = %d %d %d\nY = %d %d %d\n",
        gCal[0].lo, gCal[0].mid, gCal[0].hi, gCal[1].lo, gCal[1].mid, gCal[1].hi);
      ok = f.write((const uint8_t *)text, len) == (size_t)len;
      f.close();
    }
  } catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  snprintf(buf, sizeof(buf), "Apple joycal: %s %s", kPath, ok ? "saved" : "NOT saved");
  printLog(buf);
  return ok;
}

bool apple2JoycalReset()
{
  gHaveCal = false;
  gDirty = true;
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  bool ok = true;
  try { if (FSTYPE.exists(kPath)) ok = FSTYPE.remove(kPath); }
  catch (const std::bad_alloc &) { ok = false; }
  if (gBusLock) xSemaphoreGive(gBusLock);
  return ok;
}

bool apple2JoycalCustom() { return gHaveCal; }

// ---- the wizard -------------------------------------------------------------------------------
int  apple2JoycalStep() { return gStep; }
void apple2JoycalBegin() { gStep = A2CAL_CENTER; gDirty = true; }
void apple2JoycalCancel() { gStep = A2CAL_IDLE; gDirty = true; }

int apple2JoycalNext()
{
  gDirty = true;
  if (gStep == A2CAL_CENTER) {
    gRec[0] = {gRawX, gRawX, gRawX};
    gRec[1] = {gRawY, gRawY, gRawY};
    gStep = A2CAL_EDGES;
    return 0;
  }
  if (gStep != A2CAL_EDGES) return 0;
  gStep = A2CAL_IDLE;
  for (int a = 0; a < 2; a++) {
    const Axis &d = gDef[a], &r = gRec[a];
    if ((r.mid - r.lo) * 100 < (d.mid - d.lo) * MIN_PCT || (r.hi - r.mid) * 100 < (d.hi - d.mid) * MIN_PCT)
      return -1;
  }
  gCal[0] = gRec[0]; gCal[1] = gRec[1];
  gHaveCal = true;
  return 1;
}

bool apple2JoycalChanged()
{
  static uint32_t last = 0;
  if (!gDirty) return false;
  const uint32_t now = millis();
  if (now - last < 100) return false;          // at most ~10 repaints a second
  last = now;
  gDirty = false;
  return true;
}

void apple2JoycalRaw(int *x, int *y) { *x = gRawX; *y = gRawY; }

void apple2JoycalRange(int axis, int *lo, int *mid, int *hi)
{
  const Axis &c = gStep != A2CAL_IDLE ? gRec[axis] : axisInUse(axis);
  if (gStep == A2CAL_CENTER) { *lo = *hi = *mid = axis ? gRawY : gRawX; return; }
  *lo = c.lo; *mid = c.mid; *hi = c.hi;
}

int apple2JoycalPdl(int axis)
{
  return (int)(apple2JoycalPaddle(axis, axis ? gRawY : gRawX) * 255.0f / JOY_MAX + 0.5f);
}

#endif // A2CAL_ANALOG
