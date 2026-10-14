// device.h — which physical emu8 board the desktop build is pretending to be (see device.cpp).
//
// The desktop ships as two executables built from the same sources:
//   emu8.exe           the ESP32 boards: CYD (320x240), ESP32-S3 JC4827W543 (480x272),
//                      ESP32-P4 JC1060P470 (1024x600)
//   emu8-picocalc.exe  the ClockworkPi PicoCalc (RP2040 / RP2350): 320x320 panel, keyboard-driven UI,
//                      drawn inside a picture of the device whose keys can be clicked
// The PicoCalc's menu layout and keyboard behaviour are compile-time (EMU_SIM_PICOCALC), hence the
// second binary. Picking a device from the other family re-execs the sibling executable.
#pragma once

#if defined(BOARD_DESKTOP)

enum DesktopDevice {
  DEV_CYD = 0,
  DEV_S3,
  DEV_P4,
  DEV_PICOCALC_RP2040,
  DEV_PICOCALC_RP2350,
  DEV_COUNT
};

struct DesktopDeviceInfo {
  const char *id;         // --device=<id> / EMU_DEVICE=<id>
  const char *name;       // menu label
  int         panelW, panelH;
  bool        picocalc;   // lives in emu8-picocalc.exe
  bool        pcxt;       // can run the PC-XT core
};

// Pick the device from --device=<id>, then EMU_DEVICE, then the last one used (emu8-device.cfg next
// to the exe). If it belongs to the other executable, exec that one instead (does not return).
// Sets g_emuArgv so ESP.restart() keeps the device. Call first thing in main(), before SDL_Init.
void desktopDeviceInit(int argc, char **argv);

int                      desktopDevice();
const DesktopDeviceInfo &desktopDeviceInfo(int dev);
inline const DesktopDeviceInfo &desktopDeviceInfo() { return desktopDeviceInfo(desktopDevice()); }
bool                     desktopDeviceAvailable(int dev);   // its executable is next to this one

// Save the choice and restart as `dev` (re-exec, possibly into the sibling executable).
void desktopSwitchDevice(int dev);

#endif // BOARD_DESKTOP
