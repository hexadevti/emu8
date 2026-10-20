// apple2joycal.h - Apple II analog joystick calibration. See apple2joycal.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

// The boards with an analog stick to calibrate: the CYD's ADC stick, a USB gamepad's axis bytes
// (S3 / P4 USB host), the desktop build's SDL controller. The PicoCalc has no analog input.
#if BOARD_INPUT_ANALOG || defined(BOARD_DESKTOP) || (BOARD_INPUT_USB && !defined(PICOCALC_MAINBOARD))
#define A2CAL_ANALOG 1
#else
#define A2CAL_ANALOG 0
#endif

// The calibration wizard's steps (optionsui.cpp CONFIG > CALIBRATE).
#define A2CAL_IDLE   0
#define A2CAL_CENTER 1   // waiting for the stick to rest; Enter records the centre
#define A2CAL_EDGES  2   // recording the widest reading on each side; Enter accepts

// The source's raw range when nothing is calibrated (ADC 0..4095 with the centre read at boot,
// a USB pad 0..255, SDL -32768..32767). The input backend sets it once at setup.
void  apple2JoycalDefaults(int lo, int midX, int midY, int hi);
// Each raw reading from the input backend (raw grows to the right / down). Records it for the
// CALIBRATE page and, while the wizard is on EDGES, widens the range.
void  apple2JoycalFeed(int rawX, int rawY);
// The paddle timer (JOY_MIN..JOY_MAX) a raw reading on axis 0 (X) / 1 (Y) means.
float apple2JoycalPaddle(int axis, int raw);

// (Re)read /apple2/joycal.txt. Missing file = the source's raw range.
void  apple2JoycalLoad();
bool  apple2JoycalSave();                       // /apple2/joycal.txt
bool  apple2JoycalReset();                      // forget it (deletes the file)
bool  apple2JoycalCustom();                     // a calibration is in use (not the default range)

// ---- the wizard ----
int   apple2JoycalStep();
void  apple2JoycalBegin();                      // -> CENTER
// CENTER -> EDGES; EDGES -> IDLE and returns 1 (accepted) or -1 (range too small, kept the old one).
int   apple2JoycalNext();
void  apple2JoycalCancel();                     // back to IDLE, the old calibration kept
// Stick moves don't navigate the menu while this is true (CENTER / EDGES).
static inline bool apple2JoycalBusy() { return apple2JoycalStep() != A2CAL_IDLE; }
// True once per change worth repainting the page for (throttled); the page polls it.
bool  apple2JoycalChanged();

// For the page: the last raw reading, the range in use (or being recorded), what the Apple reads.
void  apple2JoycalRaw(int *x, int *y);
void  apple2JoycalRange(int axis, int *lo, int *mid, int *hi);
int   apple2JoycalPdl(int axis);                // 0..255, as PDL(axis) reads the last raw value
