// msx_diskrom.cpp - the MSX disk-interface ROM (Hotbit HB3600) used to be embedded here as a 16K
// flash array. It now lives on the SD card at /roms/msx/diskrom.rom and is loaded on first disk
// mount by msxEnsureDiskRom() in msx.cpp. Byte-for-byte dump in sdcard/roms/msx/diskrom.rom
// (tools/extract_rom.py). Intentionally empty - kept so existing build globs/includes stay valid.
