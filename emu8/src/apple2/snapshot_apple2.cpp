// snapshot_apple2.cpp - Apple II+/IIe save state (see src/shared/snapshot.h for the contract).
//
// One function, both directions. The stream, after snapshot.cpp's header:
//   "APL2" u8 model (1 = IIe)  u32 aux RAM size  u32 IIe bank RAM size  u8 cards (disk/HD bits)
//   "CPU "  PC SP A X Y SR
//   "SWCH"  every soft switch: video, MMU/IOU, both language cards, keyboard latch, paddles
//   "RAM "  main $0000-$BFFF, zp + auxzp, the II+ language card (16K) and, on a IIe, aux
//           $0000-$BFFF plus the six IIe bank-switched blocks (32K)
//   "DSK2"  Disk II: phases, motor, drive, Q6/Q7, head track, nibble pointer, stepper history, a
//           half-written sector, and -- only when a write had not reached the card yet -- the
//           dirty track (4K) with the image name it belongs to
//   "HDCD"  HD card registers;  "MOUS" mouse card button edge
// Every block is streamed in and out of the buffers it lives in (memory.cpp: sharedBigBuf, heap,
// lent C64 code RAM, the lent audio stack): no copy, no allocation. ROM is never saved. Nothing in
// the Apple map is a cached bank pointer -- read8/write8 test the flags every access -- so the
// flags are all there is to put back.
//
// Not saved, on purpose: the disk / HD images (they stay mounted), host input (mouse position,
// paddle positions, buttons), the speaker (host audio, not guest state), the guest clock and the
// pacer (they re-anchor by themselves after the >100ms the menu was open), and video-owned
// Vertical_blankingOn_Off.

#include "../../emu.h"
#include "../shared/snapshot.h"

// disk.cpp / hd.cpp / mouse.cpp globals that no header exports.
extern bool Drive1_2;
extern int pointer;
extern int lastSec;
extern int lastTrack;
extern bool trackChanged;
extern std::queue<uint8_t> phaseBuffer;
extern std::vector<uint8_t> outputSectorData;
#if defined(BOARD_PICOCALC)
extern uint8_t *const trackRawData;      // in platformPool (emu.h)
#else
extern uint8_t trackRawData[];
#endif
extern unsigned char hdCommand;
extern unsigned char hdStatus;
extern bool hdUnitNumber1_2;
extern unsigned short hdMemoryBuffer;
extern unsigned short hdBlockNumber;
extern ushort lastBlock;
extern bool lastMouseButton;

#define A2SNAP_TRACK_RAW   4096          // disk.cpp trackRawSize
#define A2SNAP_TRACK_ENC   5856          // disk.cpp trackEncodedSize
#define A2SNAP_SECTOR_MAX  354           // diskSoftSwitchesWrite flushes a sector at this size
#define A2SNAP_NAME_LEN    64

// An int through a fixed 32 bits (int is not the same width everywhere this builds).
static void snapInt(SnapIO &io, int &v)
{
  int32_t t = (int32_t)v;
  io.val(t);
  if (io.loading) v = (int)t;
}

bool apple2Snapshot(SnapIO &io)
{
  // ---- Identity: read into locals and refuse before anything is touched ----
  // Not io.tag(): a bad tag there is a fail(), which snapshot.cpp treats as a half-loaded
  // machine and resets. Here nothing has been loaded yet, so it is a reject().
  char id[4] = { 'A', 'P', 'L', '2' };
  uint8_t model = AppleIIe ? 1 : 0;
  uint32_t auxSize = AppleIIe ? 0xc000 : 0;
  uint32_t iieBankSize = AppleIIe ? 2 * 0x2000 + 4 * 0x1000 : 0;
  uint8_t cards = (diskAttached ? 1 : 0) | (hdAttached ? 2 : 0);
  io.bytes(id, 4);
  io.val(model);
  io.val(auxSize);
  io.val(iieBankSize);
  io.val(cards);
  if (!io.ok) return false;
  if (io.loading) {
    if (memcmp(id, "APL2", 4) != 0) { io.reject("Snapshot is damaged"); return false; }
    if (model != (AppleIIe ? 1 : 0) || auxSize != (AppleIIe ? 0xc000u : 0u) ||
        iieBankSize != (AppleIIe ? 0x8000u : 0u)) {
      io.reject("Snapshot is for another model");
      return false;
    }
    if (cards != ((diskAttached ? 1 : 0) | (hdAttached ? 2 : 0))) {
      io.reject("Snapshot needs other slot cards");   // a booted DOS with its controller gone
      return false;
    }
    // A sector write the running machine has not flushed yet would be lost (and the disk left
    // half-updated): the late save needs the CPU running with the motor off, and we may not
    // touch the card from here.
    if (trackPendingSave) { io.reject("Disk write pending - resume, then load"); return false; }
  }

  // ---- 6502 ----
  // Parked at the top of cpuLoop(), before the next opcode fetch: PC is the next instruction.
  io.tag("CPU ");
  io.val(PC);
  io.val(STP);
  io.val(A);
  io.val(X);
  io.val(Y);
  io.val(SR);

  // ---- Soft switches ----
  io.tag("SWCH");
  io.flag(Graphics_Text);
  io.flag(Page1_Page2);
  io.flag(DisplayFull_Split);
  io.flag(LoRes_HiRes);
  io.flag(Cols40_80);
  io.flag(AltCharSetOn_Off);
  io.flag(DHiResOn_Off);
  io.flag(IOUDisOn_Off);
  io.flag(Store80On_Off);
  io.flag(RAMReadOn_Off);
  io.flag(RAMWriteOn_Off);
  io.flag(AltZPOn_Off);
  io.flag(IntCXRomOn_Off);
  io.flag(IntC8RomOn_Off);
  io.flag(SlotC3RomOn_Off);
  io.flag(IIEMemoryBankBankSelect1_2);    // IIe language card
  io.flag(IIEMemoryBankReadRAM_ROM);
  io.flag(IIEMemoryBankWriteRAM_NoWrite);
  io.flag(MemoryBankBankSelect1_2);       // II+ language card
  io.flag(MemoryBankReadRAM_ROM);
  io.flag(MemoryBankWriteRAM_NoWrite);
  {
    uint8_t k = (uint8_t)keymem;          // keyboard latch, bit 7 = strobe
    io.val(k);
    if (io.loading) keymem = (char)k;
  }
  io.flag(CgReset0); io.flag(CgReset1); io.flag(CgReset2); io.flag(CgReset3);   // paddle timers
  io.flag(Cg0); io.flag(Cg1); io.flag(Cg2); io.flag(Cg3);                       // mid-read
  snapInt(io, joystickCycles0);
  snapInt(io, joystickCycles1);
  snapInt(io, joystickCycles2);
  snapInt(io, joystickCycles3);

  // ---- RAM ----
  io.tag("RAM ");
  io.bytes(ram, 0xc000);
  io.arr(zp);
  io.arr(auxzp);                          // used on a II+ too: ALTZP latches there as well
  io.bytes(memoryBankSwitchedRAM1, 0x2000);
  io.bytes(memoryBankSwitchedRAM2_1, 0x1000);
  io.bytes(memoryBankSwitchedRAM2_2, 0x1000);
  if (AppleIIe) {                         // on a II+ these alias the blocks above (memory.cpp)
    io.bytes(auxram, 0xc000);
    io.bytes(IIEAuxBankSwitchedRAM1, 0x2000);
    io.bytes(IIEmemoryBankSwitchedRAM1, 0x2000);
    io.bytes(IIEAuxBankSwitchedRAM2_1, 0x1000);
    io.bytes(IIEAuxBankSwitchedRAM2_2, 0x1000);
    io.bytes(IIEmemoryBankSwitchedRAM2_1, 0x1000);
    io.bytes(IIEmemoryBankSwitchedRAM2_2, 0x1000);
  }

  // ---- Disk II ----
  io.tag("DSK2");
  io.flag(DrivePhase0ON_OFF);
  io.flag(DrivePhase1ON_OFF);
  io.flag(DrivePhase2ON_OFF);
  io.flag(DrivePhase3ON_OFF);
  io.flag(DriveMotorON_OFF);
  io.flag(Drive1_2);
  io.flag(DriveQ6H_L);
  io.flag(DriveQ7H_L);
  snapInt(io, diskTrack);
  snapInt(io, pointer);
  snapInt(io, lastTrack);
  snapInt(io, lastSec);
  {
    // Stepper history (addPhase keeps the last four phase writes).
    uint8_t ph[4] = { 0, 0, 0, 0 };
    uint8_t n = 0;
    if (!io.loading) {
      std::queue<uint8_t> q = phaseBuffer;   // at most 4 bytes
      while (!q.empty() && n < 4) { ph[n++] = q.front(); q.pop(); }
    }
    io.val(n);
    io.arr(ph);
    if (io.loading) {
      if (n > 4) { io.fail("Snapshot is damaged"); return false; }
      while (!phaseBuffer.empty()) phaseBuffer.pop();
      for (uint8_t i = 0; i < n; i++) phaseBuffer.push(ph[i]);
    }
  }
  {
    // A sector the 6502 is part-way through writing ($C0ED); flushed at 354 bytes.
    uint16_t n = (uint16_t)outputSectorData.size();
    io.val(n);
    if (io.loading) {
      if (n >= A2SNAP_SECTOR_MAX) { io.fail("Snapshot is damaged"); return false; }
      outputSectorData.resize(n);
    }
    io.bytes(outputSectorData.data(), n);
  }
  {
    // The dirty track, only when it had not reached the card. It goes back only if the same image
    // is mounted now -- the late save would otherwise write it into a different disk.
    bool pending = trackPendingSave;
    io.flag(pending);
    if (pending) {
      char name[A2SNAP_NAME_LEN] = { 0 };
      if (!io.loading) strncpy(name, selectedDiskFileName.c_str(), A2SNAP_NAME_LEN - 1);
      io.bytes(name, A2SNAP_NAME_LEN);
      io.bytes(trackRawData, A2SNAP_TRACK_RAW);
      if (io.loading) {
        name[A2SNAP_NAME_LEN - 1] = 0;
        char cur[A2SNAP_NAME_LEN] = { 0 };
        strncpy(cur, selectedDiskFileName.c_str(), A2SNAP_NAME_LEN - 1);
        pending = strcmp(name, cur) == 0 && strcmp(cur, "/") != 0 && cur[0];
      }
    }
    if (io.loading && io.ok) {
      if (diskTrack < 0) diskTrack = 0;
      if (pointer < 0 || pointer >= A2SNAP_TRACK_ENC) pointer = 0;
      if (pending) {
        // trackRawData is the snapshot's (newer than the card): rebuild its nibbles here, and the
        // late save (apple2LateSavePoll) writes it back once the motor is off.
        trackRawDataEncode(diskTrack);
        trackChanged = false;
        diskChanged = false;
      } else {
        trackChanged = true;              // next $C0EC re-reads the head's track from the image
      }
      trackPendingSave = pending;
      diskLed(DriveMotorON_OFF ? LOW : HIGH);
    }
  }

  // ---- HD card (slot 7) ----
  io.tag("HDCD");
  io.val(hdCommand);
  io.val(hdStatus);
  io.flag(hdUnitNumber1_2);
  io.val(hdMemoryBuffer);
  io.val(hdBlockNumber);
  if (io.loading) lastBlock = (ushort)-1;  // the open handle is wherever the old session left it

  // ---- Mouse card (slot 4) ----
  io.tag("MOUS");
  io.flag(lastMouseButton);

  return io.ok;
}
