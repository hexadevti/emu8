// This MSX translation unit is compiled out entirely on boards that clear
// BOARD_HAS_MSX_CORE (currently none -- see board.h; it is on for every board).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro;
// the guard below then empties the file, keeping the Z80 core's static RAM out of the image.
#include "../../emu.h"
#if BOARD_HAS_MSX_CORE

// msx_cbios.cpp - the MSX1 main BIOS used to be embedded here as a 32K flash array (dumped from
// resources/HOTBIT12.ROM). It now lives on the SD card at /roms/msx/cbios.rom and is loaded at boot
// by loadBiosFromSD() in msx.cpp. The byte-for-byte dump is in sdcard/roms/msx/cbios.rom
// (tools/extract_rom.py). This file is intentionally empty - kept so existing build globs/includes
// stay valid.

#endif  // BOARD_HAS_MSX_CORE
