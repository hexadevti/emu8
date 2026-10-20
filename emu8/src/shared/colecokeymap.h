// colecokeymap.h - host keyboard (HID usage code) -> ColecoVision controller 1 (stick, the two side
// fire buttons, the 12-key keypad). See colecokeymap.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

// A resolved rule: 0..5 = the bit number in colecoSetInput()'s mask (active low there), 6.. = a
// keypad key, colecoSetKeypad(code - COLK_KEY0): 0..9, then '*' (10) and '#' (11).
#define COLK_UP     0
#define COLK_DOWN   1
#define COLK_LEFT   2
#define COLK_RIGHT  3
#define COLK_LFIRE  4      // left side button
#define COLK_RFIRE  5      // right side button
#define COLK_KEY0   6      // keypad 0; 1..9 follow
#define COLK_STAR   16     // keypad '*'
#define COLK_HASH   17     // keypad '#'
#define COLK_COUNT  18
#define COLK_NONE   0xFF   // no rule for this key            (same value as C64K_NONE)
#define COLK_UNMAP  0xFE   // a rule saying "this key does nothing" (same value as C64K_UNMAP)

static inline bool colKIsButton(uint8_t c) { return c < COLK_COUNT; }
static inline bool colKIsKeypad(uint8_t c) { return c >= COLK_KEY0 && c < COLK_COUNT; }

// What host key `hid` presses (COLK_NONE / COLK_UNMAP: nothing); overrides before the default. With
// `shifted`, a SHIFT+ rule wins, else it is the plain key's (Shift-8 = '*' as printed on the key).
// CTRL / ALT are looked up as C64K_HID_CTRL / C64K_HID_ALT (c64keymap.h), unshifted, like the C64.
uint8_t colecoKeymapLookup(uint8_t hid, bool shifted);
// (Re)read the overrides from /emu8/coleco/keymap.txt (or /colecokeymap.txt). Missing file = defaults only.
void    colecoKeymapLoad();

// ---- for the KEYBOARD page (optionsui.cpp); the host keys and their names are the C64 page's ----
// "UP", "LFIRE", "7", "*", "#" -- as the menu shows it (the file writes '#' as HASH).
const char *colecoKeymapButtonName(uint8_t b);
// The picker's order: UP DOWN LEFT RIGHT LFIRE RFIRE, then the keypad as printed: 1..9 * 0 #.
uint8_t colecoKeymapButtonAt(int i);
// The rule as text: "LFIRE", "*", "NONE", "-" (no rule).
void    colecoKeymapDescribe(uint8_t code, char *out, size_t n);
// The rule for (hid, shifted), not falling back to the plain key: `over` true when it is an override.
// A SHIFT+ row with no rule of its own is COLK_NONE ("as no SHIFT").
uint8_t colecoKeymapRule(uint8_t hid, bool shifted, bool *over);
uint8_t colecoKeymapDefault(uint8_t hid, bool shifted);
// Change one rule in memory (COLK_NONE = back to the default). False when the override table is full.
bool    colecoKeymapSet(uint8_t hid, bool shifted, uint8_t code);
void    colecoKeymapResetAll();
int     colecoKeymapChangedCount();
// Write the overrides to /emu8/coleco/keymap.txt. False if the card could not be written.
bool    colecoKeymapSave();
