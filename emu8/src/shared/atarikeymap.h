// atarikeymap.h - host keyboard (HID usage code) -> Atari 2600 joystick / console switch. See atarikeymap.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

// A resolved rule is a mask: the stick directions in atariSetInput()'s dirBits (bit0..3, so a
// diagonal is two of them), plus FIRE and the console's SELECT / RESET switches.
#define ATARIK_UP     0x01
#define ATARIK_DOWN   0x02
#define ATARIK_LEFT   0x04
#define ATARIK_RIGHT  0x08
#define ATARIK_FIRE   0x10
#define ATARIK_SELECT 0x20
#define ATARIK_RESET  0x40
#define ATARIK_DIRS   0x0F
#define ATARIK_NONE   0xFF   // no rule for this key            (same value as C64K_NONE)
#define ATARIK_UNMAP  0xFE   // a rule saying "this key does nothing" (same value as C64K_UNMAP)

static inline bool atariKIsAction(uint8_t c) { return c && c < 0x80; }

// The mask host key `hid` presses (ATARIK_NONE / ATARIK_UNMAP: none); overrides before the default.
// CTRL / ALT are looked up as C64K_HID_CTRL / C64K_HID_ALT (c64keymap.h), like the C64.
uint8_t atariKeymapLookup(uint8_t hid);
// (Re)read the overrides from /atari/keymap.txt (or /atarikeymap.txt). Missing file = defaults only.
void    atariKeymapLoad();

// ---- for the KEYBOARD page (optionsui.cpp); the host keys and their names are the C64 page's ----
// The picker: the eight directions (clockwise from UP), then FIRE, SELECT, RESET.
int     atariKeymapActionCount();
uint8_t atariKeymapActionAt(int i);
const char *atariKeymapActionName(uint8_t code);            // "UP", "UP+LEFT", "FIRE", "RESET"
// The rule as text: "FIRE", "NONE", "-" (no rule).
void    atariKeymapDescribe(uint8_t code, char *out, size_t n);
// The rule for `hid`: `over` true when it comes from the overrides.
uint8_t atariKeymapRule(uint8_t hid, bool *over);
uint8_t atariKeymapDefault(uint8_t hid);
// Change one rule in memory (ATARIK_NONE = back to the default). False when the override table is full.
bool    atariKeymapSet(uint8_t hid, uint8_t code);
void    atariKeymapResetAll();
int     atariKeymapChangedCount();
// Write the overrides to /atari/keymap.txt. False if the card could not be written.
bool    atariKeymapSave();
