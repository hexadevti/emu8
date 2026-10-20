// msxkeymap.h - host keyboard (HID usage code) -> MSX keyboard map. See msxkeymap.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "c64keymap.h"   // C64K_NONE / C64K_UNMAP and the C64K_HID_* modifier codes, shared by every map

// A resolved MSX key is one byte: mode * 72 + index, where index = row*8 + bit of the 8255 PPI
// matrix rows 0..8 (msxKeyMatrix(row, bit, down); the keypad rows 9-10 are not mapped) and mode is
// what the key does to the MSX's SHIFT line while held. Codes >= MSXK_CODES are special.
#define MSXK_KEYS       72
#define MSXK_SHIFT_KEEP 0      // SHIFT as the host keyboard has it
#define MSXK_SHIFT_ON   1      // SHIFT held (F6 = SHIFT+F1, CLS = SHIFT+HOME, ...)
#define MSXK_SHIFT_OFF  2      // SHIFT released
#define MSXK_CODES      (3 * MSXK_KEYS)   // 216
#define MSXK_NONE       C64K_NONE    // 0xFF: no rule for this key
#define MSXK_UNMAP      C64K_UNMAP   // 0xFE: a rule saying "this key does nothing"

#define MSXK_SHIFT_INDEX 48    // SHIFT: row 6, bit 0

static inline bool    msxkIsKey(uint8_t c)  { return c < MSXK_CODES; }
static inline uint8_t msxkIndex(uint8_t c)  { return c % MSXK_KEYS; }   // row*8 + bit
static inline uint8_t msxkShift(uint8_t c)  { return c / MSXK_KEYS; }
static inline uint8_t msxkMake(uint8_t index, int mode) { return (uint8_t)(mode * MSXK_KEYS + index); }

// The MSX key for host key `hid` typed with the host SHIFT `shift` down: a SHIFT+ rule first,
// then the plain rule; the SD card's overrides before the built-in default in each case.
uint8_t msxKeymapLookup(uint8_t hid, bool shift);
// (Re)read the overrides from /msx/keymap.txt (or /msxkeymap.txt). Missing file = defaults only.
void    msxKeymapLoad();

// ---- for the KEYBOARD page (optionsui.cpp) ----
// The host keys are the C64 page's (c64KeymapHostCount / HostAt / HostName / HostParse).
// The MSX keys the picker lists: the 72 matrix keys, by name.
const char *msxKeymapMsxName(uint8_t index);
// The picker's order (letters, digits, function keys, the rest): the matrix index of entry i, 0..71.
uint8_t msxKeymapMsxAt(int i);
// The rule as text: "F1 +SHIFT", "NONE", "-" (no rule).
void    msxKeymapDescribe(uint8_t code, bool shifted, char *out, size_t n);
// The rule for exactly (hid, shifted): `over` true when it comes from the overrides.
uint8_t msxKeymapRule(uint8_t hid, bool shifted, bool *over);
uint8_t msxKeymapDefault(uint8_t hid, bool shifted);
// Change one rule in memory (MSXK_NONE = back to the default). False when the override table is full.
bool    msxKeymapSet(uint8_t hid, bool shifted, uint8_t code);
void    msxKeymapResetAll();
int     msxKeymapChangedCount();
// Write the overrides to /msx/keymap.txt. False if the card could not be written.
bool    msxKeymapSave();
