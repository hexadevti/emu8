// picocalc_sim.h — the desktop PicoCalc simulator (emu8-picocalc, EMU_SIM_PICOCALC).
//
//   picocalc_kbd.cpp   the PicoCalc keyboard: the STM32's key codes, shift/caps resolution and the
//                      same translation to USB-HID + hotkeys as src/picocalc/input_picocalc.cpp.
//                      Fed by the host keyboard (input_sdl.cpp) and by clicks on the skin.
//   picocalc_skin.cpp  the picture of the device (docs/picocalc.png, embedded) with the emulator
//                      shown in its screen and clickable keys.
#pragma once

#if defined(BOARD_DESKTOP)

#include <stdint.h>

// PicoCalc key codes (src/picocalc/input_picocalc.cpp). Codes below 0x80 are ASCII.
enum {
  PCK_BACKSPACE = 0x08, PCK_TAB = 0x09, PCK_ENTER = 0x0A,
  PCK_F1 = 0x81, PCK_F2 = 0x82, PCK_F3 = 0x83, PCK_F4 = 0x84, PCK_F5 = 0x85,
  PCK_F6 = 0x86, PCK_F7 = 0x87, PCK_F8 = 0x88, PCK_F9 = 0x89, PCK_F10 = 0x8A,
  PCK_MOD_ALT = 0xA1, PCK_MOD_SHL = 0xA2, PCK_MOD_SHR = 0xA3,
  PCK_MOD_SYM = 0xA4, PCK_MOD_CTRL = 0xA5,
  PCK_ESC = 0xB1,
  PCK_LEFT = 0xB4, PCK_UP = 0xB5, PCK_DOWN = 0xB6, PCK_RIGHT = 0xB7,
  PCK_CAPS_LOCK = 0xC1,
  PCK_BREAK = 0xD0, PCK_INSERT = 0xD1, PCK_HOME = 0xD2, PCK_DEL = 0xD4,
  PCK_END = 0xD5, PCK_PAGE_UP = 0xD6, PCK_PAGE_DOWN = 0xD7,
};

// A physical key, named by its unshifted code ('a', '1', PCK_F1, PCK_ESC, PCK_MOD_SHL...). The
// keyboard model applies shift / caps lock / alt the way the STM32 firmware does and remembers what
// it sent, so the release always matches the press.
void pcSimPhysKey(uint8_t key, bool down, bool repeat);
// Host keyboard (SDL scancode) -> pcSimPhysKey. Returns false for keys the PicoCalc does not have.
bool pcSimHostKey(int sdlScancode, bool down, bool repeat);
bool pcSimCapsLock();

// Draw the device inside the current ImGui window, with the emulator texture in its screen, and
// handle clicks on its keys. Returns false if the skin is unavailable (then draw the plain image).
// screen* receive the screen's rect in window pixels.
struct SDL_Renderer; struct SDL_Texture;
bool pcSimSkinDraw(SDL_Renderer *ren, SDL_Texture *emuTex, bool linear, int *screenX, int *screenY,
                   int *screenW, int *screenH);

#endif // BOARD_DESKTOP
