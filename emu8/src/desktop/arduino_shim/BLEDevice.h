// BLEDevice.h — desktop shim. BLE is declared-but-unused in emu.h (lines ~35-44); provide just the
// types those static globals need so emu.h compiles. No Bluetooth on desktop.
#pragma once

#include <cstdint>

class BLEUUID {
public:
  BLEUUID() {}
  BLEUUID(uint16_t) {}
  BLEUUID(const char *) {}
};
class BLERemoteCharacteristic {};
class BLEAdvertisedDevice {};
class BLEClient {};
class BLEScan {};
class BLEDevice {
public:
  static void init(const char *) {}
};
