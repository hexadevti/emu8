// MC146818.h - minimal RTC/CMOS for the emu8 PC-XT port.
//
// FabGL's MC146818 persisted CMOS in ESP-IDF NVS and drove a periodic interrupt
// via esp_timer. We only need the register file + the 0x70/0x71 index/data port
// pair + updateTime() (the BIOS reads sec/min/hour to seed the tick counter).
// CMOS persistence and the periodic RTC interrupt (INT 70h) are not needed to
// boot DOS, so they are omitted; time starts at zero.

#pragma once

#include "fabgl.h"

namespace fabgl {

class MC146818 {

public:

  typedef bool (*InterruptCallback)(void * context);

  void init(char const * /*nvsNamespace*/) { reset(); }

  void setCallbacks(void * context, InterruptCallback interruptCallback) {
    m_context = context;
    m_interruptCallback = interruptCallback;
  }

  void reset() {
    memset(m_regs, 0, sizeof(m_regs));
    m_index = 0;
    m_regs[0x0B] = 0x02;   // status B: 24-hour mode, BCD
    m_regs[0x0D] = 0x80;   // status D: valid RAM/time
  }

  void commit() { }

  // address 0 -> port 0x70 (index), address 1 -> port 0x71 (data)
  uint8_t read(int address) {
    if (address == 0)
      return m_index;
    return m_regs[m_index & 0x3F];
  }

  void write(int address, uint8_t value) {
    if (address == 0)
      m_index = value & 0x7F;   // bit7 = NMI mask (ignored)
    else
      m_regs[m_index & 0x3F] = value;
  }

  uint8_t & reg(int address) { return m_regs[address & 0x3F]; }

  // BIOS reads regs 0x00/0x02/0x04 (sec/min/hour, BCD) to seed BIOS_SYSTICKS.
  void updateTime() {
    m_regs[0x00] = 0; // seconds
    m_regs[0x02] = 0; // minutes
    m_regs[0x04] = 0; // hours
  }

private:

  uint8_t m_regs[64];
  uint8_t m_index = 0;
  void * m_context = nullptr;
  InterruptCallback m_interruptCallback = nullptr;

};

} // namespace fabgl
