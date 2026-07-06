#include "../../emu.h"

// Apple II system ROMs, loaded at boot from /roms/apple2/*.bin on the SD card (they used to be
// embedded flash byte-arrays in rom.h). The pointers are declared extern in rom.h (pulled in by
// emu.h everywhere); they are null until apple2LoadRoms() runs. Dumped byte-for-byte from the old
// arrays with tools/extract_rom.py into sdcard/roms/apple2/.

const unsigned char* rom                  = nullptr;   // $D000-$FFFF native (II+) ROM (12K)
const unsigned char* appleiieenhancedc0ff = nullptr;   // $C000-$FFFF enhanced IIe ROM (16696)
const unsigned char* diskiicardrom        = nullptr;   // $C600 Disk II boot ROM (560, only $C600-C6FF read)
const unsigned char* mousecardrom         = nullptr;   // $C400 mouse card ROM (256)
const unsigned char* hdrom                = nullptr;   // $C700 HD card ROM (256, also read by IIGS slot 7)
bool apple2RomLoadFailed = false;

// Internal SRAM first (the 6502 executes from rom / the IIe ROM in the hot read8 path), PSRAM
// fallback. Apple II internal DRAM is tight, and these were flash (not DRAM) before, so PSRAM is an
// acceptable home if internal can't take them.
static uint8_t* a2RomAlloc(size_t n) {
  uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t*)ps_malloc(n);
  return p;
}

// Read exactly `len` bytes of `path` from the SD card into a fresh buffer. Returns null on any
// failure: missing file, wrong size, OOM, short read.
static const unsigned char* a2LoadFile(const char* path, int len) {
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "Apple II: ROM missing: %s", path); printLog(buf); return nullptr; }
  if ((int)f.size() != len) {
    sprintf(buf, "Apple II: ROM %s wrong size (%d, want %d)", path, (int)f.size(), len);
    printLog(buf); f.close(); return nullptr;
  }
  uint8_t* b = a2RomAlloc(len);
  if (!b) { printLog("Apple II: ROM alloc failed"); f.close(); return nullptr; }
  int rd = 0;
  while (rd < len) { int n = f.read(b + rd, (len - rd > 8192) ? 8192 : (len - rd)); if (n <= 0) break; rd += n; }
  f.close();
  if (rd != len) { free(b); return nullptr; }
  return b;
}

// Load just the HD card ROM ($C700). Shared with the IIGS, which reads it for slot 7 when a hard-disk
// image is mounted; cached, so it is safe to call from both the Apple II boot and the IIGS.
bool apple2EnsureHdRom() {
  if (hdrom) return true;
  hdrom = a2LoadFile("/roms/apple2/hd.bin", 256);
  return hdrom != nullptr;
}

// Load all five Apple II system ROMs from /roms/apple2. Returns false (and sets apple2RomLoadFailed)
// if any is missing or the wrong size; the caller must then NOT run the 6502 - read8 indexes these
// pointers directly and would dereference null.
bool apple2LoadRoms() {
  rom                  = a2LoadFile("/roms/apple2/main.bin",   0x3000);
  appleiieenhancedc0ff = a2LoadFile("/roms/apple2/iie.bin",    16696);
  diskiicardrom        = a2LoadFile("/roms/apple2/diskii.bin", 560);
  mousecardrom         = a2LoadFile("/roms/apple2/mouse.bin",  256);
  apple2EnsureHdRom();                                     // hdrom ($C700)
  bool ok = rom && appleiieenhancedc0ff && diskiicardrom && mousecardrom && hdrom;
  if (ok) printLog("Apple II: ROMs loaded from /roms/apple2");
  else    apple2RomLoadFailed = true;
  return ok;
}

// renderLoop hook (src/shared/video.cpp): hold a "ROMs not found" screen while the ROMs are missing.
bool apple2RenderLoadWarning() {
  if (!apple2RomLoadFailed) return false;
  static bool drawn = false;
  if (!drawn) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK); tft.drawString("Apple II: ROMs NOT FOUND", 8, 8, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Put main.bin, iie.bin, diskii.bin,", 8, 40, 1);
    tft.drawString("mouse.bin, hd.bin in /roms/apple2", 8, 54, 1);
    tft.drawString("on the SD card.", 8, 68, 1);
    tft.setTextDatum(MC_DATUM);
    drawn = true;
  }
  return true;
}
