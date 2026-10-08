#include "../../emu.h"

// Apple II system ROMs, loaded at boot from /roms/ on the SD card (they used to be embedded flash
// byte-arrays in rom.h). The default names are AppleWin's (Apple2_Plus.rom, Apple2e_Enhanced.rom,
// DISK2.rom, MouseInterface.rom, HDDRVR.BIN -- see romsel.cpp). The pointers are declared extern in
// rom.h (pulled in by emu.h everywhere); they are null until apple2LoadRoms() runs.

const unsigned char* rom                  = nullptr;   // $D000-$FFFF native (II+) ROM (12K)
const unsigned char* appleiieenhancedc0ff = nullptr;   // $C000-$FFFF enhanced IIe ROM (16K)
const unsigned char* diskiicardrom        = nullptr;   // $C600 Disk II boot ROM (256)
const unsigned char* mousecardrom         = nullptr;   // $C400 mouse card ROM (first 256 of the 2K)
const unsigned char* hdrom                = nullptr;   // $C700 HD card ROM (256)
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

// Read the first `len` bytes of `slot`'s file (romPath) from the SD card into a fresh buffer. The
// file may be longer, up to the slot's maxSize: only the first `len` bytes are ever addressed, and
// the published images carry more (MouseInterface.rom is the whole 2K card ROM; the old iie.bin and
// diskii.bin dumps had trailing bytes). Returns null on any failure: missing file, wrong size, OOM,
// short read.
static const unsigned char* a2LoadFile(int slot, int len) {
  const char* path = romPath(slot);
  if (!*path) { sprintf(buf, "Apple II: no %s ROM set", romSlotInfo(slot).label); printLog(buf); return nullptr; }
  const int maxLen = (int)romSlotInfo(slot).maxSize;
  // Name the file on the boot screen. These are the slowest step of an Apple II boot -- iie.bin
  // alone is 16KB off an SD card -- and on a cold card they are most of the wait, so this is the
  // step worth showing rather than one "Loading ROMs" for the whole set.
  { char msg[48]; const char *base = strrchr(path, '/');
    snprintf(msg, sizeof(msg), "Loading %s", base ? base + 1 : path);
    bootProgressStep(msg); }
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "Apple II: ROM missing: %s", path); printLog(buf); return nullptr; }
  if ((int)f.size() < len || (int)f.size() > maxLen) {
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

#if BOARD_A2_ROM_IN_FLASH
// The IIe ROM on the PicoCalc: streamed off the card into the spare-flash BIOS window
// (src/picocalc/romflash_picocalc.cpp) and read from there through XIP, so it costs no heap -- the
// IIe memory map leaves the RP2040 too little to hold 16K of ROM next to it. Only one system is ever
// booted, so the window the MSX / C64 / Coleco / ZX use for theirs is free. Unchanged sectors are
// not rewritten, so every boot after the first is a read-and-compare.
static const unsigned char* a2LoadIIeToFlash() {
  const char* path = romPath(ROMSEL_A2_IIE);
  if (!*path) { printLog("Apple II: no IIe ROM set"); return nullptr; }
  { char msg[48]; const char *base = strrchr(path, '/');
    snprintf(msg, sizeof(msg), "Loading %s", base ? base + 1 : path);
    bootProgressStep(msg); }
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "Apple II: ROM missing: %s", path); printLog(buf); return nullptr; }
  if ((int)f.size() < 0x4000 || (int)f.size() > (int)romSlotInfo(ROMSEL_A2_IIE).maxSize) {
    sprintf(buf, "Apple II: ROM %s wrong size (%d, want %d)", path, (int)f.size(), 0x4000);
    printLog(buf); f.close(); return nullptr;
  }
  // Staged through trackRawData: the DiskII track buffer is idle until diskSetup() runs, and a
  // malloc'd 4K here was a transient the IIe heap budget (A2_IIE_RESERVE) had to carry.
  extern uint8_t trackRawData[];
  const uint8_t* fb = romFlashLoad(ROMFLASH_BIOS, f, 0x4000, trackRawData);
  f.close();
  if (!fb) { sprintf(buf, "Apple II: ROM %s: copy to flash failed", path); printLog(buf); }
  return fb;
}
#endif

// Load just the HD card ROM ($C700), for slot 7 when a hard-disk image is mounted; cached, so it is
// safe to call more than once.
bool apple2EnsureHdRom() {
  if (hdrom) return true;
  hdrom = a2LoadFile(ROMSEL_A2_HD, 256);
  return hdrom != nullptr;
}

// Load all five Apple II system ROMs from /roms. Returns false (and sets apple2RomLoadFailed)
// if any is missing or the wrong size; the caller must then NOT run the 6502 - read8 indexes these
// pointers directly and would dereference null.
// One attempt at the set for whichever machine is selected. iiplus.bin and iie.bin are the two
// system ROMs and they are alternatives, not a pair: read8 reaches iiplus.bin from exactly one place
// (memory.cpp, `return rom[address - 0xd000]`) and that line is the else of `if (AppleIIe)`, while
// a IIe takes $D000-$FFFF -- and $C100-$CFFF, and the $C800 space -- from iie.bin. So each machine
// loads one of them and never the other: 13056 heap bytes for a II+, and for a IIe either 17152 or,
// where BOARD_A2_ROM_IN_FLASH copies the IIe ROM into flash, just the 768 bytes of card ROM.
static bool a2LoadRomSet() {
  a2RomOutOfRam = false;                                   // sticky across the whole set
#if BOARD_A2_ROM_IN_FLASH
  // Copied into flash rather than the heap on this board, so a IIe spends nothing here -- see board.h.
  if (AppleIIe) appleiieenhancedc0ff = a2LoadIIeToFlash();
#else
  if (AppleIIe) appleiieenhancedc0ff = a2LoadFile(ROMSEL_A2_IIE, 0x4000);
#endif
  // Paths come from romPath(): the file picked on the settings ROMS page, else /roms/<name>.
  if (!AppleIIe) rom                 = a2LoadFile(ROMSEL_A2_IIPLUS, 0x3000);
  diskiicardrom = a2LoadFile(ROMSEL_A2_DISKII, 256);
  mousecardrom  = a2LoadFile(ROMSEL_A2_MOUSE,  256);
  apple2EnsureHdRom();                                     // hdrom ($C700)
  sprintf(buf, "Apple II: %s ROM set loaded, %u bytes free (%s not read by this machine)",
          AppleIIe ? "IIe" : "II+", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
          AppleIIe ? "II+ ROM" : "IIe ROM");
  printLog(buf);
  return (AppleIIe ? appleiieenhancedc0ff != nullptr : rom != nullptr)
         && diskiicardrom && mousecardrom && hdrom;
}

static void a2FreeRomSet() {
  free((void*)rom);                  rom                  = nullptr;
#if !BOARD_A2_ROM_IN_FLASH
  // Where the IIe ROM was copied into flash this pointer never came from the heap: free() on it
  // would thread a flash address into the free list and corrupt the heap on the very path -- out
  // of RAM, falling back to a II+ -- that most needs the heap intact.
  free((void*)appleiieenhancedc0ff);
#endif
  appleiieenhancedc0ff = nullptr;
  free((void*)diskiicardrom);        diskiicardrom        = nullptr;
  free((void*)mousecardrom);         mousecardrom         = nullptr;
  free((void*)hdrom);                hdrom                = nullptr;   // a2EnsureHdRom reloads it
}

// Load all the Apple II system ROMs from /roms. Returns false (and sets apple2RomLoadFailed)
// if any is missing or the wrong size; the caller must then NOT run the 6502 - read8 indexes these
// pointers directly and would dereference null.
bool apple2LoadRoms() {
  bool ok = a2LoadRomSet();
  // A IIe that ran out of heap anywhere in that set is recoverable, and THIS is the conclusive
  // test: it is the first point where FSSetup's buffers are on the books too, so unlike the
  // estimate in memoryAlloc() it is measuring the real thing. Which file failed does not matter --
  // the 17152-byte set is what does not fit. Give it back along with the 68K of IIe map, then load
  // the set again as a II+: 13056 bytes into a heap with ~85K free.
  if (!ok && AppleIIe && a2RomOutOfRam) {
    a2FreeRomSet();
    apple2FallbackToIIplus("not enough RAM for the IIe ROM set");
    ok = a2LoadRomSet();
  }
  if (ok) printLog("Apple II: ROMs loaded from /roms");
  else    apple2RomLoadFailed = true;   // missing / wrong size: a card problem, so say so on screen
  return ok;
}

// renderLoop hook (src/shared/video.cpp): hold a "ROMs not found" screen while the ROMs are missing.
bool apple2RenderLoadWarning() {
  if (!apple2RomLoadFailed && !apple2MemAllocFailed) return false;
  static bool drawn = false;
  // A missing ROM is fixed from SETTINGS -> ROMS, so yield to the settings window (Ctrl-F1 / F10 /
  // tap) and redraw once it closes. Out of RAM has nothing there to fix: keep holding the screen.
  if (OptionsWindow && !apple2MemAllocFailed) { drawn = false; return false; }
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
      tft.drawString("Put AppleWin's Apple2_Plus.rom, DISK2.rom", 8, 40, 1);
      tft.drawString("Apple2e_Enhanced.rom, MouseInterface.rom,", 8, 54, 1);
      tft.drawString("HDDRVR.BIN in /roms on the SD card,", 8, 68, 1);
      tft.drawString("or pick other files in SETTINGS > ROMS.", 8, 82, 1);
    }
    tft.setTextDatum(MC_DATUM);
    drawn = true;
  }
  return true;
}
