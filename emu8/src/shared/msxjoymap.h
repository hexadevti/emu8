// msxjoymap.h - host keyboard (HID usage code) -> MSX joystick (port 1). See msxjoymap.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "c64joymap.h"   // the C64J_* direction bits and c64jIsAction(): the MSX stick uses the same bits

// A resolved rule is the joystick lines the key pulls, as a mask of msxSetInput()'s bits (set =
// pulled; the PSG reads them active low): one direction, a diagonal, TRIG A or TRIG B.
#define MSXJ_TRIG_A C64J_FIRE    // 0x10
#define MSXJ_TRIG_B C64J_FIRE2   // 0x20
#define MSXJ_LINES  0x3F

// The lines host key `hid` pulls (C64J_NONE / C64J_UNMAP: none); overrides before the default.
// CTRL / ALT / SHIFT are looked up as C64K_HID_CTRL / C64K_HID_ALT / C64K_HID_SHIFT (c64keymap.h).
uint8_t msxJoymapLookup(uint8_t hid);
// (Re)read the overrides from /msx/joymap.txt (or /msxjoymap.txt). Missing file = defaults only.
void    msxJoymapLoad();

// ---- for the JOYSTICK page (optionsui.cpp) ----
// The host keys are the C64 JOYSTICK page's (c64JoymapHostCount / HostAt / HostName).
// The picker: the eight directions (as on the stick, clockwise from UP) then TRIG_A, TRIG_B.
int     msxJoymapActionCount();
uint8_t msxJoymapActionAt(int i);
const char *msxJoymapActionName(uint8_t code);              // "UP", "UP+LEFT", "TRIG_A"
// The rule as text: "TRIG_A", "NONE", "-" (no rule).
void    msxJoymapDescribe(uint8_t code, char *out, size_t n);
// The rule for `hid`: `over` true when it comes from the overrides.
uint8_t msxJoymapRule(uint8_t hid, bool *over);
uint8_t msxJoymapDefault(uint8_t hid);
// Change one rule in memory (C64J_NONE = back to the default). False when the override table is full.
bool    msxJoymapSet(uint8_t hid, uint8_t code);
void    msxJoymapResetAll();
int     msxJoymapChangedCount();
// Write the overrides to /msx/joymap.txt. False if the card could not be written.
bool    msxJoymapSave();
