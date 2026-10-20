// smskeymap.h - host keyboard (HID usage code) -> SMS control pad 1 button. See smskeymap.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

// A resolved rule is the button's bit number in smsSetInput()'s mask (active low there).
#define SMSK_UP     0
#define SMSK_DOWN   1
#define SMSK_LEFT   2
#define SMSK_RIGHT  3
#define SMSK_1      4      // button 1 (START)
#define SMSK_2      5      // button 2
#define SMSK_NONE   0xFF   // no rule for this key            (same value as C64K_NONE)
#define SMSK_UNMAP  0xFE   // a rule saying "this key does nothing" (same value as C64K_UNMAP)

static inline bool smsKIsButton(uint8_t c) { return c < 6; }

// The button host key `hid` presses (SMSK_NONE / SMSK_UNMAP: none); overrides before the default.
// CTRL / ALT are looked up as C64K_HID_CTRL / C64K_HID_ALT (c64keymap.h), like the C64.
uint8_t smsKeymapLookup(uint8_t hid);
// (Re)read the overrides from /emu8/sms/keymap.txt (or /smskeymap.txt). Missing file = defaults only.
void    smsKeymapLoad();

// ---- for the KEYBOARD page (optionsui.cpp); the host keys and their names are the C64 page's ----
// "UP", "1", "2" -- the name the file uses.
const char *smsKeymapButtonName(uint8_t b);
// The picker's order, as on the pad left to right: UP DOWN LEFT RIGHT 1 2.
uint8_t smsKeymapButtonAt(int i);
// The rule as text: "1", "NONE", "-" (no rule).
void    smsKeymapDescribe(uint8_t code, char *out, size_t n);
// The rule for `hid`: `over` true when it comes from the overrides.
uint8_t smsKeymapRule(uint8_t hid, bool *over);
uint8_t smsKeymapDefault(uint8_t hid);
// Change one rule in memory (SMSK_NONE = back to the default). False when the override table is full.
bool    smsKeymapSet(uint8_t hid, uint8_t code);
void    smsKeymapResetAll();
int     smsKeymapChangedCount();
// Write the overrides to /emu8/sms/keymap.txt. False if the card could not be written.
bool    smsKeymapSave();
