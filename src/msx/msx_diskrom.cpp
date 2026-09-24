// This MSX translation unit is compiled out entirely on boards that clear
// BOARD_HAS_Z80_CORES (the PicoCalc's original RP2040 mainboard -- see board.h for why).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro;
// the guard below then empties the file, handing this core's static RAM to the Apple II.
#include "../../emu.h"
#if BOARD_HAS_Z80_CORES

// msx_diskrom.cpp - the MSX disk-interface ROM (Hotbit HB3600) used to be embedded here as a 16K
// flash array. It now lives on the SD card at /roms/msx/diskrom.rom and is loaded on first disk
// mount by msxEnsureDiskRom() in msx.cpp. Byte-for-byte dump in sdcard/roms/msx/diskrom.rom
// (tools/extract_rom.py). Intentionally empty - kept so existing build globs/includes stay valid.

#endif  // BOARD_HAS_Z80_CORES
