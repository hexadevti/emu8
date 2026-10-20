// apple2keymap.h - host keyboard (HID usage code) -> Apple II+ / IIe keyboard map. See apple2keymap.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

// A resolved Apple key is one byte: bits 0-5 = the key (A2K_* below), bits 6-7 = what the key
// does to the Apple's SHIFT / CTRL while held. Codes >= 0xFD are special; the same NONE / UNMAP
// values as the C64 map (c64keymap.h), and RESET on the C64's RESTORE value, so the KEYBOARD pages
// (optionsui.cpp) handle both maps alike.
#define A2K_SHIFT_KEEP 0      // SHIFT as the host keyboard has it
#define A2K_SHIFT_ON   1      // SHIFT held (Shift-2 '"', Shift-P '@', ...)
#define A2K_SHIFT_OFF  2      // SHIFT released (the host's Shift-; ':' types the Apple's unshifted ':')
#define A2K_CTRL_ON    3      // CTRL held, SHIFT as typed (Up = CTRL-K, as the IIe's up-arrow sends)
#define A2K_NONE       0xFF   // no rule for this key
#define A2K_UNMAP      0xFE   // a rule saying "this key does nothing"
#define A2K_RESET      0xFD   // RESET: not a character, it resets the 6502

// The Apple II+ keys, in the picker's order.
enum : uint8_t {
  A2K_A, A2K_Z = A2K_A + 25,
  A2K_1, A2K_0 = A2K_1 + 9,          // 1..9, then 0
  A2K_COLON, A2K_MINUS, A2K_SEMICOLON, A2K_COMMA, A2K_PERIOD, A2K_SLASH,
  A2K_SPACE, A2K_RETURN, A2K_ESC, A2K_LEFT, A2K_RIGHT, A2K_REPT, A2K_CTRL, A2K_LSHIFT, A2K_RSHIFT,
  // The IIe's own keys (it has no COLON or REPT key; those stay II+ only).
  A2K_DELETE, A2K_TAB, A2K_UP, A2K_DOWN, A2K_EQUAL, A2K_LBRACKET, A2K_RBRACKET, A2K_BACKSLASH,
  A2K_QUOTE, A2K_GRAVE,
  // Held / toggle keys, LAST: they take no +CTRL, and CTRL on them (mode 3, bits 6-7 = 11) would read
  // as 0xFD..0xFF = RESET / UNMAP / NONE. apple2KeymapSet and the file reader strip their mode.
  A2K_CAPS, A2K_OAPPLE, A2K_SAPPLE,
  A2K_NKEYS
};

// Right Alt / GUI (open-apple is the left one, C64K_HID_ALT; solid-apple this one). Its HID usage.
#define A2K_HID_RALT   0xE6

static inline bool    a2kIsKey(uint8_t c) { return c < A2K_RESET && (c & 63) < A2K_NKEYS; }
static inline uint8_t a2kIndex(uint8_t c) { return c & 63; }
static inline uint8_t a2kMode(uint8_t c)  { return c >> 6; }

// The Apple key for host key `hid` typed with the host SHIFT `shift` down: a SHIFT+ rule first,
// then the plain rule; the SD card's overrides before the built-in default in each case.
uint8_t apple2KeymapLookup(uint8_t hid, bool shift);
// The code key `index` sends with SHIFT / CTRL down, as the model's keyboard encoder makes it (bit 7
// set, ready for keymem; the IIe's letters also follow apple2CapsLock); 0 for the keys that send
// nothing (REPT, CTRL, SHIFT, CAPS LOCK, the apple keys).
uint8_t apple2KeyCode(uint8_t index, bool shift, bool ctrl);
// The IIe's CAPS LOCK (on at power-up, as the IIe's key is usually left down): letters typed
// without SHIFT are capitals while it is on. The CAPS LOCK key toggles it; the II+ ignores it.
extern bool apple2CapsLock;
// A key that is held rather than typed and takes no SHIFT / CTRL mode (CAPS LOCK, the apple keys):
// the KEYBOARD page skips the SHIFT step for it.
static inline bool a2kIsPlainOnly(uint8_t index) { return index >= A2K_CAPS && index < A2K_NKEYS; }
// (Re)read the overrides from /emu8/apple2/keymap.txt (or /apple2keymap.txt); the IIe from keymap-iie.txt
// (or /apple2keymap-iie.txt). Missing file = defaults only.
void apple2KeymapLoad();

// ---- for the KEYBOARD page (optionsui.cpp) ----
// The host keys the page lists: the C64 page's (c64keymap.cpp), then RALT.
int     apple2KeymapHostCount();
uint8_t apple2KeymapHostAt(int i);
void    apple2KeymapHostName(uint8_t hid, char *out, size_t n);   // c64KeymapHostName + "RALT"
uint8_t apple2KeymapHostParse(const char *s);                     // the reverse, 0 = unknown
const char *apple2KeymapKeyName(uint8_t index);                   // "A", "COLON", "REPT"
// The Apple keys the picker offers, in its order (the model's keyboard: the II+'s 51 or the IIe's 62).
int     apple2KeymapKeyCount();
uint8_t apple2KeymapKeyAt(int i);
// The rule as text: "P +SHIFT", "K +CTRL", "RESET", "NONE", "-" (no rule).
void    apple2KeymapDescribe(uint8_t code, char *out, size_t n);
// The rule for exactly (hid, shifted): `over` true when it comes from the overrides.
uint8_t apple2KeymapRule(uint8_t hid, bool shifted, bool *over);
uint8_t apple2KeymapDefault(uint8_t hid, bool shifted);
// Change one rule in memory (A2K_NONE = back to the default). False when the override table is full.
bool    apple2KeymapSet(uint8_t hid, bool shifted, uint8_t code);
void    apple2KeymapResetAll();
int     apple2KeymapChangedCount();
// Write the overrides to /emu8/apple2/keymap.txt (IIe: keymap-iie.txt). False if the card could not be written.
bool    apple2KeymapSave();
