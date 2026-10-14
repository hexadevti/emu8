// SD.h — desktop shim. SDClass is a host-backed fs::FS (the emulated card = the g_sdRoot directory).
// `SDClass SD;` is defined in sd_host.cpp. emu.h does `#define FSTYPE SD`.
#pragma once

#include "FS.h"
#include "SPI.h"

typedef enum {
  CARD_NONE = 0,
  CARD_MMC,
  CARD_SD,
  CARD_SDHC,
  CARD_UNKNOWN,
} sdcard_type_t;

class SDClass : public fs::FS {
public:
  // Mount is a no-op on desktop (FSSetup() in sd_host.cpp sets g_sdRoot). Overloads cover the
  // device call site FSTYPE.begin(SD_CS_PIN, hspi, SD_SPI_HZ) (not compiled on desktop) too.
  bool begin() { return true; }
  bool begin(int /*cs*/) { return true; }
  bool begin(int /*cs*/, SPIClass & /*spi*/, uint32_t /*freq*/ = 4000000) { return true; }
  void end() {}

  uint8_t  cardType() { return CARD_SD; }
  uint64_t cardSize()   { return (uint64_t)2 * 1024 * 1024 * 1024; }   // pretend 2 GB
  uint64_t totalBytes() { return (uint64_t)2 * 1024 * 1024 * 1024; }
  uint64_t usedBytes()  { return 0; }
};

extern SDClass SD;
