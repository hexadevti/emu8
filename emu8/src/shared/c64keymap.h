// c64keymap.h - host keyboard (HID usage code) -> C64 keyboard map. See c64keymap.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

// A resolved C64 key is one byte: bits 0-5 = matrix index (col*8 + row, the CIA1 PA/PB lines),
// bits 6-7 = what the key does to the C64's SHIFT line while held. Codes >= 0xC0 are special.
#define C64K_SHIFT_KEEP 0      // SHIFT as the host keyboard has it
#define C64K_SHIFT_ON   1      // SHIFT held (F2 = SHIFT+F1, CRSR LEFT = SHIFT+CRSR RIGHT, ...)
#define C64K_SHIFT_OFF  2      // SHIFT released (Shift-2 '@' types the C64's unshifted @ key)
#define C64K_NONE       0xFF   // no rule for this key
#define C64K_UNMAP      0xFE   // a rule saying "this key does nothing"
#define C64K_RESTORE    0xFD   // RESTORE: not in the matrix, it pulses the 6510's NMI

// Pseudo usage codes for the modifier keys (they arrive as the modifier byte, not in keys[]).
#define C64K_HID_CTRL   0xE0   // either Ctrl
#define C64K_HID_ALT    0xE2   // either Alt / GUI
#define C64K_HID_SHIFT  0xE1   // either Shift pressed on its own (only the PicoCalc mapper uses it)

#define C64K_LSHIFT_INDEX 15   // left SHIFT: col 1, row 7
#define C64K_RSHIFT_INDEX 52   // right SHIFT: col 6, row 4

static inline bool    c64kIsMatrix(uint8_t c) { return c < 0xC0; }
static inline uint8_t c64kIndex(uint8_t c)    { return c & 63; }   // col*8 + row
static inline uint8_t c64kShift(uint8_t c)    { return c >> 6; }

// The C64 key for host key `hid` typed with the host SHIFT `shift` down: a SHIFT+ rule first,
// then the plain rule; the SD card's overrides before the built-in default in each case.
uint8_t c64KeymapLookup(uint8_t hid, bool shift);
// (Re)read the overrides from /emu8/c64/keymap.txt (or /c64keymap.txt). Missing file = defaults only.
void c64KeymapLoad();

// ---- for the KEYBOARD page (optionsui.cpp) ----
// The host keys the page lists, in display order.
int     c64KeymapHostCount();
uint8_t c64KeymapHostAt(int i);
// "A", "F12", "PAGE_UP", "CTRL", "0x87" -- the name the file uses.
void    c64KeymapHostName(uint8_t hid, char *out, size_t n);
// The reverse: an upper-case name (as the file spells it) -> HID usage code, 0 = unknown.
uint8_t c64KeymapHostParse(const char *s);
// The C64 keys the picker lists: the 64 matrix keys, by name.
const char *c64KeymapC64Name(uint8_t index);
// The picker's order (letters, digits, function keys, the rest): the matrix index of entry i, 0..63.
uint8_t c64KeymapC64At(int i);
// The rule as text: "F1 +SHIFT", "RESTORE", "NONE", "-" (no rule). `shifted`: a SHIFT+ rule,
// where no suffix means -SHIFT, so it is spelled out.
void    c64KeymapDescribe(uint8_t code, bool shifted, char *out, size_t n);
// The rule for exactly (hid, shifted): `over` true when it comes from the overrides.
uint8_t c64KeymapRule(uint8_t hid, bool shifted, bool *over);
uint8_t c64KeymapDefault(uint8_t hid, bool shifted);
// Change one rule in memory (C64K_NONE = back to the default). False when the override table is full.
bool    c64KeymapSet(uint8_t hid, bool shifted, uint8_t code);
void    c64KeymapResetAll();
int     c64KeymapChangedCount();
// Write the overrides to /emu8/c64/keymap.txt. False if the card could not be written.
bool    c64KeymapSave();
