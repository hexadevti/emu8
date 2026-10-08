#include "../../emu.h"

// romsel.cpp - user-picked system ROM files, one per ROM a platform needs.
//
// Every core loads its system ROMs from fixed names in /roms/ on the SD card -- the names the files
// are published under upstream (AppleWin, VICE, blueMSX, Fuse...; see "ROM sources" in README.md), so
// a download can be copied to the card as is. The settings menu's ROMS page (optionsui.cpp) lets the
// user point any of them at another file on the card instead; the choice is saved in EEPROM straight
// away and the loaders read it through romPath() on the next boot. An empty (or invalid) slot means
// "the default name", so a fresh device, and one upgraded from firmware that had no such slots,
// loads the defaults. A slot can also be set to no file at all (ROMSEL_NONE): romPath() is then ""
// and the loader treats the ROM as missing.
//
// The "/roms" in the table below is only the factory ROM base folder: romDefaultPath() swaps in the
// one set in the general settings (romBaseDir), which is stored after the slots with the apps base.
//
// Storage: ROMSEL_RECORDS records of ROMSEL_SLOT_LEN bytes (the ROM slots, then the two base
// folders), each a length-prefixed string like the other file names. Everywhere but the PicoCalc they sit in EEPROM from RomSelEEPROMaddress. On the
// PicoCalc, arduino-pico's EEPROM.begin() mirrors the whole EEPROM in heap, and those 832 bytes are
// more than the RP2040 can spare (the C64 file browser ran out of heap). There the slots get a flash
// sector of their own instead -- the top 4K of the unused flash-filesystem partition, just below
// the EEPROM sector -- read in place through XIP, so they cost no RAM at all while the machine runs.

static const RomSlotInfo kSlots[ROMSEL_COUNT] = {
  // platform          model label        default path                      min      max
  { PLATFORM_APPLE2,   0,  "II+ ROM",     "/roms/Apple2_Plus.rom",          0x3000,  0x3000 },
  { PLATFORM_APPLE2,   1,  "IIe ROM",     "/roms/Apple2e_Enhanced.rom",     0x4000,  16696  },
  { PLATFORM_APPLE2,  -1,  "DISK II",     "/roms/DISK2.rom",                256,     560    },
  { PLATFORM_APPLE2,  -1,  "MOUSE",       "/roms/MouseInterface.rom",       256,     0x800  },
  { PLATFORM_APPLE2,  -1,  "HD CARD",     "/roms/HDDRVR.BIN",               256,     256    },
  { PLATFORM_C64,     -1,  "BASIC",       "/roms/basic-901226-01.bin",      0x2000,  0x2000 },
  { PLATFORM_C64,     -1,  "KERNAL",      "/roms/kernal-901227-03.bin",     0x2000,  0x2000 },
  { PLATFORM_C64,     -1,  "CHARGEN",     "/roms/chargen-901225-01.bin",    0x1000,  0x1000 },
  { PLATFORM_MSX,     -1,  "BIOS",        "/roms/hotbit12.rom",             0x4000,  0x8000 },
  { PLATFORM_MSX,     -1,  "DISK ROM",    "/roms/hb3600_disk.rom",           0x4000,  0x4000 },
  { PLATFORM_COLECO,  -1,  "BIOS",        "/roms/coleco.rom",               0x2000,  0x2000 },
  { PLATFORM_ZX,      -1,  "48K ROM",     "/roms/48.rom",                   0x4000,  0x4000 },
  { PLATFORM_PCXT,    -1,  "BIOS",        "/roms/pcxt_bios.bin",            1,       0x10000 },
};

const RomSlotInfo &romSlotInfo(int slot) { return kSlots[slot]; }

static const int kSlotBytes = ROMSEL_RECORDS * ROMSEL_SLOT_LEN;   // the ROM slots + the two base folders

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

// EEPROM reset: an erased sector is every slot and both base folders back to their defaults.
void romSelEraseAll()
{
  const uint32_t at = (uint32_t)((uintptr_t)romSelSector() - XIP_BASE);
#ifndef __FREERTOS
  noInterrupts();
#endif
  rp2040.idleOtherCore();
  flash_range_erase(at, FLASH_SECTOR_SIZE);
  rp2040.resumeOtherCore();
#ifndef __FREERTOS
  interrupts();
#endif
}
#else
static uint8_t romSelRead(int off) { return EEPROM.read(RomSelEEPROMaddress + off); }

static bool romSelWrite(int off, const uint8_t *data, int len)
{
  for (int i = 0; i < len; i++) EEPROM.write(RomSelEEPROMaddress + off + i, data[i]);
  EEPROM.commit();
  return true;
}

void romSelEraseAll()
{
  for (int i = 0; i < kSlotBytes; i++) EEPROM.write(RomSelEEPROMaddress + i, 0xFF);
  EEPROM.commit();
}
#endif

bool romSlotVisible(int slot)
{
  const RomSlotInfo &s = kSlots[slot];
  if (s.platform != currentPlatform) return false;
  if (s.platform == PLATFORM_APPLE2 && s.a2Model >= 0) {
    if (s.a2Model != (AppleIIe ? 1 : 0)) return false;   // only the system ROM this machine reads
  }
  return true;
}

// Record `rec` as a path in `out`, or nullptr when it is empty / not a plausible path (fresh or
// never-written storage reads as 0x00 or 0xFF).
static const char *recRead(int rec, char *out)
{
  const int a = rec * ROMSEL_SLOT_LEN;
  const int len = romSelRead(a);
  if (len <= 0 || len >= ROMSEL_SLOT_LEN) return nullptr;
  for (int i = 0; i < len; i++) out[i] = (char)romSelRead(a + 1 + i);
  out[len] = 0;
  if (out[0] != '/' || strlen(out) != (size_t)len) return nullptr;
  return out;
}

static bool recWrite(int rec, const char *path)
{
  const size_t len = path ? strlen(path) : 0;
  if (len >= ROMSEL_SLOT_LEN) return false;
  uint8_t r[ROMSEL_SLOT_LEN];
  r[0] = (uint8_t)len;
  memcpy(r + 1, path ? path : "", len);
  return romSelWrite(rec * ROMSEL_SLOT_LEN, r, 1 + (int)len);
}

// The saved path for `slot`, "" when it is set to no ROM, or nullptr when there is no pick.
// Returns a static buffer: copy it before the next call.
const char *romOverride(int slot)
{
  static char path[ROMSEL_SLOT_LEN];
  if (slot < 0 || slot >= ROMSEL_COUNT) return nullptr;
  const int a = slot * ROMSEL_SLOT_LEN;
  if (romSelRead(a) == 1 && romSelRead(a + 1) == (uint8_t)ROMSEL_NONE[0]) return "";
  return recRead(slot, path);
}

// --- base folders (general settings, opened with Ctrl-F1 from the system menu) ---
// ROM base: where the default ROM names are looked for ("/roms" unless changed). Apps base: the
// folder every system's file browser starts in ("/" unless changed). Both without a trailing slash
// except the root itself.
const char *romBaseDir()
{
  static char d[ROMSEL_SLOT_LEN];
  return recRead(ROMSEL_BASE_ROMS, d) ? d : "/roms";
}

const char *appsBaseDir()
{
  static char d[ROMSEL_SLOT_LEN];
  return recRead(ROMSEL_BASE_APPS, d) ? d : "/";
}

static bool setBaseDir(int rec, const char *dir, const char *def, const char *what)
{
  if (dir && strcmp(dir, def) == 0) dir = nullptr;      // the default is stored as "nothing"
  if (!recWrite(rec, dir)) return false;
  sprintf(buf, "ROMSEL: %s -> %s", what, dir ? dir : def);
  printLog(buf);
  return true;
}
bool setRomBaseDir(const char *dir)  { return setBaseDir(ROMSEL_BASE_ROMS, dir, "/roms", "ROM base"); }
bool setAppsBaseDir(const char *dir) { return setBaseDir(ROMSEL_BASE_APPS, dir, "/", "apps base"); }

// The default file for `slot`: its upstream name inside the ROM base folder. Static buffer.
const char *romDefaultPath(int slot)
{
  static char path[2 * ROMSEL_SLOT_LEN];
  const char *name = strrchr(kSlots[slot].defPath, '/');
  const char *base = romBaseDir();
  snprintf(path, sizeof(path), "%s%s", strcmp(base, "/") == 0 ? "" : base, name);
  return path;
}

// The file to load for `slot`: the user's pick if there is one, else the default name.
const char *romPath(int slot)
{
  const char *o = romOverride(slot);
  return o ? o : romDefaultPath(slot);
}

// Save (or, with nullptr / "", clear back to the default; ROMSEL_NONE for no ROM) and commit now, so the pick survives a
// power cut before the menu is closed.
bool romSetOverride(int slot, const char *path)
{
  if (slot < 0 || slot >= ROMSEL_COUNT) return false;
  if (!recWrite(slot, path)) return false;
  sprintf(buf, "ROMSEL: %s %s -> %s", kSlots[slot].label, romDefaultPath(slot), !(path && *path) ? "(default)" : strcmp(path, ROMSEL_NONE) == 0 ? "(none)" : path);
  printLog(buf);
  return true;
}
