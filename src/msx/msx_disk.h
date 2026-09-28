// msx_disk.h - MSX disk drive for the MSX1 core. The HB3600 disk-BASIC ROM lives in slot 2
// ($4000-$7FF7) and drives a WD2793 FDC the interface memory-maps at $7FF8-$7FFF; the FDC transfers
// 512-byte sectors to/from the mounted .dsk image (see msx_disk.cpp): an in-memory copy written back
// to SD, or (no PSRAM) a streamed image whose sectors go straight to the SD file.
#pragma once
#include <stdint.h>

namespace msx {
void    diskSetRom(const uint8_t* rom, int len);   // install (or null to remove) the disk ROM in slot 2
void    diskSetImage(uint8_t* img, int size);      // mount a .dsk image already in memory
// Mount a .dsk that stays on the SD card (no RAM copy -- the PicoCalc has no PSRAM for a 720K image):
// every sector the FDC reads or writes goes through io(). io returns false on failure (a failed
// write reports write-protect).
typedef bool (*DiskSectorIO)(uint32_t offset, uint8_t* buf512, bool write);
void    diskSetStream(int size, DiskSectorIO io);
void    diskEject();                               // unmount the image (ROM stays if installed)
bool    diskPresent();                             // is a .dsk mounted?
bool    diskRomInstalled();                        // is the disk ROM present in slot 2?
uint8_t diskRead(uint16_t addr);                   // slot-2 read  (disk ROM + WD2793 registers)
void    diskWrite(uint16_t addr, uint8_t v);       // slot-2 write (disk ROM region ignored; FDC at $7FF8+)
#if defined(BOARD_DESKTOP)
uint8_t diskPeek(uint16_t addr);                   // desktop debugger: side-effect-free slot-2 read (no FDC advance)
int     diskTrackCount();                          // desktop debugger: cylinders in the mounted .dsk (rings for the heat map)
#endif
// SD write-back: the FDC marks written sectors dirty; the device layer drains them to the .dsk file.
bool    diskHasDirty();
int     diskTakeDirtySector(const uint8_t** data, uint32_t* offset);   // next dirty sector (clears it); -1 = none
}
