// msx_cbios.cpp - the MSX1 main BIOS used to be embedded here as a 32K flash array (dumped from
// resources/HOTBIT12.ROM). It now lives on the SD card at /roms/msx/cbios.rom and is loaded at boot
// by loadBiosFromSD() in msx.cpp. The byte-for-byte dump is in sdcard/roms/msx/cbios.rom
// (tools/extract_rom.py). This file is intentionally empty - kept so existing build globs/includes
// stay valid.
