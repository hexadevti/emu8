// c64joymap.h - host keyboard (HID usage code) -> C64 joystick. See c64joymap.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

// A resolved rule is the joystick lines the key pulls, as a mask of c64SetJoystick()'s bits
// (set = pulled; the CIA reads them active low): one direction, or FIRE. FIRE2 / FIRE3
// are a 2/3-button joystick's extra buttons, on the paddle lines (SID POTX / POTY,
// c64SetJoyButtons()); only programs written for such a joystick read them.
#define C64J_UP    0x01
#define C64J_DOWN  0x02
#define C64J_LEFT  0x04
#define C64J_RIGHT 0x08
#define C64J_FIRE  0x10
#define C64J_FIRE2 0x20
#define C64J_FIRE3 0x40
#define C64J_LINES 0x1F   // the CIA's lines; above them the paddle-line buttons
#define C64J_NONE  0xFF   // no rule for this key            (same value as C64K_NONE)
#define C64J_UNMAP 0xFE   // a rule saying "this key does nothing" (same value as C64K_UNMAP)

static inline bool c64jIsAction(uint8_t c) { return c && c < 0x80; }

// The lines host key `hid` pulls (C64J_NONE / C64J_UNMAP: none); overrides before the default.
// CTRL / ALT / SHIFT are looked up as C64K_HID_CTRL / C64K_HID_ALT / C64K_HID_SHIFT (c64keymap.h).
uint8_t c64JoymapLookup(uint8_t hid);
// (Re)read the overrides from /emu8/c64/joymap.txt (or /c64joymap.txt). Missing file = defaults only.
void    c64JoymapLoad();

// ---- for the JOYSTICK page (optionsui.cpp) ----
// The host keys it lists: the C64 KEYBOARD page's, then SHIFT.
int     c64JoymapHostCount();
uint8_t c64JoymapHostAt(int i);
void    c64JoymapHostName(uint8_t hid, char *out, size_t n);   // c64KeymapHostName + "SHIFT"
// The picker: the four directions (UP RIGHT DOWN LEFT) then FIRE, FIRE2, FIRE3.
int     c64JoymapActionCount();
uint8_t c64JoymapActionAt(int i);
const char *c64JoymapActionName(uint8_t code);              // "UP", "LEFT", "FIRE", "FIRE2"
// The rule as text: "FIRE", "NONE", "-" (no rule).
void    c64JoymapDescribe(uint8_t code, char *out, size_t n);
// The rule for `hid`: `over` true when it comes from the overrides.
uint8_t c64JoymapRule(uint8_t hid, bool *over);
uint8_t c64JoymapDefault(uint8_t hid);
// Change one rule in memory (C64J_NONE = back to the default). False when the override table is full.
bool    c64JoymapSet(uint8_t hid, uint8_t code);
void    c64JoymapResetAll();
int     c64JoymapChangedCount();
// Write the overrides to /emu8/c64/joymap.txt. False if the card could not be written.
bool    c64JoymapSave();
