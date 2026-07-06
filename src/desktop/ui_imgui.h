// ui_imgui.h — desktop-only native UI shell (Dear ImGui over SDL2 + SDL_Renderer).
//
// This is the "independent desktop interface": a resizable, dockable window with a crisp native menu
// bar, settings, and debug panels, instead of upscaling the device's 320x240 on-screen UI. It shares
// the SAME SDL window/renderer the emulator already created (display_sdl.cpp) and presents the
// emulator's framebuffer as a texture INSIDE an ImGui dockspace. None of this touches the emulator
// cores — it lives entirely behind BOARD_DESKTOP, so device builds are unaffected and the cores stay
// shared + debuggable.
#pragma once

#include "../../board.h"

#if defined(BOARD_DESKTOP)

#include <SDL.h>

// Create/destroy the ImGui context + SDL backends. Shares the existing window + renderer.
void desktopUiInit(SDL_Window *win, SDL_Renderer *ren);
void desktopUiShutdown();

// Persistence of the desktop session. Load BEFORE the window is created (it carries the saved window
// size + view prefs + which panels are open); Save on a clean quit (also flushes the ImGui dock
// layout to imgui.ini and the emulator settings/last-disk to eeprom.bin via saveConfig()).
void desktopUiLoadConfig();                    // read emu8.cfg into the UI state
void desktopUiGetWindowSize(int *w, int *h);   // saved window size (0 = none stored yet)
void desktopUiSaveState();                     // write emu8.cfg + imgui.ini + saveConfig()

// Feed every SDL event to ImGui (call from the input pump, on the main thread). The WantCapture
// flags tell the emulator input layer whether ImGui consumed the keyboard/mouse this frame.
void desktopUiProcessEvent(const SDL_Event *e);
bool desktopUiWantCaptureMouse();
bool desktopUiWantCaptureKeyboard();

// Build one UI frame and present it: menu bar + emulator image (aspect-fit, dockable) + debug panels,
// then ImGui render + SDL_RenderPresent. `emuTex` is the live 320x240 framebuffer texture.
void desktopUiFrame(SDL_Texture *emuTex, int fbW, int fbH);

// Map a window-pixel point to emulator framebuffer coords using the last drawn image rect. Returns
// false when the point is outside the emulator image (so a click on the ImGui chrome isn't a "touch").
bool desktopUiMapToEmu(int winX, int winY, int *outFbX, int *outFbY);

#endif // BOARD_DESKTOP
