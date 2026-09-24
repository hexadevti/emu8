#include "../../emu.h"
#if defined(BOARD_DESKTOP)
#include "../desktop/debug_bridge.h"   // dbgBusTouch: desktop memory-access heat map (no-op on device)
#endif

// Set when any of the buffers below could not be allocated. read8/write8 index these pointers
// directly, so a null one is not a degraded mode -- it is a wild pointer. memset(NULL, 0, 48K) in
// particular walks straight off the end of the RP2040 bootrom into unmapped address space and
// hard-faults the core, which on the PicoCalc also kills the USB CDC task and leaves nothing but a
// black screen and an unrecognised USB device. So: check every allocation, say which one failed,
// and let apple2RenderLoadWarning() put it on the panel instead of running a broken 6502.
bool apple2MemAllocFailed = false;

// Set when a IIe was asked for and the board could not hold one, so a II+ was built instead. The
// boot splash reads it and greys its IIe button out (src/shared/video.cpp): one wasted boot, and
// after that the panel says why instead of silently handing back a II+ every time.
bool apple2IIeUnavailable = false;

static unsigned char* a2Alloc(size_t n, const char* what) {
  unsigned char* p = (unsigned char*)malloc(n);
  if (!p) {
    sprintf(buf, "Apple II: out of RAM allocating %s (%u bytes)", what, (unsigned)n);
    printLog(buf);
    apple2MemAllocFailed = true;
    apple2RomLoadFailed  = true;   // the same halt path: loop() stops running the CPU
  }
  return p;
}

// The IIe-only half of the map: aux RAM plus the six aux/main bank-switched blocks. Returns false
// (having freed whatever it did get) if the board cannot spare the room, so the caller can come up
// as a II+ instead of as a red "NOT ENOUGH RAM" screen.
//
// Three of the four 4K banks come out of the tail of sharedBigBuf rather than the heap. The Apple II
// uses only the first 0xC000 of that 64032-byte static buffer -- read8/write8 send everything above
// $BFFF to soft switches, ROM or the language card -- so 14880 bytes of it sit idle for the whole
// session. On the RP2040 that 12K is the difference between the IIe map fitting and not.
// What has to be left over AFTER the map for a IIe to actually reach a BASIC prompt: the system
// ROMs it still loads from SD once FSSetup has run, plus FSSetup's own buffers (~5K measured), the
// disk/HD image scan, the file-browser vectors and the FreeRTOS task stacks that setup() creates
// below us. A IIe does NOT load main.bin -- read8 reaches it only from the II+ side of
// `if (AppleIIe)` -- so the ROM half of this is iie.bin plus 1072 bytes of card ROM, and where
// BOARD_A2_ROM_IN_FLASH holds iie.bin in flash it is the 1072 bytes alone.
//
// Compared against the reported free size and NOT against a probe malloc, which is what this
// originally did. On the RP2040 malloc handed back a 46000-byte block with 25404 bytes free and the
// boot then died on the next ROM: newlib's heap grows toward the core stacks, so a malloc that
// succeeds does not mean the memory was there to give. The reported figure is the conservative one.
//
// It is still only an estimate of what comes later, so it is the cheap early exit rather than the
// decision: apple2LoadRoms() makes the real call once FSSetup's buffers are also on the books.
#if BOARD_A2_ROM_IN_FLASH
#define A2_IIE_RESERVE 20000    // 1072 of card ROM + SD buffers + scan + task stacks
#else
#define A2_IIE_RESERVE 34000    // ...and 16696 more when iie.bin comes off the card
#endif

// True while auxram / the three heap IIe banks hold malloc'd blocks, so apple2FallbackToIIplus()
// knows whether there is anything to give back. The other three IIe pointers are inside
// sharedBigBuf and must never be freed.
static bool a2IIeMapOnHeap = false;

static bool a2AllocIIeMap()
{
  static_assert(sizeof(sharedBigBuf) >= 0xc000 + 3 * 0x1000,
                "Apple IIe banks are carved out of sharedBigBuf past the 48K the 6502 can see");
  unsigned char* tail = sharedBigBuf + 0xc000;

  unsigned char* ax   = a2Alloc(0xc000, "aux RAM");
  unsigned char* ab1  = a2Alloc(0x2000, "IIe aux bank-switched RAM 1");
  unsigned char* mb1  = a2Alloc(0x2000, "IIe bank-switched RAM 1");
  unsigned char* mb21 = a2Alloc(0x1000, "IIe bank-switched RAM 2.1");
  size_t freeLeft = apple2MemAllocFailed ? 0 : heap_caps_get_free_size(MALLOC_CAP_8BIT);
  if (!apple2MemAllocFailed) {
    sprintf(buf, "Apple II: IIe map fits, %u bytes left, want %d more for ROM/disk/stacks",
            (unsigned)freeLeft, A2_IIE_RESERVE);
    printLog(buf);
  }
  if (apple2MemAllocFailed || freeLeft < A2_IIE_RESERVE) {
    free(ax); free(ab1); free(mb1); free(mb21);
    apple2MemAllocFailed = false;   // not fatal: memoryAlloc() falls back to the II+ map
    apple2RomLoadFailed  = false;
    return false;
  }
  auxram                      = ax;
  IIEAuxBankSwitchedRAM1      = ab1;
  IIEmemoryBankSwitchedRAM1   = mb1;
  IIEmemoryBankSwitchedRAM2_1 = mb21;
  IIEAuxBankSwitchedRAM2_1    = tail;             // the three carved out of sharedBigBuf's
  IIEAuxBankSwitchedRAM2_2    = tail + 0x1000;    // idle tail, 4K each
  IIEmemoryBankSwitchedRAM2_2 = tail + 0x2000;
  memset(ax,   0, 0xc000);
  memset(ab1,  0, 0x2000);
  memset(mb1,  0, 0x2000);
  memset(mb21, 0, 0x1000);
  memset(tail, 0, 3 * 0x1000);
  a2IIeMapOnHeap = true;
  return true;
}

// A II+ has none of that silicon, so none of it is allocated -- 89K of heap that the II+ never
// touches. The pointers are aliased rather than left null because $C000-$C00F (STORE80 / RAMRD /
// RAMWRT / ALTZP) are latched unconditionally in softswitches.cpp: on a real II+ those switches do
// not exist and the write is inert, so reads keep coming from main RAM and the language card, which
// is exactly what aliasing aux at `ram` and the six IIe blocks at their II+ counterparts produces.
// (Every path that indexes them is gated on AppleIIe anyway; this is the belt to that braces.)
static void a2AliasIIplusMap()
{
  auxram                      = ram;
  IIEAuxBankSwitchedRAM1      = memoryBankSwitchedRAM1;      // 0x2000 apiece
  IIEmemoryBankSwitchedRAM1   = memoryBankSwitchedRAM1;
  IIEAuxBankSwitchedRAM2_1    = memoryBankSwitchedRAM2_1;    // 0x1000 apiece
  IIEmemoryBankSwitchedRAM2_1 = memoryBankSwitchedRAM2_1;
  IIEAuxBankSwitchedRAM2_2    = memoryBankSwitchedRAM2_2;
  IIEmemoryBankSwitchedRAM2_2 = memoryBankSwitchedRAM2_2;
}

// Give the IIe map back and come up as a II+ instead. Called from two places, because there are
// two ways to find out that a IIe does not fit and only the second one is conclusive: the reserve
// probe above (cheap, a guess) and iie.bin failing to allocate for real (src/apple2/apple2_roms.cpp,
// after FSSetup has taken its own bite out of the heap). Either way a working II+ beats a halted
// IIe, and freeing 68K here is what lets the II+ ROMs, the disk scan and the FreeRTOS task stacks
// below us all still fit.
void apple2FallbackToIIplus(const char* why)
{
  if (a2IIeMapOnHeap) {
    free(auxram);                        // the three sharedBigBuf-backed banks are NOT freed
    free(IIEAuxBankSwitchedRAM1);
    free(IIEmemoryBankSwitchedRAM1);
    free(IIEmemoryBankSwitchedRAM2_1);
    a2IIeMapOnHeap = false;
  }
  AppleIIe = false;
  activeFlags = flagsIIplus;
  a2AliasIIplusMap();                    // ...so the six IIe pointers are valid again, not dangling
  apple2IIeUnavailable = true;           // the splash greys its IIe button out from now on
  apple2MemAllocFailed = false;          // neither failure is fatal any more: the II+ map is live
  apple2RomLoadFailed  = false;
  sprintf(buf, "Apple II: %s -> coming up as a II+ (%u bytes free)", why,
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
  printLog(buf);
  // Write the downgrade back to EEPROM. Without this the card still says IIe, so every power-on
  // repeats this whole dance and lands on the splash again; the user picking II+ there does not
  // save it either, because by then AppleIIe already reads false and splashSelect sees no change.
  // One flash write on the one boot that discovers the board cannot do it.
  saveConfig();
  // Show the boot splash on THIS boot rather than dropping the user into a II+ with no explanation.
  // We run before videoSetup(), which is what reads the magic, so the panel comes up on the system
  // menu with IIe greyed "NO RAM" and II+ highlighted. Timing out from there boots the II+ we just
  // built, so nothing is lost by it.
  requestSplashOnNextBoot();
}

// Build the memory map for ONE machine. The boot splash offers Apple II+ and Apple IIe as two
// separate systems (src/shared/video.cpp) and the choice is fixed for the session, so unlike the
// old MACHINE toggle -- which switched read8/write8 between two maps that were both allocated up
// front -- only the selected map exists. II+ costs ~16K of heap here, IIe ~86K.
void memoryAlloc() {
  showFreeMem();
  ram = sharedBigBuf;   // main RAM = the shared static buffer (the C64 framebuffer when on C64);
                        // frees ~48K of heap so the render/joystick/disk tasks can allocate.
  // The language card, common to both machines.
  memoryBankSwitchedRAM1 = a2Alloc(0x2000, "bank-switched RAM 1");
  memoryBankSwitchedRAM2_1 = a2Alloc(0x1000, "bank-switched RAM 2.1");
  memoryBankSwitchedRAM2_2 = a2Alloc(0x1000, "bank-switched RAM 2.2");
  if (!apple2MemAllocFailed && AppleIIe && !a2AllocIIeMap())
    apple2FallbackToIIplus("no room for the IIe map plus its ROM/disk/stack budget");
  if (!AppleIIe) a2AliasIIplusMap();
  menuScreen = a2Alloc(0x546, "menu screen");
  menuColor = a2Alloc(0x546, "menu color");
  showFreeMem();
  if (apple2MemAllocFailed) {
    printLog("Apple II: NOT starting the 6502 -- this board does not have enough RAM.");
    return;                              // nothing below is safe with a null pointer
  }
  memset(ram, 0, 0xc000 * sizeof(unsigned char));
  memset(memoryBankSwitchedRAM1, 0, 0x2000 * sizeof(unsigned char));
  memset(memoryBankSwitchedRAM2_1, 0, 0x1000 * sizeof(unsigned char));
  memset(memoryBankSwitchedRAM2_2, 0, 0x1000 * sizeof(unsigned char));
  memset(menuScreen, 0xa0, 0x546 * sizeof(unsigned char));
  memset(menuColor, 0xf0, 0x546 * sizeof(unsigned char));
  showFreeMem();
}

void showFreeMem() {
  Serial.print("Free mem:");
  Serial.print(heap_caps_get_free_size(MALLOC_CAP_8BIT));
  Serial.print(" (");
  Serial.print(heap_caps_get_free_size(MALLOC_CAP_8BIT) / sizeof(float));
  Serial.println(" floats)");
}

IRAM_ATTR unsigned char read8(unsigned short address)
{
#if defined(BOARD_DESKTOP)
  dbgBusTouch(address, DBG_HEAT_R);
#endif

  if (address < 0x0200)
  {
    if (AltZPOn_Off)
      return auxzp[address];
    else
      return zp[address];
  }
  else if (address < 0xc000)
  {
    if (!Store80On_Off)
    {
      if (RAMReadOn_Off)
        return auxram[address];
      else
        return ram[address];
    }
    else
    {
      if (address >= 0x0400 && address < 0x0800)
      {
        if (!Page1_Page2) // Page 2
          return auxram[address];
        else // Page 1
          return ram[address];
      } // Text Pages
      else if (address >= 0x2000 && address < 0x4000) // Graphics Pages
      {
        if (LoRes_HiRes)
        {
          if (RAMReadOn_Off)
            return auxram[address];
          else
            return ram[address];
        }
        else
        {
          if (!Page1_Page2) // Page 2
            return auxram[address];
          else // Page 1
            return ram[address];
        }
      }
      else
      {
        if (RAMReadOn_Off)
          return auxram[address];
        else
          return ram[address];
      }
    }
  }
  else if (address < 0xc100)
  { // Softswitches
    // $C030 is the hottest soft switch in the machine by orders of magnitude: a noise routine hits
    // it about every 9 emulated cycles, where every other switch here is touched a handful of times
    // a frame. It is handled inline rather than through the general path because that path is three
    // nested calls -- readSoftSwitches, processSoftSwitches, and a jump table over ~40 cases --
    // living in flash, so on the PicoCalc every access paid XIP misses. Measured there at ~1.2us a
    // toggle, which is worse than it sounds: it is not a flat tax but one PROPORTIONAL TO HOW MUCH
    // SOUND IS PLAYING. The emulator ran 1.00MHz in silence and 0.92MHz under dense noise, and
    // because the audio replay clock tracks the guest clock, the pitch sagged in time with the
    // sound. read8 is already IRAM_ATTR, so this branch costs no flash fetch at all.
    //
    // Behaviour-identical to going the long way round: the $C030 case in processSoftSwitches does
    // `speakerToggle(); break;` and the function tails out at `return 0;`. The case there is kept,
    // both because writes still reach it and because it is where the switch is documented.
    if (address == 0xc030) { speakerToggle(); return 0; }
    return readSoftSwitches(address);
  }
  else if (address < 0xc800)
  {
    // Null-checked, not AppleIIe-checked, so behaviour is identical wherever iie.bin IS
    // loaded. A II+ session never loads it (see apple2LoadRoms) and
    // these three sites are the only ones that read it without an AppleIIe gate in front,
    // so without the check a II+ read of $C300 -- which DOS and the 80-column scan both do
    // -- would dereference null. A real II+ has nothing in these slots anyway.
    if (appleiieenhancedc0ff && IntCXRomOn_Off)
    {
      return appleiieenhancedc0ff[address - 0xc000];
    }
    else
    {
      if (address >= 0xc300 && address < 0xc400)
      {
        if (appleiieenhancedc0ff && !SlotC3RomOn_Off)
        { 
          IntC8RomOn_Off = true;
          return appleiieenhancedc0ff[address - 0xc000];
        }
        else
        {
          IntC8RomOn_Off = false;
          return 0;
        }
      }
      else if (address >= 0xc400 && address < 0xc500)
      {
        return mouse ? mousecardrom[address - 0xc400] : 0;
      }
      else if (address >= 0xc600 && address < 0xc700)
      {
        return diskAttached ? diskiicardrom[address - 0xc600] : 0;
      }
      else if (address >= 0xc700 && address < 0xc800)
      {
        return hdAttached ? hdrom[address - 0xc700] : 0;
      }
    }
  }
  else if (address < 0xd000)
  {
    if (appleiieenhancedc0ff && IntC8RomOn_Off)
      return appleiieenhancedc0ff[address - 0xc000];
  }
  else
  {
    if (MemoryBankReadRAM_ROM)
    {
      return languagecardRead(address);
    }
    else
    {
      if (AppleIIe)
      {
        if (IIEMemoryBankReadRAM_ROM)
        {
          if (address < 0xe000)
          {
            if (IIEMemoryBankBankSelect1_2)
            {
              if (AltZPOn_Off)
                return IIEAuxBankSwitchedRAM2_1[address - 0xd000];
              else
                return IIEmemoryBankSwitchedRAM2_1[address - 0xd000];
            }
            else
            {
              if (AltZPOn_Off)
                return IIEAuxBankSwitchedRAM2_2[address - 0xd000];
              else
                return IIEmemoryBankSwitchedRAM2_2[address - 0xd000];
            }
          }
          else
          {
            if (AltZPOn_Off)
              return IIEAuxBankSwitchedRAM1[address - 0xe000];
            else
              return IIEmemoryBankSwitchedRAM1[address - 0xe000];
          }
        }
        else
          return appleiieenhancedc0ff[address - 0xc000];
      }
      else
        return rom[address - 0xd000];
    }
  }

  // Unmapped $C100-$CFFF: on the IIe these fall back to internal ROM (matches real
  // hardware and is what the IIe boot path expects); II+ tolerates 0. Returning a
  // defined value also avoids undefined behavior under -O2.
  if (AppleIIe)
    return appleiieenhancedc0ff[address - 0xc000];
  return 0;
}

IRAM_ATTR void write8(unsigned short address, unsigned char value)
{
#if defined(BOARD_DESKTOP)
  dbgBusTouch(address, DBG_HEAT_W);
#endif

  if (address < 0x0200)
  {
    if (AltZPOn_Off)
      auxzp[address] = value;
    else
      zp[address] = value;
  }
  else if (address < 0xc000)
  {
    if (!Store80On_Off)
    {
      if (RAMWriteOn_Off)
        auxram[address] = value;
      else
        ram[address] = value;
    }
    else // softswitches.Store80On_Off
    {
      if (address >= 0x0400 && address < 0x0800) // Text Pages
      {
        if (!Page1_Page2)
          auxram[address] = value;
        else
          ram[address] = value;
      }
      else if (address >= 0x2000 && address < 0x4000) // Graphics Pages
      {
        if (LoRes_HiRes)
        {
          if (RAMWriteOn_Off)
            auxram[address] = value;
          else
            ram[address] = value;
        }
        else
        {
          if (!Page1_Page2) // Page 2
            auxram[address] = value;
          else // Page 1
            ram[address] = value;
        }
      }
      else
      {
        if (RAMWriteOn_Off)
          auxram[address] = value;
        else
          ram[address] = value;
      }
    }
  }
  else if (address < 0xc100)
  { // Softswitched
    if (address == 0xc030) { speakerToggle(); return; }   // hot path; see the note in read8
    writeSoftSwitches(address, value);
  }
  else if (address >= 0xd000)
  {
    if (AppleIIe)
    {
      if (address >= 0xd000 && address < 0xe000)
      {
        if (IIEMemoryBankBankSelect1_2)
        {
          if (AltZPOn_Off)
            IIEAuxBankSwitchedRAM2_1[address - 0xd000] = value;
          else
            IIEmemoryBankSwitchedRAM2_1[address - 0xd000] = value;
        }
        else
        {
          if (AltZPOn_Off)
            IIEAuxBankSwitchedRAM2_2[address - 0xd000] = value;
          else
            IIEmemoryBankSwitchedRAM2_2[address - 0xd000] = value;
        }
      }
      else
      {
        if (AltZPOn_Off)
          IIEAuxBankSwitchedRAM1[address - 0xe000] = value;
        else
          IIEmemoryBankSwitchedRAM1[address - 0xe000] = value;
      }
    }
    else
    {
      if (MemoryBankWriteRAM_NoWrite)
        languagecardWrite(address, value);
    }
  }
}

IRAM_ATTR unsigned short read16(unsigned short address)
{
  return (unsigned short)read8(address) | (((unsigned short)read8(address + 1)) << 8);
}

void write16(unsigned short address, unsigned short value)
{
  write8(address, value & 0x00FF);
  write8(address + 1, (value >> 8) & 0x00FF);
}
