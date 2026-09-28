#include "../../emu.h"

// romsel.cpp - user-picked system ROM files, one per ROM a platform needs.
//
// Every core loads its system ROMs from fixed names under /roms/<platform>/ on the SD card. The
// settings menu's ROMS page (optionsui.cpp) lets the user point any of them at another file on the
// card instead; the choice is saved in EEPROM straight away and the loaders read it through
// romPath() on the next boot. An empty (or invalid) slot means "the default name", so a fresh
// device, and one upgraded from firmware that had no such slots, behaves exactly as before.
//
// Storage: ROMSEL_COUNT slots of ROMSEL_SLOT_LEN bytes, each a length-prefixed string like the
// other file names. Everywhere but the PicoCalc they sit in EEPROM from RomSelEEPROMaddress. On the
// PicoCalc, arduino-pico's EEPROM.begin() mirrors the whole EEPROM in heap, and those 832 bytes are
// more than the RP2040 can spare (the C64 file browser ran out of heap). There the slots get a flash
// sector of their own instead -- the top 4K of the unused flash-filesystem partition, just below
// the EEPROM sector -- read in place through XIP, so they cost no RAM at all while the machine runs.

static const RomSlotInfo kSlots[ROMSEL_COUNT] = {
  // platform          model label        default path                      min      max
  { PLATFORM_APPLE2,   0,  "II+ ROM",     "/roms/apple2/iiplus.bin",        0x3000,  0x3000 },
  { PLATFORM_APPLE2,   1,  "IIe ROM",     "/roms/apple2/iie.bin",           16696,   16696  },
  { PLATFORM_APPLE2,  -1,  "DISK II",     "/roms/apple2/diskii.bin",        560,     560    },
  { PLATFORM_APPLE2,  -1,  "MOUSE",       "/roms/apple2/mouse.bin",         256,     256    },
  { PLATFORM_APPLE2,  -1,  "HD CARD",     "/roms/apple2/hd.bin",            256,     256    },
  { PLATFORM_C64,     -1,  "BASIC",       "/roms/c64/basic.bin",            0x2000,  0x2000 },
  { PLATFORM_C64,     -1,  "KERNAL",      "/roms/c64/kernal.bin",           0x2000,  0x2000 },
  { PLATFORM_C64,     -1,  "CHARGEN",     "/roms/c64/chargen.bin",          0x1000,  0x1000 },
  { PLATFORM_MSX,     -1,  "BIOS",        "/roms/msx/cbios.rom",            0x4000,  0x8000 },
  { PLATFORM_MSX,     -1,  "DISK ROM",    "/roms/msx/diskrom.rom",          0x4000,  0x4000 },
  { PLATFORM_COLECO,  -1,  "BIOS",        "/roms/coleco/coleco.rom",        0x2000,  0x2000 },
  { PLATFORM_ZX,      -1,  "48K ROM",     "/roms/zxspectrum/spec48.rom",    0x4000,  0x4000 },
  { PLATFORM_PCXT,    -1,  "BIOS",        "/roms/pcxt/bios.bin",            1,       0x10000 },
};

const RomSlotInfo &romSlotInfo(int slot) { return kSlots[slot]; }

static const int kSlotBytes = ROMSEL_COUNT * ROMSEL_SLOT_LEN;

#if defined(BOARD_PICOCALC)
#include <hardware/flash.h>
extern "C" uint8_t _FS_end;

// Erased flash reads 0xFF, which romOverride() already takes as "no pick".
static const uint8_t *romSelSector() { return &_FS_end - FLASH_SECTOR_SIZE; }
static uint8_t romSelRead(int off)   { return romSelSector()[off]; }

// Rewrite the slot area with `len` bytes at `off` replaced. The rest is staged in a short-lived
// buffer (the ROMS page has already dropped its file list by now) and programmed with the same
// sequence EEPROM.commit() and romflash_picocalc.cpp use: the other core parked, and under
// FreeRTOS interrupts masked and the scheduler suspended, so nothing fetches from flash meanwhile.
static bool romSelWrite(int off, const uint8_t *data, int len)
{
  const uint8_t *sec = romSelSector();
  if (memcmp(sec + off, data, len) == 0) return true;               // unchanged: no erase
  const size_t n = (kSlotBytes + FLASH_PAGE_SIZE - 1) & ~(size_t)(FLASH_PAGE_SIZE - 1);
  uint8_t *img = (uint8_t *)malloc(n);
  if (!img) { printLog("ROMSEL: no heap to stage the flash write"); return false; }
  memcpy(img, sec, n);
  memcpy(img + off, data, len);
  const uint32_t at = (uint32_t)((uintptr_t)sec - XIP_BASE);
#ifndef __FREERTOS
  noInterrupts();
#endif
  rp2040.idleOtherCore();
  flash_range_erase(at, FLASH_SECTOR_SIZE);
  flash_range_program(at, img, n);
  rp2040.resumeOtherCore();
#ifndef __FREERTOS
  interrupts();
#endif
  free(img);
  return memcmp(sec + off, data, len) == 0;
}
#else
static uint8_t romSelRead(int off) { return EEPROM.read(RomSelEEPROMaddress + off); }

static bool romSelWrite(int off, const uint8_t *data, int len)
{
  for (int i = 0; i < len; i++) EEPROM.write(RomSelEEPROMaddress + off + i, data[i]);
  EEPROM.commit();
  return true;
}
#endif

bool romSlotVisible(int slot)
{
  const RomSlotInfo &s = kSlots[slot];
  if (s.platform != currentPlatform) return false;
  if (s.platform == PLATFORM_APPLE2 && s.a2Model >= 0) {
    if (s.a2Model != (AppleIIe ? 1 : 0)) return false;   // only the system ROM this machine reads
#if BOARD_A2_ROM_IN_FLASH
    if (s.a2Model == 1) return false;                    // iie.bin is built into the firmware here
#endif
  }
  return true;
}

// The saved path for `slot`, or nullptr when it is empty / not a plausible path (fresh or
// never-written EEPROM reads as 0x00 or 0xFF). Returns a static buffer: copy it before the next call.
const char *romOverride(int slot)
{
  static char path[ROMSEL_SLOT_LEN];
  if (slot < 0 || slot >= ROMSEL_COUNT) return nullptr;
  const int a = slot * ROMSEL_SLOT_LEN;
  const int len = romSelRead(a);
  if (len <= 0 || len >= ROMSEL_SLOT_LEN) return nullptr;
  for (int i = 0; i < len; i++) path[i] = (char)romSelRead(a + 1 + i);
  path[len] = 0;
  if (path[0] != '/' || strlen(path) != (size_t)len) return nullptr;
  return path;
}

// The file to load for `slot`: the user's pick if there is one, else the default name.
const char *romPath(int slot)
{
  const char *o = romOverride(slot);
  return o ? o : kSlots[slot].defPath;
}

// Save (or, with nullptr / "", clear back to the default) and commit now, so the pick survives a
// power cut before the menu is closed.
bool romSetOverride(int slot, const char *path)
{
  if (slot < 0 || slot >= ROMSEL_COUNT) return false;
  const size_t len = path ? strlen(path) : 0;
  if (len >= ROMSEL_SLOT_LEN) return false;
  uint8_t rec[ROMSEL_SLOT_LEN];
  rec[0] = (uint8_t)len;
  memcpy(rec + 1, path ? path : "", len);
  if (!romSelWrite(slot * ROMSEL_SLOT_LEN, rec, 1 + (int)len)) return false;
  sprintf(buf, "ROMSEL: %s %s -> %s", kSlots[slot].label, kSlots[slot].defPath, len ? path : "(default)");
  printLog(buf);
  return true;
}
