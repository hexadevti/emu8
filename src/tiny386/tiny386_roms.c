#if !defined(BOARD_JC4827W543)  // tiny386 is not built for the S3 board (too big; vendored core not wired for the device toolchain)
// tiny386_roms.c - the SeaBIOS + VGABIOS images (LGPLv3) used to be embedded here as ~168K of flash
// arrays (seabios_rom / vgabios_rom). They now live on the SD card at /roms/tiny386/seabios.bin and
// /roms/tiny386/vgabios.bin and are loaded at boot by load_rom() in tiny386_core.cpp (via
// t386_read_sd in tiny386.cpp). Byte-for-byte dumps are in sdcard/roms/tiny386/ (tools/extract_rom.py).
// This file is intentionally empty - kept so the build globs stay valid.

#endif // !defined(BOARD_JC4827W543)
