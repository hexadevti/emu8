#include "../../emu.h"
#include "c64.h"

// C64 system ROMs: BASIC ($A000-$BFFF), KERNAL ($E000-$FFFF) and CHARGEN ($D000-$DFFF).
// These used to be embedded here as ~20K of flash byte-arrays. They now live on the SD card
// under /roms/c64/ and are loaded into PSRAM buffers at boot by c64LoadRoms(), so the firmware
// no longer carries them. The .bin files were dumped byte-for-byte from the old arrays with
// tools/extract_rom.py (see sdcard/roms/c64/ in the repo for the staging copies).

const unsigned char *basic_rom   = nullptr;   // $A000-$BFFF  (8K)
const unsigned char *kernal_rom  = nullptr;   // $E000-$FFFF  (8K)
const unsigned char *charset_rom = nullptr;   // $D000-$DFFF  (4K character generator)

// Read exactly `len` bytes of `path` from the SD card into a fresh buffer. Prefer PSRAM so
// these read-mostly ROMs don't compete with the C64's 64K work RAM / VIC framebuffer for the
// scarce internal-DRAM region (they were in flash, not DRAM, before). Returns null on any
// failure: missing file, wrong size, OOM, or a short read.
// PicoCalc (BOARD_ROM_IN_FLASH): the three ROMs go into the flash BIOS window at `flashOff` instead
// (the C64 has no other use for it) and are read in place through XIP. On the heap they cost 20K of
// the RP2040's ~124K and left a running C64 with under 1K free: SD opens, directory scans and CRT
// loads then failed or threw at random. Called from c64Setup before vicSetup, so sharedBigBuf (the
// framebuffer later) is free to stage the 4K sectors.
static const unsigned char *loadRomFile(const char *path, int len, uint32_t flashOff) {
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "C64: ROM missing: %s", path); printLog(buf); return nullptr; }
  if ((int)f.size() != len) {
    sprintf(buf, "C64: ROM %s wrong size (%d, want %d)", path, (int)f.size(), len);
    printLog(buf); f.close(); return nullptr;
  }
#if BOARD_ROM_IN_FLASH
  const uint8_t *fb = romFlashLoad(ROMFLASH_BIOS, f, len, sharedBigBuf, flashOff);
  f.close();
  if (!fb) { sprintf(buf, "C64: ROM %s: copy to flash failed", path); printLog(buf); }
  return fb;
#else
  (void)flashOff;
  uint8_t *b = (uint8_t *)ps_malloc(len);
  if (!b) b = (uint8_t *)malloc(len);          // no PSRAM available -> regular heap
  if (!b) { printLog("C64: ROM alloc failed"); f.close(); return nullptr; }
  int got = f.read(b, len);
  f.close();
  if (got != len) { free(b); return nullptr; }
  return b;
#endif
}

// Load all three system ROMs from /roms/c64 on the SD card. Returns false if any is missing
// or the wrong size; the caller must then show an error and NOT run the 6510 (the read paths
// index these pointers directly and would dereference null).
bool c64LoadRoms() {
  // romPath(): the file picked on the settings ROMS page, else /roms/c64/<name>.
  basic_rom   = loadRomFile(romPath(ROMSEL_C64_BASIC),   0x2000, 0x0000);
  kernal_rom  = loadRomFile(romPath(ROMSEL_C64_KERNAL),  0x2000, 0x2000);
  charset_rom = loadRomFile(romPath(ROMSEL_C64_CHARGEN), 0x1000, 0x4000);
  bool ok = basic_rom && kernal_rom && charset_rom;
  if (ok) printLog("C64: ROMs loaded (BASIC + KERNAL + CHARGEN)");
  return ok;
}
