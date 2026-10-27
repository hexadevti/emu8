// pico_shim.cpp - out-of-line half of the ESP-IDF compatibility layer (see pico_shim/pico_shim.h).
//
// Only the pieces that must be real symbols rather than inline code live here: ps_malloc, the
// heap_caps_* family, and esp_reset_reason(). Guarded so arduino-cli, which compiles every .cpp
// under src/ for every target, produces an empty object file on the other boards.
#include "../../board.h"

#if defined(BOARD_PICOCALC)

#include "pico_shim/pico_shim.h"
#include <tusb.h>
#include <hardware/flash.h>
#if defined(PICO_RP2350)
#include <hardware/structs/qmi.h>
#endif

extern "C" {

// --- allocation -------------------------------------------------------------------------------
// One flat SRAM heap, so the "which memory?" argument only decides whether we refuse. A request
// that names SPIRAM without accepting internal RAM as a fallback CANNOT be served on this board
// (BOARD_HAS_PSRAM 0 -- the PicoCalc's 8MB PSRAM hangs off plain GPIOs and is not memory-mapped),
// and every such call site checks the result for null, so returning null is the honest answer.
static inline bool wantsUnavailablePsram(uint32_t caps) {
  return (caps & MALLOC_CAP_SPIRAM) && !(caps & MALLOC_CAP_INTERNAL) && !BOARD_HAS_PSRAM;
}

void *ps_malloc(size_t n)                 { return malloc(n); }
void *ps_calloc(size_t n, size_t size)    { return calloc(n, size); }
void *ps_realloc(void *p, size_t n)       { return realloc(p, n); }

void *heap_caps_malloc(size_t n, uint32_t caps) {
  return wantsUnavailablePsram(caps) ? nullptr : malloc(n);
}
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
  return wantsUnavailablePsram(caps) ? nullptr : calloc(n, size);
}
void *heap_caps_realloc(void *p, size_t n, uint32_t caps) {
  return wantsUnavailablePsram(caps) ? nullptr : realloc(p, n);
}
void heap_caps_free(void *p) { free(p); }

size_t heap_caps_get_free_size(uint32_t caps) {
  return wantsUnavailablePsram(caps) ? 0 : rp2040.getFreeHeap();
}

// The pico-sdk heap exposes no largest-contiguous-block query. The callers use this only to log a
// fragmentation figure, so report the total free size; it is an upper bound, never a false
// negative, and no allocation decision is made from it.
size_t heap_caps_get_largest_free_block(uint32_t caps) {
  return heap_caps_get_free_size(caps);
}

// --- reset reason -----------------------------------------------------------------------------
// The one consumer (videoSetup) asks a single question: "did WE reboot the board, or did the user
// power-cycle / press RST?" -- a hardware reset re-shows the SELECT SYSTEM splash, a software one
// boots straight into the chosen platform.
//
// ESP.restart() reboots the RP2350 through the watchdog, so both SOFT_RESET and WDT_RESET mean
// "we did it". Treating WDT_RESET as software is safe here precisely because this firmware never
// arms a hardware watchdog (see the no-op disableCore0WDT/... in pico_shim.h) -- nothing else can
// produce it. NOTE: arduino-pico's own source marks getResetReason() as untested on RP2350; if the
// splash logic misbehaves, this function is the single place to correct it.
esp_reset_reason_t esp_reset_reason(void) {
  switch (rp2040.getResetReason()) {
    case RP2040::SOFT_RESET:  return ESP_RST_SW;
    case RP2040::WDT_RESET:   return ESP_RST_SW;
    case RP2040::PWRON_RESET: return ESP_RST_POWERON;
    case RP2040::RUN_PIN_RESET: return ESP_RST_EXT;
    default:                  return ESP_RST_UNKNOWN;
  }
}

}   // extern "C"

// The one instance of the ESP-compatibility forwarder declared in pico_shim.h. It holds no
// state -- every method forwards straight to arduino-pico's rp2040 object.
PicoEspClass ESP;

// A watchdog reboot drops the USB pull-up for only a few microseconds, too short for the host to
// see the device leave. Windows then talks to the rebooted board as if it were the old one, and
// the port ends up "Device Descriptor Request Failed" (code 43), with no COM port until the hub
// is reset. So detach first, and stay detached long enough for the hub to register the unplug.
void PicoEspClass::restart()
{
  Serial.flush();
  tud_disconnect();
  delay(300);
  rp2040.restart();
}

// --- flash window (see "The flash this firmware can actually reach" in pico_shim.h) ----------
extern "C" uint8_t _EEPROM_start;
extern "C" uint8_t _FS_start;
extern "C" uint8_t __flash_binary_end;
void printLog(const char *txt);

static const uint32_t kLoaderReserve = 0x41000;   // loader area at the top of flash, see flashMap()

struct PicoFlashMap {
  uintptr_t shift;      // linker end-of-flash minus the visible end: what every area moves down
  uint32_t  physBase;   // physical offset of XIP_BASE
  uint32_t  visible;    // bytes of flash visible from XIP_BASE
  bool      ok;
};

static const PicoFlashMap &flashMap()
{
  static PicoFlashMap m;
  static bool done = false;
  if (done) return m;
  done = true;
  const uintptr_t linkTop = (uintptr_t)&_EEPROM_start + FLASH_SECTOR_SIZE;
  m.visible  = (uint32_t)(linkTop - XIP_BASE);
  m.physBase = 0;
#if defined(PICO_RP2350)
  // QMI ATRANS0 maps the first 4MB of the XIP window: BASE is the physical start and SIZE the
  // length, both in 4K units. Reset value (no loader, whole chip): base 0, size 4MB. Under the
  // UF2 Loader the boot ROM sets them to the app partition.
  const uint32_t at   = qmi_hw->atrans[0];
  const uint32_t size = ((at & QMI_ATRANS0_SIZE_BITS) >> QMI_ATRANS0_SIZE_LSB) * FLASH_SECTOR_SIZE;
  m.physBase          = ((at & QMI_ATRANS0_BASE_BITS) >> QMI_ATRANS0_BASE_LSB) * FLASH_SECTOR_SIZE;
  if (size < m.visible) m.visible = size;
#endif
  // Keep the last 256K of the chip, and the 4K below it, free for a loader whatever runs us:
  // PicoMultiLoader lives there on the RP2040 (0x1C0000, its app record at 0x1BF000) and keeps
  // its Wi-Fi files there on the RP2350 (docs/apps.md in PicoMultiLoader). Without a loader this
  // only costs the MSX/SMS cartridge window 260K.
  const uint32_t chipTop = (uint32_t)(linkTop - XIP_BASE) - kLoaderReserve;
  if (m.physBase + m.visible > chipTop) m.visible = chipTop - m.physBase;
  m.shift = linkTop - (XIP_BASE + m.visible);
  // Lowest flash any settings area uses: the ROM-in-flash BIOS + disk-ROM windows (48K) under the
  // filesystem partition. Everything from there up must lie above the firmware image.
  const uintptr_t lowest  = (uintptr_t)&_FS_start - m.shift - 0xC000;
  const uintptr_t fwEnd   = ((uintptr_t)&__flash_binary_end + FLASH_SECTOR_SIZE - 1) & ~(uintptr_t)(FLASH_SECTOR_SIZE - 1);
  m.ok = m.visible > 0 && (uintptr_t)&_FS_start - XIP_BASE > m.shift && lowest >= fwEnd;
  return m;
}

uintptr_t picoFlashRelocate(const void *linkAddr) { return (uintptr_t)linkAddr - flashMap().shift; }
uint32_t  picoFlashOffset(uintptr_t xipAddr)      { return flashMap().physBase + (uint32_t)(xipAddr - XIP_BASE); }
bool      picoFlashMapOk()                        { return flashMap().ok; }

void picoFlashLogMap()
{
  const PicoFlashMap &m = flashMap();
  char line[160];
  snprintf(line, sizeof(line), "FLASH: %uK visible at physical 0x%06x, settings at 0x%08x%s",
           (unsigned)(m.visible / 1024), (unsigned)m.physBase,
           (unsigned)picoFlashRelocate(&_EEPROM_start),
           m.ok ? (m.physBase ? " (inside the loader's app partition)" : "")
                : " -- window too small, settings kept in RAM only");
  printLog(line);
}

// The EEPROM object every caller actually reaches, via the `#define EEPROM picoEEPROM` in
// pico_shim.h. The library's own EEPROMClass global is left untouched and unused.
#undef EEPROM
PicoEEPROMClass picoEEPROM;

void PicoEEPROMClass::begin(size_t size)
{
  if (picoFlashMapOk()) {
    _sector = (uint8_t *)picoFlashRelocate(&_EEPROM_start);
    EEPROMClass::begin(size);
    return;
  }
  // Nothing readable to load from: start erased (a "New Device" boot) and never commit.
  _sector = nullptr;
  size = (size <= 0 || size > 4096) ? 4096 : ((size + 255) & ~(size_t)255);
  if (!_data) _data = new uint8_t[size];
  _size = size;
  memset(_data, 0xFF, _size);
  _dirty = false;
}

// arduino-pico's EEPROMClass::commit(), with the physical offset of the relocated sector.
bool PicoEEPROMClass::commit()
{
  if (!_size || !_data || !_sector) return false;
  if (!_dirty) return true;
  const uint32_t at = picoFlashOffset((uintptr_t)_sector);
#ifndef __FREERTOS
  noInterrupts();
#endif
  rp2040.idleOtherCore();
  flash_range_erase(at, FLASH_SECTOR_SIZE);
  flash_range_program(at, _data, _size);
  rp2040.resumeOtherCore();
#ifndef __FREERTOS
  interrupts();
#endif
  _dirty = false;
  return true;
}

// Bring-up checkpoints (see pico_shim.h). Written by the render task on core 1, read by the
// 6502 meter on core 0.

// --- fast XIP after a flash write (RP2350) ----------------------------------------------------
// Every flash_range_erase/program ends by re-entering XIP. The RP2040 does that by re-running
// boot2, which puts the fast quad read mode back. The RP2350 build has no boot2 (rpipico2
// build.boot2=none), so the SDK falls back to the bootrom's flash_enter_cmd_xip(): serial 03h
// reads at a conservative divider. It saves and restores QMI CS1 (PSRAM) over the call, but not
// CS0, the flash all the code runs from -- so after the first write every flash fetch was several
// times slower until the next reboot. That was "every system is slow the FIRST time it runs, fine
// after a restart": the first run copies its BIOS/cart into flash (romflash_picocalc.cpp), later
// boots find it already there and write nothing. Settings saves (EEPROM.commit) did it too.
//
// The RP2350 builds link with -Wl,--wrap=flash_range_erase,--wrap=flash_range_program
// (.vscode/tasks.json, tools/release.ps1), so every caller -- ours and the EEPROM library's --
// lands here. These run from SRAM, inside the window where the caller has the other core parked,
// and put CS0's read setup back exactly as it was before the write. Without the link flags these
// are simply unused; on the RP2040 they would only pass through.
#include <hardware/flash.h>
#if !PICO_RP2040
#include <hardware/structs/qmi.h>
#endif

extern "C" {
void __real_flash_range_erase(uint32_t flash_offs, size_t count);
void __real_flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count);

volatile uint32_t picocalcXipRestores = 0;   // writes that left CS0 slow and were put right

void __no_inline_not_in_flash_func(__wrap_flash_range_erase)(uint32_t flash_offs, size_t count) {
#if !PICO_RP2040
  const uint32_t timing = qmi_hw->m[0].timing, rcmd = qmi_hw->m[0].rcmd, rfmt = qmi_hw->m[0].rfmt;
  __real_flash_range_erase(flash_offs, count);
  if (qmi_hw->m[0].timing != timing || qmi_hw->m[0].rcmd != rcmd || qmi_hw->m[0].rfmt != rfmt) {
    qmi_hw->m[0].timing = timing; qmi_hw->m[0].rcmd = rcmd; qmi_hw->m[0].rfmt = rfmt;
    picocalcXipRestores++;
  }
#else
  __real_flash_range_erase(flash_offs, count);
#endif
}

void __no_inline_not_in_flash_func(__wrap_flash_range_program)(uint32_t flash_offs, const uint8_t *data, size_t count) {
#if !PICO_RP2040
  const uint32_t timing = qmi_hw->m[0].timing, rcmd = qmi_hw->m[0].rcmd, rfmt = qmi_hw->m[0].rfmt;
  __real_flash_range_program(flash_offs, data, count);
  if (qmi_hw->m[0].timing != timing || qmi_hw->m[0].rcmd != rcmd || qmi_hw->m[0].rfmt != rfmt) {
    qmi_hw->m[0].timing = timing; qmi_hw->m[0].rcmd = rcmd; qmi_hw->m[0].rfmt = rfmt;
    picocalcXipRestores++;
  }
#else
  __real_flash_range_program(flash_offs, data, count);
#endif
}
}   // extern "C"

#endif // BOARD_PICOCALC
