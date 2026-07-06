// EEPROM.h — desktop shim: a byte array backed by a host file (eeprom.bin). eprom.cpp is reused
// unchanged; only this storage object is swapped. begin()/read()/write()/commit() live in hal.cpp.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

class EEPROMClass {
public:
  bool    begin(size_t size);          // allocate `size` bytes + load eeprom.bin (hal.cpp)
  void    end() {}
  uint8_t read(int addr);              // hal.cpp
  void    write(int addr, uint8_t val);// hal.cpp
  bool    commit();                    // flush to eeprom.bin (hal.cpp)

  // typed helpers the codebase uses
  bool    readBool(int addr) { return read(addr) != 0; }
  size_t  writeBool(int addr, bool v) { write(addr, v ? 1 : 0); return 1; }
  int8_t  readChar(int addr) { return (int8_t)read(addr); }
  size_t  writeChar(int addr, int8_t v) { write(addr, (uint8_t)v); return 1; }
  uint8_t readByte(int addr) { return read(addr); }
  size_t  writeByte(int addr, uint8_t v) { write(addr, v); return 1; }

  template <typename T> T &get(int addr, T &t) { for (size_t i = 0; i < sizeof(T); i++) ((uint8_t*)&t)[i] = read(addr + (int)i); return t; }
  template <typename T> const T &put(int addr, const T &t) { for (size_t i = 0; i < sizeof(T); i++) write(addr + (int)i, ((const uint8_t*)&t)[i]); return t; }

  uint8_t &operator[](int addr);       // hal.cpp (raw byte access)
  size_t   length();                   // hal.cpp
};

extern EEPROMClass EEPROM;
