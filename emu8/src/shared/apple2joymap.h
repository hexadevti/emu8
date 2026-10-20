// apple2joymap.h - host keyboard (HID usage code) -> Apple II joystick. See apple2joymap.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

// A resolved rule is what the key does to the joystick, as a mask: a direction, or one
// of the two buttons. The Apple has no digital stick: a direction is a paddle timer held at its end
// (timerpdl0 = X, timerpdl1 = Y), a button is $C061 / $C062 (Pb0 = open-apple, Pb1 = solid-apple).
#define A2J_UP    0x01
#define A2J_DOWN  0x02
#define A2J_LEFT  0x04
#define A2J_RIGHT 0x08
#define A2J_BTN0  0x10
#define A2J_BTN1  0x20
#define A2J_DIRS  0x0F
#define A2J_NONE  0xFF   // no rule for this key            (same value as A2K_NONE)
#define A2J_UNMAP 0xFE   // a rule saying "this key does nothing" (same value as A2K_UNMAP)

static inline bool a2jIsAction(uint8_t c) { return c && c < 0x80; }

// What host key `hid` does to the joystick (A2J_NONE / A2J_UNMAP: nothing); overrides first.
// CTRL / ALT / RALT / SHIFT are looked up as C64K_HID_CTRL / C64K_HID_ALT / A2K_HID_RALT / C64K_HID_SHIFT.
uint8_t apple2JoymapLookup(uint8_t hid);
// (Re)read the overrides from /emu8/apple2/joymap.txt (or /apple2joymap.txt). Missing file = defaults only.
void    apple2JoymapLoad();

// ---- for the JOYSTICK page (optionsui.cpp) ----
// The host keys it lists: the Apple KEYBOARD page's, then SHIFT.
int     apple2JoymapHostCount();
uint8_t apple2JoymapHostAt(int i);
void    apple2JoymapHostName(uint8_t hid, char *out, size_t n);   // apple2KeymapHostName + "SHIFT"
// The picker: the four directions (UP RIGHT DOWN LEFT) then BUTTON0, BUTTON1.
int     apple2JoymapActionCount();
uint8_t apple2JoymapActionAt(int i);
const char *apple2JoymapActionName(uint8_t code);              // "UP", "LEFT", "BUTTON0"
void    apple2JoymapDescribe(uint8_t code, char *out, size_t n);   // "BUTTON0", "NONE", "-"
uint8_t apple2JoymapRule(uint8_t hid, bool *over);
uint8_t apple2JoymapDefault(uint8_t hid);
bool    apple2JoymapSet(uint8_t hid, uint8_t code);            // A2J_NONE = back to the default
void    apple2JoymapResetAll();
int     apple2JoymapChangedCount();
bool    apple2JoymapSave();                                    // /emu8/apple2/joymap.txt
