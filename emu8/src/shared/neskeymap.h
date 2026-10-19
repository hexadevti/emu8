// neskeymap.h - host keyboard (HID usage code) -> NES controller 1 button. See neskeymap.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

// A resolved rule is the button's bit number in nesSetController()'s mask (active high).
#define NESK_A      0
#define NESK_B      1
#define NESK_SELECT 2
#define NESK_START  3
#define NESK_UP     4
#define NESK_DOWN   5
#define NESK_LEFT   6
#define NESK_RIGHT  7
#define NESK_NONE   0xFF   // no rule for this key            (same value as C64K_NONE)
#define NESK_UNMAP  0xFE   // a rule saying "this key does nothing" (same value as C64K_UNMAP)

static inline bool nesKIsButton(uint8_t c) { return c < 8; }

// The button host key `hid` presses (NESK_NONE / NESK_UNMAP: none); overrides before the default.
// CTRL / ALT are looked up as C64K_HID_CTRL / C64K_HID_ALT (c64keymap.h), like the C64.
uint8_t nesKeymapLookup(uint8_t hid);
// (Re)read the overrides from /nes/keymap.txt (or /neskeymap.txt). Missing file = defaults only.
void    nesKeymapLoad();

// ---- for the KEYBOARD page (optionsui.cpp); the host keys and their names are the C64 page's ----
// "UP", "SELECT", "A" -- the name the file uses.
const char *nesKeymapButtonName(uint8_t b);
// The picker's order, as on the pad left to right: UP DOWN LEFT RIGHT SELECT START B A.
uint8_t nesKeymapButtonAt(int i);
// The rule as text: "START", "NONE", "-" (no rule).
void    nesKeymapDescribe(uint8_t code, char *out, size_t n);
// The rule for `hid`: `over` true when it comes from the overrides.
uint8_t nesKeymapRule(uint8_t hid, bool *over);
uint8_t nesKeymapDefault(uint8_t hid);
// Change one rule in memory (NESK_NONE = back to the default). False when the override table is full.
bool    nesKeymapSet(uint8_t hid, uint8_t code);
void    nesKeymapResetAll();
int     nesKeymapChangedCount();
// Write the overrides to /nes/keymap.txt. False if the card could not be written.
bool    nesKeymapSave();
