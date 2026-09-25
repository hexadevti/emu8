// This MSX translation unit is compiled out entirely on boards that clear
// BOARD_HAS_MSX_CORE (currently none -- see board.h; it is on for every board).
// emu.h must be included FIRST because it is what pulls in board.h and defines the macro;
// the guard below then empties the file, keeping the Z80 core's static RAM out of the image.
#include "../../emu.h"
#if BOARD_HAS_MSX_CORE

// msx_diskrom.cpp - the MSX disk-interface ROM (Hotbit HB3600) used to be embedded here as a 16K
// flash array. It now lives on the SD card at /roms/msx/diskrom.rom and is loaded on first disk
// mount by msxEnsureDiskRom() in msx.cpp. Byte-for-byte dump in sdcard/roms/msx/diskrom.rom
// (tools/extract_rom.py). Intentionally empty - kept so existing build globs/includes stay valid.

#endif  // BOARD_HAS_MSX_CORE
