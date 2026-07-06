// msx_disk.cpp - MSX disk drive: the HB3600 disk ROM (slot 2) + a WD2793 FDC emulated memory-mapped
// at $7FF8-$7FFF (the disk interface decodes the top 8 bytes of the ROM space as FDC registers):
//   $7FF8 command(W)/status(R)   $7FF9 track   $7FFA sector   $7FFB data
//   $7FFC side select (bit0)     $7FFD drive/motor control
// Sectors come from the mounted .dsk (a linear logical-sector image): a Read/Write Sector command
// transfers 512 bytes through the data register with the DRQ status bit, like real hardware. Writes
// update the in-memory image (persist for the session; not yet flushed back to SD). Arduino-free.

#include "msx_disk.h"
#include <string.h>
#if defined(BOARD_DESKTOP)
#include "../desktop/debug_bridge.h"   // desktop debugger: dbgDiskRead/Write feed the disk-read heat map (no-op on device)
#endif

namespace msx {

static const uint8_t* g_diskRom = nullptr;
static int            g_diskRomLen = 0;
static uint8_t*       g_diskImg = nullptr;
static int            g_diskImgSize = 0;
static int            g_sides = 2, g_spt = 9;     // geometry derived from the image size

// ---- WD2793 state -------------------------------------------------------------------------------
static uint8_t  fdcStatus = 0, fdcTrack = 0, fdcSector = 0, fdcData = 0, fdcCmd = 0;
static uint8_t  fdcSide = 0, fdcCtrl = 0;
static uint8_t  fdcBuf[512];
static int      fdcIdx = 0, fdcLen = 0, fdcDir = 0;   // dir: 0 idle, 1 reading, 2 writing
static int      fdcSeekTicks = 0;                     // brief "busy" after a Type I (seek) command
static uint32_t fdcWriteOff = 0;

// Write-back: track which sectors changed so the device layer can flush them to the SD .dsk file.
static uint8_t  g_dirty[1024];                        // 1 bit/sector -> up to 8192 sectors (4 MB)
static bool     g_anyDirty = false;
static int      g_totalSec = 0;
static void markDirty(int sec) {
  if (sec >= 0 && sec < g_totalSec && (sec >> 3) < (int)sizeof(g_dirty)) {
    g_dirty[sec >> 3] |= (uint8_t)(1 << (sec & 7));
    g_anyDirty = true;
  }
}
bool diskHasDirty() { return g_anyDirty; }
int diskTakeDirtySector(const uint8_t** data, uint32_t* offset) {
  if (g_diskImg) {
    int nbytes = (g_totalSec + 7) >> 3;
    if (nbytes > (int)sizeof(g_dirty)) nbytes = sizeof(g_dirty);
    for (int byi = 0; byi < nbytes; byi++) {
      if (!g_dirty[byi]) continue;
      for (int b = 0; b < 8; b++)
        if (g_dirty[byi] & (1 << b)) {
          g_dirty[byi] &= (uint8_t)~(1 << b);
          int sec = byi * 8 + b;
          *offset = (uint32_t)sec * 512;
          *data = g_diskImg + *offset;
          return sec;
        }
    }
  }
  g_anyDirty = false;
  return -1;
}

// The HB3600 interface status at $7FFC-$7FFF that the ROM's transfer loop polls (via a BC pointer):
//   bit6 = BUSY (1 = command running, 0 = done)   bit7 = NOT-DRQ (0 = a data byte is ready)
// The loop does: LD A,(BC); ADD A,A; RET P (bit6==0 -> done); JP C (bit7==1 -> wait); else transfer.
static uint8_t ifaceStatus() {
  if (fdcDir == 1 || fdcDir == 2) return 0x40;               // Type II transfer: busy + DRQ ready
  if (fdcSeekTicks > 0) { fdcSeekTicks--; return 0xC0; }     // Type I settling: busy, no DRQ
  return 0x00;                                               // idle / command complete
}
// status bits: BUSY 0x01, DRQ 0x02, (TRACK00/LOST) 0x04, CRC 0x08, RNF/SEEKERR 0x10, WP 0x40, NOTRDY 0x80

void diskSetRom(const uint8_t* rom, int len) { g_diskRom = rom; g_diskRomLen = len; fdcStatus = 0; fdcDir = 0; }
void diskSetImage(uint8_t* img, int size) {
  g_diskImg = img; g_diskImgSize = size;
  g_totalSec = size / 512;
  g_spt = 9;
  g_sides = (g_totalSec > 80 * 9) ? 2 : 1;       // 720K = 2 sides, 360K = 1
  memset(g_dirty, 0, sizeof(g_dirty)); g_anyDirty = false;
}
void diskEject() { g_diskImg = nullptr; g_diskImgSize = 0; fdcDir = 0; memset(g_dirty, 0, sizeof(g_dirty)); g_anyDirty = false; }
bool diskPresent() { return g_diskImg != nullptr; }
bool diskRomInstalled() { return g_diskRom != nullptr; }

static uint32_t sectorOffset(int track, int side, int sector) {
  return (uint32_t)((track * g_sides + side) * g_spt + (sector - 1)) * 512;   // linear logical sector
}

static void fdcCommand(uint8_t v) {
  fdcCmd = v;
  uint8_t hi = v & 0xF0;
  if (hi == 0xD0) { fdcStatus &= ~(0x01 | 0x02); fdcDir = 0; return; }        // Force Interrupt (Type IV)
  if (v < 0x80) {                                                              // Type I: Restore/Seek/Step
    if      (hi == 0x00) fdcTrack = 0;                                         // Restore
    else if (hi == 0x10) fdcTrack = fdcData;                                   // Seek to data register
    else if (hi == 0x40 || hi == 0x50) fdcTrack++;                             // Step In
    else if (hi == 0x60 || hi == 0x70) { if (fdcTrack) fdcTrack--; }           // Step Out
    fdcStatus = (fdcTrack == 0) ? 0x04 : 0x00;                                 // TRACK00, not busy
    fdcSeekTicks = 4;                                                          // brief busy on $7FFF (seek wait)
    return;
  }
  if (hi == 0x80 || hi == 0x90) {                                             // Read Sector
    uint32_t off = sectorOffset(fdcTrack, fdcSide, fdcSector);
    if (!g_diskImg || off + 512 > (uint32_t)g_diskImgSize) { fdcStatus = 0x10; fdcDir = 0; return; }  // RNF
    memcpy(fdcBuf, g_diskImg + off, 512);
    fdcIdx = 0; fdcLen = 512; fdcDir = 1; fdcStatus = 0x03;                    // BUSY | DRQ
#if defined(BOARD_DESKTOP)
    dbgDiskRead(fdcTrack, fdcSide * g_spt + (fdcSector - 1));                  // heat map: ring=cylinder, wedge=side*spt+sector
#endif
    return;
  }
  if (hi == 0xA0 || hi == 0xB0) {                                             // Write Sector
    uint32_t off = sectorOffset(fdcTrack, fdcSide, fdcSector);
    if (!g_diskImg || off + 512 > (uint32_t)g_diskImgSize) { fdcStatus = 0x10; fdcDir = 0; return; }
    fdcWriteOff = off; fdcIdx = 0; fdcLen = 512; fdcDir = 2; fdcStatus = 0x03; // BUSY | DRQ
#if defined(BOARD_DESKTOP)
    dbgDiskWrite(fdcTrack, fdcSide * g_spt + (fdcSector - 1));                 // heat map (write channel = red)
#endif
    return;
  }
  if (hi == 0xC0) {                                                           // Read Address (ID field)
    fdcBuf[0] = fdcTrack; fdcBuf[1] = fdcSide; fdcBuf[2] = (fdcSector < 1) ? 1 : fdcSector;
    fdcBuf[3] = 0x02; fdcBuf[4] = 0; fdcBuf[5] = 0;                            // len code 2 = 512, dummy CRC
    fdcSector = fdcTrack;                                                      // WD2793 copies track->sector reg
    fdcIdx = 0; fdcLen = 6; fdcDir = 1; fdcStatus = 0x03;
    return;
  }
  fdcStatus = 0; fdcDir = 0;                                                   // Read/Write Track: succeed (no-op)
}

uint8_t diskRead(uint16_t addr) {
  if (!g_diskRom) return 0xFF;
  if (addr >= 0x7FF8 && addr <= 0x7FFF) {                                      // memory-mapped WD2793
    switch (addr & 7) {
      case 0: return fdcStatus;                                               // status
      case 1: return fdcTrack;
      case 2: return fdcSector;
      case 3:                                                                 // data register
        if (fdcDir == 1) {
          uint8_t b = (fdcIdx < fdcLen) ? fdcBuf[fdcIdx] : 0xFF;
          if (fdcIdx < fdcLen) fdcIdx++;
          if (fdcIdx >= fdcLen) { fdcStatus = 0; fdcDir = 0; }                // transfer done: clear status
          return b;
        }
        return fdcData;
      default: return ifaceStatus();                                         // $7FFC-$7FFF interface status
    }
  }
  if (addr >= 0x4000 && addr <= 0x7FF7) return g_diskRom[addr - 0x4000];      // disk ROM (page 1 only)
  return 0xFF;
}

void diskWrite(uint16_t addr, uint8_t v) {
  if (!g_diskRom) return;
  if (addr < 0x7FF8 || addr > 0x7FFF) return;                                 // ROM writes ignored
  switch (addr & 7) {
    case 0: fdcCommand(v); break;                                            // command
    case 1: fdcTrack = v; break;
    case 2: fdcSector = v; break;
    case 3:                                                                   // data register
      fdcData = v;
      if (fdcDir == 2) {
        if (fdcIdx < fdcLen) fdcBuf[fdcIdx++] = v;
        if (fdcIdx >= fdcLen) {
          if (g_diskImg && fdcWriteOff + 512 <= (uint32_t)g_diskImgSize) {
            memcpy(g_diskImg + fdcWriteOff, fdcBuf, 512);                     // update the in-memory image
            markDirty((int)(fdcWriteOff / 512));                              // queue this sector for SD write-back
          }
          fdcStatus = 0; fdcDir = 0;
        }
      }
      break;
    case 4: fdcSide = v & 1; fdcCtrl = v; break;                              // side select (bit0)
    case 5: fdcCtrl = v; break;                                              // drive/motor
  }
}

#if defined(BOARD_DESKTOP)
// Side-effect-free read for the desktop memory viewer/disassembler: returns the disk ROM and the
// current FDC register values WITHOUT advancing the data transfer, clearing status, or ticking the
// seek timer (diskRead does all of those, which would corrupt a running disk access).
uint8_t diskPeek(uint16_t addr) {
  if (!g_diskRom) return 0xFF;
  if (addr >= 0x7FF8 && addr <= 0x7FFF) {
    switch (addr & 7) {
      case 0: return fdcStatus;
      case 1: return fdcTrack;
      case 2: return fdcSector;
      case 3: return (fdcDir == 1 && fdcIdx < fdcLen) ? fdcBuf[fdcIdx] : fdcData;   // no fdcIdx advance
      default:                                                                      // interface status, no tick
        if (fdcDir == 1 || fdcDir == 2) return 0x40;
        if (fdcSeekTicks > 0) return 0xC0;
        return 0x00;
    }
  }
  if (addr >= 0x4000 && addr <= 0x7FF7) return g_diskRom[addr - 0x4000];
  return 0xFF;
}
// Cylinder count of the mounted .dsk for the debugger's disk-read heat map (rings = cylinders;
// wedges = side*spt + sector). 720K = 80, 360K = 40; 0 when nothing is mounted.
int diskTrackCount() {
  int per = g_sides * g_spt;
  return (g_diskImg && per > 0) ? (g_totalSec / per) : 0;
}
#endif

} // namespace msx
