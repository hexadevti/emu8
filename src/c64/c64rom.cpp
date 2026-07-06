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
static const unsigned char *loadRomFile(const char *path, int len) {
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "C64: ROM missing: %s", path); printLog(buf); return nullptr; }
  if ((int)f.size() != len) {
    sprintf(buf, "C64: ROM %s wrong size (%d, want %d)", path, (int)f.size(), len);
    printLog(buf); f.close(); return nullptr;
  }
  uint8_t *b = (uint8_t *)ps_malloc(len);
  if (!b) b = (uint8_t *)malloc(len);          // no PSRAM available -> regular heap
  if (!b) { printLog("C64: ROM alloc failed"); f.close(); return nullptr; }
  int got = f.read(b, len);
  f.close();
  if (got != len) { free(b); return nullptr; }
  return b;
}

// Load all three system ROMs from /roms/c64 on the SD card. Returns false if any is missing
// or the wrong size; the caller must then show an error and NOT run the 6510 (the read paths
// index these pointers directly and would dereference null).
bool c64LoadRoms() {
  basic_rom   = loadRomFile("/roms/c64/basic.bin",   0x2000);
  kernal_rom  = loadRomFile("/roms/c64/kernal.bin",  0x2000);
  charset_rom = loadRomFile("/roms/c64/chargen.bin", 0x1000);
  bool ok = basic_rom && kernal_rom && charset_rom;
  if (ok) printLog("C64: ROMs loaded from /roms/c64 (BASIC + KERNAL + CHARGEN)");
  return ok;
}
