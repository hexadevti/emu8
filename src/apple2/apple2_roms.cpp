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

// Set by a2LoadFile when the file itself was fine and only the heap was not. The IIe fallback
// needs to tell those apart: a missing or truncated iie.bin is a card problem and must stay on the
// red ROMs NOT FOUND screen that says which files to copy, while an unallocatable one is a RAM
// problem the board can recover from by coming up as a II+.
static bool a2RomOutOfRam = false;

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
  // Name the file on the boot screen. These are the slowest step of an Apple II boot -- iie.bin
  // alone is 16KB off an SD card -- and on a cold card they are most of the wait, so this is the
  // step worth showing rather than one "Loading ROMs" for the whole set.
  { char msg[48]; const char *base = strrchr(path, '/');
    snprintf(msg, sizeof(msg), "Loading %s", base ? base + 1 : path);
    bootProgressStep(msg); }
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "Apple II: ROM missing: %s", path); printLog(buf); return nullptr; }
  if ((int)f.size() != len) {
    sprintf(buf, "Apple II: ROM %s wrong size (%d, want %d)", path, (int)f.size(), len);
    printLog(buf); f.close(); return nullptr;
  }
  uint8_t* b = a2RomAlloc(len);
  if (!b) {
    sprintf(buf, "Apple II: no RAM for %s (%d bytes, %u free)", path, len,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    printLog(buf);
    a2RomOutOfRam = true;
    f.close();
    return nullptr;
  }
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
// One attempt at the set for whichever machine is selected. main.bin and iie.bin are the two
// system ROMs and they are alternatives, not a pair: read8 reaches main.bin from exactly one place
// (memory.cpp, `return rom[address - 0xd000]`) and that line is the else of `if (AppleIIe)`, while
// a IIe takes $D000-$FFFF -- and $C100-$CFFF, and the $C800 space -- from iie.bin. So each machine
// loads one of them and never the other: 13360 heap bytes for a II+, and for a IIe either 17768 or,
// where BOARD_A2_ROM_IN_FLASH puts iie.bin in flash, just the 1072 bytes of card ROM.
static bool a2LoadRomSet() {
  a2RomOutOfRam = false;                                   // sticky across the whole set
#if BOARD_A2_ROM_IN_FLASH
  // Already in flash on this board, so a IIe spends nothing here -- see board.h and
  // src/apple2/iie_rom_flash.cpp. Assigning rather than loading also means a missing or damaged
  // iie.bin on the card cannot stop a IIe from booting.
  if (AppleIIe) appleiieenhancedc0ff = apple2IIeRomFlash;
#else
  if (AppleIIe) appleiieenhancedc0ff = a2LoadFile("/roms/apple2/iie.bin",  16696);
#endif
  if (!AppleIIe) rom                 = a2LoadFile("/roms/apple2/main.bin", 0x3000);
  diskiicardrom = a2LoadFile("/roms/apple2/diskii.bin", 560);
  mousecardrom  = a2LoadFile("/roms/apple2/mouse.bin",  256);
  apple2EnsureHdRom();                                     // hdrom ($C700)
  sprintf(buf, "Apple II: %s ROM set loaded, %u bytes free (%s not read by this machine)",
          AppleIIe ? "IIe" : "II+", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
          AppleIIe ? "main.bin" : "iie.bin");
  printLog(buf);
  return (AppleIIe ? appleiieenhancedc0ff != nullptr : rom != nullptr)
         && diskiicardrom && mousecardrom && hdrom;
}

static void a2FreeRomSet() {
  free((void*)rom);                  rom                  = nullptr;
  free((void*)appleiieenhancedc0ff); appleiieenhancedc0ff = nullptr;
  free((void*)diskiicardrom);        diskiicardrom        = nullptr;
  free((void*)mousecardrom);         mousecardrom         = nullptr;
  free((void*)hdrom);                hdrom                = nullptr;   // a2EnsureHdRom reloads it
}

// Load all the Apple II system ROMs from /roms/apple2. Returns false (and sets apple2RomLoadFailed)
// if any is missing or the wrong size; the caller must then NOT run the 6502 - read8 indexes these
// pointers directly and would dereference null.
bool apple2LoadRoms() {
  bool ok = a2LoadRomSet();
  // A IIe that ran out of heap anywhere in that set is recoverable, and THIS is the conclusive
  // test: it is the first point where FSSetup's buffers are on the books too, so unlike the
  // estimate in memoryAlloc() it is measuring the real thing. Which file failed does not matter --
  // the 17768-byte set is what does not fit. Give it back along with the 68K of IIe map, then load
  // the set again as a II+: 13360 bytes into a heap with ~85K free.
  if (!ok && AppleIIe && a2RomOutOfRam) {
    a2FreeRomSet();
    apple2FallbackToIIplus("not enough RAM for the IIe ROM set");
    ok = a2LoadRomSet();
  }
  if (ok) printLog("Apple II: ROMs loaded from /roms/apple2");
  else    apple2RomLoadFailed = true;   // missing / wrong size: a card problem, so say so on screen
  return ok;
}

// renderLoop hook (src/shared/video.cpp): hold a "ROMs not found" screen while the ROMs are missing.
bool apple2RenderLoadWarning() {
  if (!apple2RomLoadFailed && !apple2MemAllocFailed) return false;
  static bool drawn = false;
  if (!drawn) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    if (apple2MemAllocFailed) {
      // Out of RAM, not a missing file. Worth its own screen: the two look identical from the
      // outside (halted 6502) but nothing the user does to the SD card will fix this one.
      tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK);
      tft.drawString("Apple II: NOT ENOUGH RAM", 8, 8, 2);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("This board could not allocate the", 8, 40, 1);
      tft.drawString("Apple II memory map (~131 KB).", 8, 54, 1);
      tft.drawString("See the serial log for which block", 8, 68, 1);
      tft.drawString("failed.", 8, 82, 1);
    } else {
      tft.setTextColor(tft.color565(220, 40, 40), TFT_BLACK);
      tft.drawString("Apple II: ROMs NOT FOUND", 8, 8, 2);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("Put main.bin, iie.bin, diskii.bin,", 8, 40, 1);
      tft.drawString("mouse.bin, hd.bin in /roms/apple2", 8, 54, 1);
      tft.drawString("on the SD card.", 8, 68, 1);
    }
    tft.setTextDatum(MC_DATUM);
    drawn = true;
  }
  return true;
}
