// esp_attr.h - PicoCalc shim for the ESP-IDF section attributes.
//
// Kept separate from (and included by) pico_shim.h because the tiny386 core's .c files include
// this header on its own, so it must stay valid C with no other dependencies.
//
//   IRAM_ATTR       ESP-IDF: place this function in internal RAM so it never stalls on a flash
//                   cache miss. pico-sdk's equivalent is the ".time_critical" section, which
//                   memmap_default.ld copies into SRAM at boot exactly like .data.
//   DRAM_ATTR       ESP-IDF: keep this data out of flash. NOT a no-op here: on the RP2040/RP2350
//                   only MUTABLE initialised data (.data) is copied to SRAM at boot -- `const`
//                   objects stay in .rodata, i.e. in FLASH, reached through the XIP cache. A
//                   const lookup table read on every emulated instruction therefore competes for
//                   that cache with the render loop on the other core, so the hot opcode tables
//                   ask for ".time_critical.rodata" -- the linker script globs
//                   *(.time_critical*) into the RAM-resident .data output section, and the
//                   ".rodata" suffix is required because GCC refuses to put read-only data
//                   and executable code in one section ("section type conflict"). This is
//                   the same trick as pico-sdk's own __not_in_flash(group).
//   RTC_NOINIT_ATTR ESP-IDF: RTC-domain data that is NOT zeroed at startup, so it survives a
//                   software reset. pico-sdk's ".uninitialized_data" is the same idea (NOLOAD,
//                   never touched by crt0). videoSetup() uses it to carry the "show the splash"
//                   magic across ESP.restart(); if an RP2350 warm boot turns out to scrub SRAM,
//                   the fallback is one of the four watchdog scratch registers.
#pragma once

#ifndef IRAM_ATTR
#define IRAM_ATTR        __attribute__((section(".time_critical")))
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR        __attribute__((section(".time_critical.rodata")))
#endif
#ifndef RTC_NOINIT_ATTR
#define RTC_NOINIT_ATTR  __attribute__((section(".uninitialized_data")))
#endif
#ifndef RTC_DATA_ATTR
#define RTC_DATA_ATTR    __attribute__((section(".uninitialized_data")))
#endif
