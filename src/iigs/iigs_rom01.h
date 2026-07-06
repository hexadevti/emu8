// iigs_rom01.h - the Apple IIGS ROM 01 (342-0077-B, 128K) is no longer embedded in flash; it is
// loaded from the SD card (/roms/iigs/rom01.bin) into a PSRAM buffer by iigsLoadRom() in
// iigs_boot.cpp (where `iigsRom01` is defined). ROM banks: $FF = bytes 0..0xFFFF, $FE = 0x10000..
#pragma once
extern const unsigned char* iigsRom01;   // 128K ROM 01 buffer (null until iigsLoadRom() runs)
bool iigsLoadRom();                        // load it from SD; returns false if missing/wrong size
