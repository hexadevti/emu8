// romflash_picocalc.cpp - ROM images copied from the SD card into on-board flash, read in place.
//
// The MSX core wants its BIOS (32K) and the whole cartridge image (16K up to 1MB for a MegaROM)
// resident behind a plain pointer, next to 64K of work RAM and 16K of VRAM. On the ESP32 boards
// the images go to PSRAM. The PicoCalc has none, and on its RP2040 mainboard the heap left once
// the MSX has its RAM and VRAM is ~30K: not the BIOS, let alone a cartridge. The SMS is the same
// story with no BIOS: its games run 32K to 512K and are read in place through the cartridge window
// (only one platform is ever booted, so the MSX and SMS share it).
//
// What the Pico does have is 2MB (RP2040) / 4MB (RP2350) of QSPI flash, of which the firmware uses
// under 500K, and the XIP window makes any of it readable through an ordinary const pointer. So
// each image is streamed off the card one 4K sector at a time into a fixed window of that spare
// flash, and the core reads it from there like the ROM it is. The images are read-only to the
// emulated machine (msx_cart.cpp / sms_cart.cpp only ever repoint bank windows), so nothing ever writes to XIP.
//
// Layout, top-down from the arduino-pico flash filesystem partition (the menu's "flash=..._65536"
// size; the EEPROM sector sits above it at the very end of flash). Neither is touched:
//
//     _FS_start - 32K .. _FS_start                  BIOS window, fixed
//     (BIOS base) - cart size .. BIOS base          cartridge window, sized to the image
//     __flash_binary_end (rounded up) ...           must stay below the cartridge
//
// Only sectors whose contents differ are erased and programmed, so booting the same BIOS and the
// same cartridge again (the normal case: both are auto-loaded from EEPROM) is a read-and-compare
// that writes nothing. Flash endurance is therefore spent only when the user picks a new image.
//
// Programming uses the exact sequence arduino-pico's EEPROM.commit() uses on this core, which this
// board already runs every time settings are saved: idleOtherCore() parks the other core in RAM,
// and under FreeRTOS it also masks interrupts and suspends the scheduler on this one, so no code
// can fetch from flash while it is being written. The SDK flushes the XIP cache afterwards.

#include "../../emu.h"

#if BOARD_ROM_IN_FLASH

#include <hardware/flash.h>

extern "C" uint8_t _FS_start;
extern "C" uint8_t __flash_binary_end;

static const uint32_t kSector     = FLASH_SECTOR_SIZE;   // 4096: the erase granule
static const uint32_t kBiosWindow = 0x8000;              // 32K, the largest MSX1 main BIOS

static uintptr_t alignUp(uintptr_t v) { return (v + kSector - 1) & ~(uintptr_t)(kSector - 1); }
static uintptr_t biosBase()           { return (uintptr_t)&_FS_start - kBiosWindow; }
static uintptr_t firmwareEnd()        { return alignUp((uintptr_t)&__flash_binary_end); }

uint32_t romFlashCapacity(RomFlashWindow w)
{
  if (w == ROMFLASH_BIOS) return kBiosWindow;
  uintptr_t top = biosBase(), bottom = firmwareEnd();
  return top > bottom ? (uint32_t)(top - bottom) : 0;
}

const uint8_t *romFlashLoad(RomFlashWindow w, File &f, uint32_t len)
{
  if (len == 0 || len > romFlashCapacity(w)) {
    sprintf(buf, "ROMFLASH: %u bytes do not fit the %s window (%u)", (unsigned)len,
            w == ROMFLASH_BIOS ? "BIOS" : "cartridge", (unsigned)romFlashCapacity(w));
    printLog(buf);
    return nullptr;
  }
  const uintptr_t base = (w == ROMFLASH_BIOS) ? biosBase() : biosBase() - alignUp(len);

  uint8_t *chunk = (uint8_t *)malloc(kSector);
  if (!chunk) { printLog("ROMFLASH: no heap for the 4K staging buffer"); return nullptr; }

  uint32_t written = 0;
  bool ok = true;
  for (uint32_t off = 0; off < len; off += kSector) {
    uint32_t n = (len - off < kSector) ? (len - off) : kSector;
    if ((uint32_t)f.read(chunk, n) != n) { ok = false; break; }
    if (n < kSector) memset(chunk + n, 0xFF, kSector - n);     // erased-flash value past the end

    const uint8_t *dst = (const uint8_t *)(base + off);
    if (memcmp(dst, chunk, kSector) == 0) continue;            // already there: no erase, no wear

#ifndef __FREERTOS
    noInterrupts();
#endif
    rp2040.idleOtherCore();
    flash_range_erase((uint32_t)(base + off - XIP_BASE), kSector);
    flash_range_program((uint32_t)(base + off - XIP_BASE), chunk, kSector);
    rp2040.resumeOtherCore();
#ifndef __FREERTOS
    interrupts();
#endif
    written++;
  }
  free(chunk);

  if (!ok) { printLog("ROMFLASH: SD read came up short"); return nullptr; }
  sprintf(buf, "ROMFLASH: %uK at 0x%08x, %u of %u sectors rewritten", (unsigned)(len / 1024),
          (unsigned)base, (unsigned)written, (unsigned)((len + kSector - 1) / kSector));
  printLog(buf);
  return (const uint8_t *)base;
}

#endif  // BOARD_ROM_IN_FLASH
