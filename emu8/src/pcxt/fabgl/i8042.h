// i8042.h - minimal 8042 keyboard controller for the emu8 PC-XT port.
//
// FabGL's i8042 wrapped a real PS/2 hardware stack (PS2Controller + Keyboard +
// Mouse + FreeRTOS mutex). We have a USB HID keyboard instead, and the BIOS reads
// scancodes through the standard port 0x60 path (processScancode() takes the code
// from AL, which the BIOS ROM IN-s from port 0x60). So all we need is:
//   - an output FIFO that read(0)/port 0x60 pops
//   - injectScancode() that pushes an XT (set-1) scancode and raises IRQ1
//   - enough command handling (0x64) for the BIOS self-test / enable
// Keyboard/Mouse/PS2Controller are tiny stubs so the vendored bios.cpp compiles
// (it only uses them for LEDs / typematic / mouse, all no-ops here).

#pragma once

#include "fabgl.h"

namespace fabgl {

// --- stubs so bios.h's `using fabgl::...` and bios.cpp calls compile ---

class PS2Controller { };

class Keyboard {
public:
  void getLEDs(bool * numLock, bool * capsLock, bool * scrollLock) {
    *numLock = m_numLock; *capsLock = m_capsLock; *scrollLock = m_scrollLock;
  }
  void setLEDs(bool numLock, bool capsLock, bool scrollLock) {
    m_numLock = numLock; m_capsLock = capsLock; m_scrollLock = scrollLock;
  }
  bool sendCommand(uint8_t /*cmd*/, uint8_t /*expectedReply*/) { return true; }
private:
  bool m_numLock = false, m_capsLock = false, m_scrollLock = false;
};

class Mouse {
public:
  bool isMouseAvailable() { return false; }
  void setSampleRate(int) { }
  void setResolution(int) { }
  void setScaling(int)    { }
  int  deviceID()         { return 0; }
  int  getPacketSize()    { return 3; }
};


class i8042 {

public:

  typedef bool (*InterruptCallback)(void * context);

  void init()  { reset(); }

  void reset() {
    m_outHead = m_outTail = 0;
    m_repHead = m_repTail = 0;
    m_latch = 0;
    m_irqPending = false;
    m_commandByte = 0x45;     // keyboard enabled, IRQ1 enabled (typical post-init)
    m_status = 0x14;          // system flag + keyboard not inhibited
    m_pendingCmd = 0;
    m_kbdEnabled = true;
  }

  void setCallbacks(void * context, InterruptCallback keyboardInterrupt, InterruptCallback mouseInterrupt,
                    InterruptCallback reset, InterruptCallback sysReq) {
    m_context           = context;
    m_keyboardInterrupt = keyboardInterrupt;
    m_mouseInterrupt    = mouseInterrupt;
    m_reset             = reset;
    m_sysReq            = sysReq;
  }

  // Called periodically on the CPU core. Like the real chip, one byte at a time sits in the
  // output latch (status bit0 = OBF); IRQ1 is re-signalled until the PIC accepts it. The old
  // one-IRQ-per-injected-byte scheme lost the IRQ whenever a key arrived while INT 9 was still
  // in service, leaving that byte stranded in the FIFO: every later IRQ then popped an older
  // byte and typing lagged further behind each time it happened.
  void tick() { service(); }

  // Push an XT (set-1) scancode from the host keyboard. May run on the other core: it only
  // writes the slot and then m_outTail (single producer); everything else is CPU-core side.
  void injectScancode(uint8_t code) {
    int tail = m_outTail;
    int next = (tail + 1) & (QSIZE - 1);
    if (next == m_outHead) return;   // drop if full
    m_outBuf[tail] = code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    m_outTail = next;
  }

  bool hasOutput() const { return (m_status & 0x01) || m_repHead != m_repTail || m_outHead != m_outTail; }

  // address 0 -> port 0x60 (data), address 1 -> port 0x64 (status/command)
  uint8_t read(int address) {
    if (address == 0) {
      m_status &= ~0x01;            // latch consumed; next byte is loaded on a later tick
      m_irqPending = false;
      return m_latch;
    } else {
      if (!(m_status & 0x01)) service();   // pollers (POST self-test) see replies immediately
      return m_status;
    }
  }

  void write(int address, uint8_t value) {
    if (address == 0) {
      // data byte: parameter for a pending command, or a keyboard command (ACK)
      if (m_pendingCmd == 0x60) {           // write command byte
        m_commandByte = value;
        m_kbdEnabled = !(value & 0x10);     // bit4 = keyboard disable
      }
      m_pendingCmd = 0;
      // a keyboard command (e.g. 0xED set LEDs, 0xF4 enable) -> ACK
      pushReply(0xFA);
    } else {
      // command byte to 0x64
      switch (value) {
        case 0x20: pushReply(m_commandByte); break;  // read command byte
        case 0x60: m_pendingCmd = 0x60;      break;  // write command byte (param next)
        case 0xAA: pushReply(0x55);          break;  // self test passed
        case 0xAB: pushReply(0x00);          break;  // keyboard interface test ok
        case 0xAD: m_kbdEnabled = false;     break;  // disable keyboard
        case 0xAE: m_kbdEnabled = true;      break;  // enable keyboard
        case 0xD4: m_pendingCmd = 0xD4;      break;  // write to mouse (ignored)
        default: break;
      }
    }
  }

  Keyboard * keyboard() { return &m_keyboard; }
  Mouse * mouse()       { return &m_mouse; }
  void enableMouse(bool) { }

  // save state (fabgl.h PcSnap): output queue (pending scancodes), status/command, keyboard LEDs
  // (layout unchanged from the pre-latch format; the latch and reply queue are not saved, so a
  //  load just drops any byte that was in flight)
  void snapshot(PcSnap & s) {
    s.bytes(m_outBuf, sizeof(m_outBuf));
    int head = m_outHead, tail = m_outTail;
    s.i32(head); s.i32(tail);
    s.val(m_status); s.val(m_commandByte); s.val(m_pendingCmd); s.flag(m_kbdEnabled);
    if (s.loading) {
      m_outHead = head & (QSIZE - 1); m_outTail = tail & (QSIZE - 1);
      m_repHead = m_repTail = 0;
      m_status &= ~0x01; m_irqPending = false;
    }
    bool num, caps, scroll;
    m_keyboard.getLEDs(&num, &caps, &scroll);
    s.flag(num); s.flag(caps); s.flag(scroll);
    m_keyboard.setLEDs(num, caps, scroll);
  }

private:

  // controller/keyboard replies (CPU core only), delivered ahead of host scancodes
  void pushReply(uint8_t v) {
    int next = (m_repTail + 1) & (RSIZE - 1);
    if (next != m_repHead) { m_repBuf[m_repTail] = v; m_repTail = next; }
    service();
  }

  // CPU core: refill an empty latch (replies first, then host scancodes while the keyboard is
  // enabled) and keep IRQ1 asserted until the PIC takes it
  void service() {
    if (!(m_status & 0x01)) {
      if (m_repHead != m_repTail) {
        m_latch = m_repBuf[m_repHead];
        m_repHead = (m_repHead + 1) & (RSIZE - 1);
        m_status |= 0x01; m_irqPending = true;
      } else if (m_kbdEnabled && m_outHead != m_outTail) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        m_latch = m_outBuf[m_outHead];
        m_outHead = (m_outHead + 1) & (QSIZE - 1);
        m_status |= 0x01; m_irqPending = true;
      }
    }
    if (m_irqPending && m_keyboardInterrupt && m_keyboardInterrupt(m_context))
      m_irqPending = false;
  }

  static const int QSIZE = 32;
  static const int RSIZE = 8;
  uint8_t m_outBuf[QSIZE];
  volatile int m_outHead = 0, m_outTail = 0;   // host FIFO: producer core 0, consumer CPU core
  uint8_t m_repBuf[RSIZE];
  int     m_repHead = 0, m_repTail = 0;
  uint8_t m_latch = 0;
  bool    m_irqPending = false;
  uint8_t m_status = 0x14;
  uint8_t m_commandByte = 0x45;
  uint8_t m_pendingCmd = 0;
  bool    m_kbdEnabled = true;

  Keyboard m_keyboard;
  Mouse    m_mouse;

  void *            m_context = nullptr;
  InterruptCallback m_keyboardInterrupt = nullptr;
  InterruptCallback m_mouseInterrupt    = nullptr;
  InterruptCallback m_reset             = nullptr;
  InterruptCallback m_sysReq            = nullptr;

};

} // namespace fabgl
