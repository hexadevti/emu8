// zxkeymap.h - host keyboard (HID usage code) -> ZX Spectrum keyboard map. See zxkeymap.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

// A resolved Spectrum key is one byte: bits 0-5 = matrix index (half-row*5 + bit, 0..39), bits 6-7
// = what the key does to the shift keys while held. NONE / UNMAP share the C64 map's values, so the
// KEYBOARD page treats every map's "no rule" / "does nothing" alike.
#define ZXK_KEEP   0       // CAPS SHIFT as the host Shift has it
#define ZXK_CAPS   1       // CAPS SHIFT held (DELETE = CAPS+0, the cursor keys = CAPS+5..8)
#define ZXK_PLAIN  2       // CAPS SHIFT released (a SHIFT+ rule typing an unshifted key)
#define ZXK_SYM    3       // SYMBOL SHIFT held, CAPS SHIFT released (',' = SYMBOL+N)
#define ZXK_NONE   0xFF    // no rule for this key            (same value as C64K_NONE)
#define ZXK_UNMAP  0xFE    // a rule saying "this key does nothing" (same value as C64K_UNMAP)

#define ZXK_KEYS       40
#define ZXK_CAPS_INDEX 0   // CAPS SHIFT: half-row 0, bit 0
#define ZXK_SYM_INDEX  36  // SYMBOL SHIFT: half-row 7, bit 1

static inline bool    zxkIsKey(uint8_t c) { return (c & 63) < ZXK_KEYS && c < 0xF0; }
static inline uint8_t zxkIndex(uint8_t c) { return c & 63; }    // half-row*5 + bit
static inline uint8_t zxkMode(uint8_t c)  { return c >> 6; }

// The Spectrum key for host key `hid` typed with the host Shift `shift` down: a SHIFT+ rule first,
// then the plain rule; the SD card's overrides before the built-in default in each case.
// CTRL / ALT are looked up as C64K_HID_CTRL / C64K_HID_ALT (c64keymap.h).
uint8_t zxKeymapLookup(uint8_t hid, bool shift);
// (Re)read the overrides from /emu8/zx/keymap.txt (or /zxkeymap.txt). Missing file = defaults only.
void    zxKeymapLoad();

// ---- for the KEYBOARD page (optionsui.cpp); the host keys are the C64 page's ----
const char *zxKeymapZxName(uint8_t index);     // "A", "CAPS", "SYMBOL", "ENTER", "SPACE"
uint8_t zxKeymapZxAt(int i);                    // the picker's order: entry i -> matrix index, 0..39
// The rule as text: "N +SYMBOL", "0 +CAPS", "NONE", "-" (no rule).
void    zxKeymapDescribe(uint8_t code, bool shifted, char *out, size_t n);
// The rule for exactly (hid, shifted): `over` true when it comes from the overrides.
uint8_t zxKeymapRule(uint8_t hid, bool shifted, bool *over);
uint8_t zxKeymapDefault(uint8_t hid, bool shifted);
// Change one rule in memory (ZXK_NONE = back to the default). False when the override table is full.
bool    zxKeymapSet(uint8_t hid, bool shifted, uint8_t code);
void    zxKeymapResetAll();
int     zxKeymapChangedCount();
// Write the overrides to /emu8/zx/keymap.txt. False if the card could not be written.
bool    zxKeymapSave();
