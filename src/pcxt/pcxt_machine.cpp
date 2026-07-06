// pcxt_machine.cpp - PC-XT machine glue (8086 + PIC x2 + PIT + i8042 + MC146818 + CGA).
//
// Trimmed port of FabGL's PCEmulator Machine. Memory-agnostic: the 1MB RAM and
// 64KB video RAM are injected via setMemoryBuffers(), so this file has no Arduino
// or ESP heap dependency and links into both the device build and a host harness.
//
// The 8086 core is MIT (i8086.cpp); the surrounding device logic is adapted from
// FabGL (GPLv3). The single global instance is g_pcxtMachine.

#include "fabgl/machine.h"

// CGA mode/color control register bits (from FabGL machine.cpp)
#define CGA_MODECONTROLREG_TEXT80         0x01   // 0 = 40x25, 1 = 80x25
#define CGA_MODECONTROLREG_GRAPHICS       0x02   // 0 = text,  1 = graphics
#define CGA_MODECONTROLREG_COLOR          0x04
#define CGA_MODECONTROLREG_ENABLED        0x08   // 0 = video off, 1 = video on
#define CGA_MODECONTROLREG_GRAPH640       0x10   // 0 = 320x200, 1 = 640x200
#define CGA_MODECONTROLREG_BIT7BLINK      0x20
#define CGA_COLORCONTROLREG_BACKCOLR_MASK 0x0f
#define CGA_COLORCONTROLREG_HIGHINTENSITY 0x10
#define CGA_COLORCONTROLREG_PALETTESEL    0x20

using fabgl::GraphicsAdapter;

// static members
uint8_t * Machine::s_memory      = nullptr;
uint8_t * Machine::s_videoMemory = nullptr;
Machine::LockFn     Machine::s_diskLock   = nullptr;
Machine::LockFn     Machine::s_diskUnlock = nullptr;
Machine::DiskOpenFn Machine::s_diskOpen   = nullptr;
Machine::DiskIoFn   Machine::s_diskIo     = nullptr;
Machine::DiskCloseFn Machine::s_diskClose = nullptr;
Machine::SpeakerFn  Machine::s_speakerCb  = nullptr;
Machine::Int33Fn    Machine::s_int33      = nullptr;
Machine::StepHook   Machine::s_stepHook   = nullptr;

// the single instance
Machine g_pcxtMachine;


Machine::Machine() { }

void Machine::setMemoryBuffers(uint8_t * ram, uint8_t * videoRam)
{
  s_memory      = ram;
  s_videoMemory = videoRam;
}


void Machine::init()
{
  memset(s_memory, 0, PCXT_RAM_SIZE);
  memset(s_videoMemory, 0, PCXT_VIDEOMEM_SIZE);

  m_i8042.init();
  m_i8042.setCallbacks(this, keyboardInterrupt, mouseInterrupt, resetMachine, sysReq);

  m_PIT8253.setCallbacks(this, PITChangeOut);
  m_PIT8253.reset();

  m_MC146818.init("PCXT");
  m_MC146818.setCallbacks(this, MC146818Interrupt);

  m_BIOS.init(this);

  i8086::setCallbacks(this, readPort, writePort,
                      writeVideoMemory8, writeVideoMemory16,
                      readVideoMemory8, readVideoMemory16, interrupt);
  i8086::setMemory(s_memory);

  m_reset = true;
}


void Machine::reset()
{
  m_reset = false;
  m_ticksCounter = 0;

  m_CGAMemoryOffset = 0;
  m_CGAModeReg      = 0;
  m_CGAColorReg     = 0;
  m_CGAVSyncQuery   = 0;
  m_speakerDataEnable = false;
  m_speakerFreq       = 0;

  m_i8042.reset();
  m_PIC8259A.reset();
  m_PIC8259B.reset();

  m_PIT8253.reset();
  m_PIT8253.setGate(0, true);

  m_MC146818.reset();

  memset(m_CGA6845, 0, sizeof(m_CGA6845));

  m_BIOS.reset();

  i8086::reset();

  // boot drive in DL (0,1 = floppy; 0x80,0x81 = HD)
  i8086::setDL((m_bootDrive & 1) | (m_bootDrive > 1 ? 0x80 : 0x00));
}


void Machine::run(int iterations)
{
  for (int i = 0; i < iterations; ++i) {
    if (m_reset)
      reset();
    if (s_stepHook)        // mouse INT 33h event-handler injection (cheap early-out when idle)
      s_stepHook();
    i8086::step();
    tick();
  }
}


void Machine::tick()
{
  ++m_ticksCounter;

  if ((m_ticksCounter & 0x7f) == 0x7f) {
    m_PIT8253.tick();
    m_i8042.tick();
    ++m_refreshToggle;
  }

  if (m_PIC8259A.pendingInterrupt() && i8086::IRQ(m_PIC8259A.pendingInterruptNum()))
    m_PIC8259A.ackPendingInterrupt();
  if (m_PIC8259B.pendingInterrupt() && i8086::IRQ(m_PIC8259B.pendingInterruptNum()))
    m_PIC8259B.ackPendingInterrupt();
}


//////////////////////////////////////////////////////////////////////////////
// I/O ports

void Machine::writePort(void * context, int address, uint8_t value)
{
  auto m = (Machine*)context;
  switch (address) {

    case 0x20: case 0x21:
      m->m_PIC8259A.write(address & 1, value); break;

    case 0xa0: case 0xa1:
      m->m_PIC8259B.write(address & 1, value); break;

    case 0x40: case 0x41: case 0x42: case 0x43:
      m->m_PIT8253.write(address & 3, value);
      if ((address == 0x43 && (value >> 6) == 2) || address == 0x42)
        m->speakerSetFreq();
      break;

    case 0x60:
      m->m_i8042.write(0, value); break;

    // Port B: bit1 = speaker data enable, bit0 = timer 2 gate
    case 0x61:
      m->m_speakerDataEnable = value & 0x02;
      m->m_PIT8253.setGate(2, value & 0x01);
      m->speakerEnableDisable();
      break;

    case 0x64:
      m->m_i8042.write(1, value); break;

    case 0x70: case 0x71:
      m->m_MC146818.write(address & 1, value); break;

    case 0x3d4: m->m_CGA6845SelectRegister = value; break;
    case 0x3d5: m->setCGA6845Register(value); break;
    case 0x3d8: m->m_CGAModeReg = value; m->setCGAMode(); break;
    case 0x3d9: m->m_CGAColorReg = value; m->setCGAMode(); break;

    default: break;
  }
}


uint8_t Machine::readPort(void * context, int address)
{
  auto m = (Machine*)context;
  switch (address) {

    case 0x20: case 0x21:
      return m->m_PIC8259A.read(address & 1);

    case 0xa0: case 0xa1:
      return m->m_PIC8259B.read(address & 1);

    case 0x40: case 0x41: case 0x42: case 0x43:
      return m->m_PIT8253.read(address & 3);

    case 0x60:
      return m->m_i8042.read(0);

    // Port B: bit5 = timer2 out, bit4 = DRAM refresh toggle, bit1 = spk enable, bit0 = timer2 gate
    case 0x61:
      return ((int)m->m_PIT8253.getOut(2) << 5) |
             ((m->m_refreshToggle & 1) << 4)     |
             ((int)m->m_speakerDataEnable << 1)  |
             ((int)m->m_PIT8253.getGate(2));

    case 0x62:
      return 0x20 * m->m_PIT8253.getOut(2);

    case 0x64:
      return m->m_i8042.read(1);

    case 0x70: case 0x71:
      return m->m_MC146818.read(address & 1);

    case 0x3d4:
      return 0x00;
    case 0x3d5:
      return (m->m_CGA6845SelectRegister >= 14 && m->m_CGA6845SelectRegister < 16)
               ? m->m_CGA6845[m->m_CGA6845SelectRegister] : 0x00;
    case 0x3d9:
      return m->m_CGAColorReg;
    case 0x3da:
      // fake vsync: "not VSync" (0x09) on 6 of every 7 reads, retrace (0x00) on the 7th
      m->m_CGAVSyncQuery += 1;
      return (m->m_CGAVSyncQuery & 0x7) != 0 ? 0x09 : 0x00;

    default:
      return 0xff;
  }
}


//////////////////////////////////////////////////////////////////////////////
// Video memory (0xB0000..0xBFFFF -> s_videoMemory[address - 0xB0000])

void Machine::writeVideoMemory8(void * context, int address, uint8_t value)
{
  if (address >= 0xb0000)
    s_videoMemory[address - 0xb0000] = value;
}

void Machine::writeVideoMemory16(void * context, int address, uint16_t value)
{
  if (address >= 0xb0000)
    *(uint16_t*)(s_videoMemory + (address - 0xb0000)) = value;
}

uint8_t Machine::readVideoMemory8(void * context, int address)
{
  return address >= 0xb0000 ? s_videoMemory[address - 0xb0000] : 0xff;
}

uint16_t Machine::readVideoMemory16(void * context, int address)
{
  return address >= 0xb0000 ? *(uint16_t*)(s_videoMemory + (address - 0xb0000)) : 0xffff;
}


//////////////////////////////////////////////////////////////////////////////
// Magic-interrupt BIOS bridge (only inside the BIOS segment)

bool Machine::interrupt(void * context, int num)
{
  auto m = (Machine*)context;

  // Mouse INT 33h: serviced directly from the USB mouse, from any code segment (not just the BIOS).
  if (num == 0x33 && s_int33 && s_int33()) return true;

  if (i8086::CS() == BIOS_SEG) {
    switch (num) {

      case 0xf4:   // putchar (AL) - debug
        printf("%c", i8086::AX() & 0xff);
        return true;

      case 0xf5:   // BIOS helpers
        m->m_BIOS.helpersEntry();
        return true;

      case 0xf6: { // set/reset CF before IRET
        auto sf = (uint16_t*)(s_memory + i8086::SS() * 16 + (uint16_t)(i8086::SP() + 4));
        *sf = (*sf & 0xfffe) | i8086::flagCF();
        return true;
      }
      case 0xf7: { // set/reset ZF before IRET
        auto sf = (uint16_t*)(s_memory + i8086::SS() * 16 + (uint16_t)(i8086::SP() + 4));
        *sf = (*sf & 0xffbf) | (i8086::flagZF() << 6);
        return true;
      }
      case 0xf8: { // set/reset IF before IRET
        auto sf = (uint16_t*)(s_memory + i8086::SS() * 16 + (uint16_t)(i8086::SP() + 4));
        *sf = (*sf & 0xfdff) | (i8086::flagIF() << 9);
        return true;
      }

      case 0xfb:   // disk handler (INT 13h) - serialize SD/HSPI vs core-0 touch/render
        if (s_diskLock) s_diskLock();
        m->m_BIOS.diskHandlerEntry();
        if (s_diskUnlock) s_diskUnlock();
        return true;

      case 0xfc:   // video handler (INT 10h)
        m->m_BIOS.videoHandlerEntry();
        return true;
    }
  }
  return false;
}


//////////////////////////////////////////////////////////////////////////////
// Device interrupt callbacks

void Machine::PITChangeOut(void * context, int timerIndex)
{
  auto m = (Machine*)context;
  if (timerIndex == 0 && m->m_PIT8253.getOut(0))
    m->m_PIC8259A.signalInterrupt(0);   // IRQ0 (INT 08h)
}

bool Machine::keyboardInterrupt(void * context)
{
  return ((Machine*)context)->m_PIC8259A.signalInterrupt(1);  // IRQ1 (INT 09h)
}

bool Machine::mouseInterrupt(void * context)
{
  return ((Machine*)context)->m_PIC8259B.signalInterrupt(4);  // IRQ12
}

bool Machine::MC146818Interrupt(void * context)
{
  return ((Machine*)context)->m_PIC8259B.signalInterrupt(0);  // IRQ8 (INT 70h)
}

bool Machine::resetMachine(void * context)
{
  ((Machine*)context)->trigReset();
  return true;
}

bool Machine::sysReq(void * /*context*/)
{
  return true;
}


//////////////////////////////////////////////////////////////////////////////
// CGA

void Machine::setCGAMode()
{
  if ((m_CGAModeReg & CGA_MODECONTROLREG_ENABLED) == 0) {
    m_graphicsAdapter.enableVideo(false);
    return;
  }

  m_frameBuffer = s_videoMemory + 0x8000 + m_CGAMemoryOffset;  // 0xB8000 window
  m_graphicsAdapter.setVideoBuffer(m_frameBuffer);

  if ((m_CGAModeReg & CGA_MODECONTROLREG_GRAPHICS) == 0) {
    // text
    m_graphicsAdapter.setEmulation((m_CGAModeReg & CGA_MODECONTROLREG_TEXT80)
        ? GraphicsAdapter::Emulation::PC_Text_80x25_16Colors
        : GraphicsAdapter::Emulation::PC_Text_40x25_16Colors);
    m_graphicsAdapter.setBit7Blink(m_CGAModeReg & CGA_MODECONTROLREG_BIT7BLINK);
  } else if ((m_CGAModeReg & CGA_MODECONTROLREG_GRAPH640) == 0) {
    // 320x200x4
    m_graphicsAdapter.setEmulation(GraphicsAdapter::Emulation::PC_Graphics_320x200_4Colors);
    int paletteIndex = (bool)(m_CGAColorReg & CGA_COLORCONTROLREG_PALETTESEL) * 2
                     + (bool)(m_CGAColorReg & CGA_COLORCONTROLREG_HIGHINTENSITY);
    m_graphicsAdapter.setPCGraphicsPaletteInUse(paletteIndex);
    m_graphicsAdapter.setPCGraphicsBackgroundColorIndex(m_CGAColorReg & CGA_COLORCONTROLREG_BACKCOLR_MASK);
  } else {
    // 640x200x2
    m_graphicsAdapter.setEmulation(GraphicsAdapter::Emulation::PC_Graphics_640x200_2Colors);
    m_graphicsAdapter.setPCGraphicsForegroundColorIndex(m_CGAColorReg & CGA_COLORCONTROLREG_BACKCOLR_MASK);
  }
  m_graphicsAdapter.enableVideo(true);
}


void Machine::setCGA6845Register(uint8_t value)
{
  m_CGA6845[m_CGA6845SelectRegister] = value;
  switch (m_CGA6845SelectRegister) {
    case 0x0a:
      m_graphicsAdapter.setCursorVisible((m_CGA6845[0xa] >> 5) >= 2);
      // fallthrough
    case 0x0b:
      m_graphicsAdapter.setCursorShape(2 * (m_CGA6845[0xa] & 0x1f), 2 * (m_CGA6845[0xb] & 0x1f));
      break;
    case 0x0c:
    case 0x0d:
      m_CGAMemoryOffset = ((m_CGA6845[0xc] << 8) | m_CGA6845[0xd]) << 1;
      setCGAMode();
      break;
    case 0x0e:
    case 0x0f: {
      int cols = m_graphicsAdapter.getTextColumns();
      int pos = (m_CGA6845[0xe] << 8) | m_CGA6845[0xf];
      if (cols > 0)
        m_graphicsAdapter.setCursorPos(pos / cols, pos % cols);
      break;
    }
  }
}


//////////////////////////////////////////////////////////////////////////////
// Speaker (state only; audio task reads speakerEnabled()/speakerFreq())

void Machine::speakerSetFreq()
{
  int timerCount = m_PIT8253.timerInfo(2).resetCount;
  if (timerCount == 0)
    timerCount = 65536;
  m_speakerFreq = PIT_TICK_FREQ / timerCount;
  if (s_speakerCb) s_speakerCb(m_speakerFreq, speakerEnabled());
}

void Machine::speakerEnableDisable()
{
  if (s_speakerCb) s_speakerCb(m_speakerFreq, speakerEnabled());
}


//////////////////////////////////////////////////////////////////////////////
// Disk image attach (FILE*-backed; device build bridges fopen to SD in M2)

void Machine::setDriveImage(int drive, char const * filename, int cylinders, int heads, int sectors)
{
  if (m_diskCtx[drive]) { if (s_diskClose) s_diskClose(m_diskCtx[drive]); m_diskCtx[drive] = nullptr; }
  if (m_diskFilename[drive]) { free(m_diskFilename[drive]); m_diskFilename[drive] = nullptr; }

  m_BIOS.setDriveMediaType(drive, mediaUnknown);

  m_diskCylinders[drive] = cylinders;
  m_diskHeads[drive]     = heads;
  m_diskSectors[drive]   = sectors;
  m_diskSize[drive]      = 0;
  m_diskChanged[drive]   = true;

  if (filename && s_diskOpen) {
    m_diskFilename[drive] = strdup(filename);
    uint64_t sz = 0;
    m_diskCtx[drive] = s_diskOpen(filename, &sz);
    if (m_diskCtx[drive]) {
      m_diskSize[drive] = sz;
      if (cylinders == 0 || heads == 0 || sectors == 0)
        autoDetectDriveGeometry(drive);
    }
  }
}


void Machine::autoDetectDriveGeometry(int drive)
{
  static const struct { uint16_t tracks; uint8_t sectors; uint8_t heads; } FLOPPYFORMATS[] = {
    { 40,  8, 1 }, { 40,  9, 1 }, { 40,  8, 2 }, { 40,  9, 2 },
    { 80,  9, 2 }, { 80, 15, 2 }, { 80, 18, 2 }, { 80, 36, 2 },
  };
  for (auto const & ff : FLOPPYFORMATS) {
    if ((uint64_t)512 * ff.tracks * ff.sectors * ff.heads == m_diskSize[drive]) {
      m_diskCylinders[drive] = ff.tracks;
      m_diskHeads[drive]     = ff.heads;
      m_diskSectors[drive]   = ff.sectors;
      return;
    }
  }
  // hard disk geometry (max ~528MB)
  constexpr int MAXCYL = 1024, MAXHEADS = 16, MAXSECTORS = 63;
  int c = 1, h = 1;
  int s = (int)(m_diskSize[drive] / 512);
  if (s > MAXSECTORS) { h = s / MAXSECTORS; s = MAXSECTORS; }
  if (h > MAXHEADS)   { c = h / MAXHEADS;   h = MAXHEADS; }
  if (c > MAXCYL)     c = MAXCYL;
  m_diskCylinders[drive] = c;
  m_diskHeads[drive]     = h;
  m_diskSectors[drive]   = s;
}
