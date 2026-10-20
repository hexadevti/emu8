// zxjoymap.h - host keyboard (HID usage code) -> ZX Spectrum Kempston joystick. See zxjoymap.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

// A resolved rule is the Kempston lines the key pulls, as a mask of zxSetKempston()'s bits (set =
// pulled; port 0x1F reads them active high): one direction, or FIRE.
#define ZXJ_RIGHT 0x01
#define ZXJ_LEFT  0x02
#define ZXJ_DOWN  0x04
#define ZXJ_UP    0x08
#define ZXJ_FIRE  0x10
#define ZXJ_NONE  0xFF   // no rule for this key            (same value as C64K_NONE)
#define ZXJ_UNMAP 0xFE   // a rule saying "this key does nothing" (same value as C64K_UNMAP)

static inline bool zxjIsAction(uint8_t c) { return c && c < 0x80; }

// The lines host key `hid` pulls (ZXJ_NONE / ZXJ_UNMAP: none); overrides before the default.
// CTRL / ALT are looked up as C64K_HID_CTRL / C64K_HID_ALT (c64keymap.h).
uint8_t zxJoymapLookup(uint8_t hid);
// (Re)read the overrides from /emu8/zx/joymap.txt (or /zxjoymap.txt). Missing file = defaults only.
void    zxJoymapLoad();

// ---- for the JOYSTICK page (optionsui.cpp); the host keys are the KEYBOARD page's ----
// The picker: the four directions (UP RIGHT DOWN LEFT) then FIRE.
int     zxJoymapActionCount();
uint8_t zxJoymapActionAt(int i);
const char *zxJoymapActionName(uint8_t code);               // "UP", "LEFT", "FIRE"
// The rule as text: "FIRE", "NONE", "-" (no rule).
void    zxJoymapDescribe(uint8_t code, char *out, size_t n);
// The rule for `hid`: `over` true when it comes from the overrides.
uint8_t zxJoymapRule(uint8_t hid, bool *over);
uint8_t zxJoymapDefault(uint8_t hid);
// Change one rule in memory (ZXJ_NONE = back to the default). False when the override table is full.
bool    zxJoymapSet(uint8_t hid, uint8_t code);
void    zxJoymapResetAll();
int     zxJoymapChangedCount();
// Write the overrides to /emu8/zx/joymap.txt. False if the card could not be written.
bool    zxJoymapSave();
