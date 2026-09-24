// Not built for the PicoCalc (RP2350): this core needs multi-megabyte ps_malloc'd guest RAM,
// and the board has 520KB of SRAM with its 8MB PSRAM on plain GPIOs (not memory-mapped). The
// shared dispatch/render/UI code links against src/picocalc/bigram_stubs.cpp instead.
#if !defined(BOARD_PICOCALC)
// iigs_rom01.cpp - the embedded 128K ROM 01 array was removed. ROM 01 now lives on the SD card at
// /roms/iigs/rom01.bin and is loaded by iigsLoadRom() (defined in iigs_boot.cpp, where the
// `iigsRom01` pointer also lives). This file is intentionally empty and remains gitignored.
// NOTE: do NOT re-run gen_rom.py - it would regenerate the old array here and duplicate the symbol.
#endif // !defined(BOARD_PICOCALC)
