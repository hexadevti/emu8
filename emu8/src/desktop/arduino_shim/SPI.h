// SPI.h — desktop shim. No real SPI bus; the SD card and touch are host-backed (sd_host.cpp,
// mouse-driven touch), so every method is a no-op. `SPIClass hspi` is defined in sd_host.cpp.
#pragma once

#include <cstdint>

#define FSPI 0
#define HSPI 2
#define VSPI 3

#define LSBFIRST  0
#define MSBFIRST  1
#define SPI_MODE0 0
#define SPI_MODE1 1
#define SPI_MODE2 2
#define SPI_MODE3 3

class SPISettings {
public:
  SPISettings() {}
  SPISettings(uint32_t, uint8_t, uint8_t) {}
};

class SPIClass {
public:
  SPIClass(uint8_t = 0) {}
  void     begin(int = -1, int = -1, int = -1, int = -1) {}
  void     end() {}
  void     beginTransaction(SPISettings) {}
  void     endTransaction() {}
  uint8_t  transfer(uint8_t) { return 0; }
  uint16_t transfer16(uint16_t) { return 0; }
  void     setFrequency(uint32_t) {}
  void     setDataMode(uint8_t) {}
  void     setBitOrder(uint8_t) {}
};
