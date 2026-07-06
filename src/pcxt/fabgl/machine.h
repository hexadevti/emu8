// machine.h - PC-XT machine glue for the emu8 port.
//
// Trimmed re-implementation of FabGL's PCEmulator Machine: keeps the 8086 + PIC
// (x2) + PIT + i8042 + MC146818 + CGA wiring and the magic-interrupt BIOS bridge,
// drops Hercules / MCP23S17 / serial UARTs / SoundGenerator / FreeRTOS task.
//
// Memory buffers (1MB RAM + 64KB video RAM) are INJECTED by the caller
// (setMemoryBuffers) so this translation unit stays free of Arduino/ESP heap APIs
// and links into both the device build and a desktop host harness.

#pragma once

#include <stdio.h>
#include <stdint.h>

#include "graphicsadapter.h"
#include "i8086.h"
#include "PIC8259.h"
#include "PIT8253.h"
#include "i8042.h"
#include "MC146818.h"
#include "bios.h"     // defines DISKCOUNT and class BIOS

using fabgl::GraphicsAdapter;
using fabgl::PIC8259;
using fabgl::PIT8253;
using fabgl::i8042;
using fabgl::MC146818;
using fabgl::i8086;

#define PCXT_RAM_SIZE      1048576    // 1 MB, must match BIOS MEMSIZE
#define PCXT_VIDEOMEM_SIZE 65536      // 64 KB video window (0xB0000..0xBFFFF)


class Machine {

public:
  Machine();

  // inject pre-allocated buffers (RAM >= 1MB, videoRam >= 64KB) before init()
  void setMemoryBuffers(uint8_t * ram, uint8_t * videoRam);

  void setBaseDirectory(char const * value) { m_baseDir = value; }

  void setDriveImage(int drive, char const * filename, int cylinders = 0, int heads = 0, int sectors = 0);

  bool diskChanged(int drive)      { return m_diskChanged[drive]; }
  void resetDiskChanged(int drive) { m_diskChanged[drive] = false; }

  void setBootDrive(int drive)     { m_bootDrive = drive; }

  void init();
  void reset();

  // step the CPU and tick the chipset up to `iterations` times (device loop)
  void run(int iterations);

  void trigReset() { m_reset = true; }

  // host keyboard -> 8042 (XT set-1 scancode), raises IRQ1
  void injectScancode(uint8_t code) { m_i8042.injectScancode(code); }

  // optional HSPI bus lock around disk I/O (set by the device glue to busTake/busGive;
  // null on host). The whole INT 13h handler runs inside lock()/unlock().
  typedef void (*LockFn)();
  static void setDiskLock(LockFn lock, LockFn unlock) { s_diskLock = lock; s_diskUnlock = unlock; }

  // PC-speaker: the device glue registers a callback invoked whenever the PIT ch2 frequency or the
  // port-0x61 gate/data bits change, so the audio ISR can track it with no latency.
  typedef void (*SpeakerFn)(int freq, bool on);
  static void setSpeakerCallback(SpeakerFn f) { s_speakerCb = f; }

  // INT 33h (mouse) is serviced directly by the device glue from the USB mouse (no MOUSE.COM / PS2
  // hardware). The handler reads/writes the 8086 registers and returns true if it handled the call.
  typedef bool (*Int33Fn)();
  static void setInt33Handler(Int33Fn f) { s_int33 = f; }

  // Called once per instruction (between instructions, before step()). The device glue uses it to
  // inject the INT 33h mouse event-handler far-call into the running program (QBASIC uses callbacks).
  typedef void (*StepHook)();
  static void setStepHook(StepHook f) { s_stepHook = f; }

  // Disk backend (set by the device glue: SD/File on device, stdio on the host harness).
  // The stdio FILE* path was unreliable on the ESP32 SD VFS (fseek/ftell returned 0),
  // so disk images go through these hooks instead. ctx is an opaque per-drive handle.
  typedef void * (*DiskOpenFn)(const char * path, uint64_t * sizeOut);          // null if can't open
  typedef int    (*DiskIoFn)(void * ctx, uint64_t pos, uint8_t * buf, uint32_t count, bool write); // bytes done
  typedef void   (*DiskCloseFn)(void * ctx);
  static void setDiskBackend(DiskOpenFn o, DiskIoFn io, DiskCloseFn c) { s_diskOpen = o; s_diskIo = io; s_diskClose = c; }

  // used by the BIOS INT 13h handler (bios.cpp) instead of fseek/fread/fwrite
  int diskRead(int drive, uint64_t pos, uint8_t * dest, uint32_t count) {
    return s_diskIo ? s_diskIo(m_diskCtx[drive], pos, dest, count, false) : 0;
  }
  int diskWrite(int drive, uint64_t pos, const uint8_t * src, uint32_t count) {
    return s_diskIo ? s_diskIo(m_diskCtx[drive], pos, (uint8_t *)src, count, true) : 0;
  }

  uint32_t ticksCounter() { return m_ticksCounter; }

  i8042 *    getI8042()        { return &m_i8042; }
  MC146818 * getMC146818()     { return &m_MC146818; }
  uint8_t *  memory()          { return s_memory; }
  uint8_t *  videoMemory()     { return s_videoMemory; }
  uint8_t *  frameBuffer()     { return m_frameBuffer; }
  GraphicsAdapter * graphicsAdapter() { return &m_graphicsAdapter; }

  void *       disk(int index)         { return m_diskCtx[index]; }   // truthy = mounted
  char const * diskFilename(int index) { return m_diskFilename[index]; }
  uint64_t     diskSize(int index)     { return m_diskSize[index]; }
  uint16_t     diskCylinders(int index){ return m_diskCylinders[index]; }
  uint8_t      diskHeads(int index)    { return m_diskHeads[index]; }
  uint8_t      diskSectors(int index)  { return m_diskSectors[index]; }

private:

  void tick();

  void setCGAMode();
  void setCGA6845Register(uint8_t value);

  static void    writePort(void * context, int address, uint8_t value);
  static uint8_t readPort(void * context, int address);

  static void    writeVideoMemory8(void * context, int address, uint8_t value);
  static void    writeVideoMemory16(void * context, int address, uint16_t value);
  static uint8_t readVideoMemory8(void * context, int address);
  static uint16_t readVideoMemory16(void * context, int address);

  static bool interrupt(void * context, int num);

  static void PITChangeOut(void * context, int timerIndex);
  static bool keyboardInterrupt(void * context);
  static bool mouseInterrupt(void * context);
  static bool MC146818Interrupt(void * context);
  static bool resetMachine(void * context);
  static bool sysReq(void * context);

  void speakerSetFreq();
  void speakerEnableDisable();

  void autoDetectDriveGeometry(int drive);


  bool             m_reset = false;

  GraphicsAdapter  m_graphicsAdapter;
  BIOS             m_BIOS;

  char *    m_diskFilename[DISKCOUNT] = {};
  bool      m_diskChanged[DISKCOUNT]  = {};
  void *    m_diskCtx[DISKCOUNT]      = {};   // opaque handle from the disk backend
  uint64_t  m_diskSize[DISKCOUNT]     = {};
  uint16_t  m_diskCylinders[DISKCOUNT]= {};
  uint8_t   m_diskHeads[DISKCOUNT]    = {};
  uint8_t   m_diskSectors[DISKCOUNT]  = {};

  static uint8_t * s_memory;
  static uint8_t * s_videoMemory;
  uint8_t *        m_frameBuffer = nullptr;

  static LockFn      s_diskLock;
  static LockFn      s_diskUnlock;
  static DiskOpenFn  s_diskOpen;
  static DiskIoFn    s_diskIo;
  static DiskCloseFn s_diskClose;
  static SpeakerFn   s_speakerCb;
  static Int33Fn     s_int33;
  static StepHook    s_stepHook;

  PIC8259   m_PIC8259A;   // master
  PIC8259   m_PIC8259B;   // slave
  PIT8253   m_PIT8253;
  i8042     m_i8042;
  MC146818  m_MC146818;

  uint32_t  m_ticksCounter = 0;

  // CGA
  uint8_t   m_CGA6845SelectRegister = 0;
  uint8_t   m_CGA6845[18]           = {};
  uint16_t  m_CGAMemoryOffset       = 0;
  uint8_t   m_CGAModeReg            = 0;
  uint8_t   m_CGAColorReg           = 0;
  uint16_t  m_CGAVSyncQuery         = 0;

  // speaker (PIT ch2)
  bool      m_speakerDataEnable = false;
  int       m_speakerFreq       = 0;
  uint32_t  m_refreshToggle     = 0;

  uint8_t   m_bootDrive = 0;
  char const * m_baseDir = nullptr;

public:
  // exposed for the LCD render task / audio (read-only state)
  uint8_t   cgaModeReg()    const { return m_CGAModeReg; }
  uint8_t   cgaColorReg()   const { return m_CGAColorReg; }
  uint16_t  cgaMemOffset()  const { return m_CGAMemoryOffset; }
  bool      speakerEnabled() { return m_speakerDataEnable && m_PIT8253.getGate(2) && m_speakerFreq > 0; }
  int       speakerFreq()   const { return m_speakerFreq; }

};

// the single machine instance (defined in pcxt_machine.cpp)
extern Machine g_pcxtMachine;
