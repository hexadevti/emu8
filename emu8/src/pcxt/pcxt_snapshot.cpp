// Not built for the PicoCalc on an RP2040 (Cortex-M0+): its heap leaves ~75KB of guest RAM, too
// little for DOS ("Configuration too large for memory"). The RP2350 runs it paged (fabgl/pcmem.h);
// see BOARD_HAS_PCXT_CORE in board.h. The RP2040 links against src/picocalc/bigram_stubs.cpp.
#if !(defined(BOARD_PICOCALC) && defined(__ARM_ARCH_6M__))
// pcxt_snapshot.cpp - PC-XT save states (src/shared/snapshot.h). One function, both directions.
//
// Stream after the shared header (all little-endian, fixed width):
//   "PCID"  u8  memory layout (0 = flat 1MB, 1 = paged 4KB)
//           u8  writable-page map, 32 bytes (bit p = guest page p << 12 is RAM)
//           u64 image size per drive (A:, B:, C:, D:; 0 = empty)
//   "MACH" .. "CPU "   chipset + 8086 (Machine::snapshot, pcxt_machine.cpp): port 61h/speaker, CGA
//           6845/mode/colour, adapter state, PIC x2, PIT, 8042 + pending scancodes, RTC/CMOS, BIOS
//           (multi-byte key state, INT 1Eh, media types), CPU registers/flags/IP/prefixes/IRQ/halt
//   "MOUS"  INT 33h driver state (pcxtMouseSnapshot, pcxt.cpp)
//   "RAM "  each writable guest page once (a page mirrored elsewhere is not repeated), ascending;
//           flat builds then add the 64KB video window (0xB0000; paged builds map CGA as pages)
// A different RAM layout or a different hard disk is refused before anything is touched; another
// floppy (or none) is allowed and reported to DOS as a media change. Disk images are never saved.

#include "../../emu.h"
#include "../shared/snapshot.h"
#include "pcxt.h"
#include "fabgl/machine.h"

void pcxtMouseSnapshot(SnapIO& io);   // pcxt.cpp

static void pcSnapMove(void* ctx, void* p, size_t n)    { ((SnapIO*)ctx)->bytes(p, n); }
static void pcSnapMark(void* ctx, const char* t)        { ((SnapIO*)ctx)->tag(t); }

// Host pointer of guest page p (0x00..0xFF) when it is RAM, else nullptr.
static uint8_t* pcSnapPage(int p) {
#if PCXT_PAGED_MEM
  return pcmem::wr[p];
#else
  return pcmem::flat + ((uint32_t)p << 12);
#endif
}

// True if page p is backed by the same memory as a lower page (A20/EBDA/CGA mirrors): saved once.
static bool pcSnapMirror(int p) {
#if PCXT_PAGED_MEM
  for (int q = 0; q < p; q++) if (pcmem::wr[q] == pcmem::wr[p]) return true;
#else
  (void)p;
#endif
  return false;
}

bool pcxtSnapshot(SnapIO& io) {
  if (!g_pcxtMachine.memory()) { io.reject("PC-XT is not running"); return false; }

  // ---- identity (into locals; checked before anything is overwritten) ----
  uint8_t  layout = PCXT_PAGED_MEM ? 1 : 0;
  uint8_t  map[32] = {0};
  uint64_t disk[DISKCOUNT];
  for (int p = 0; p < 256; p++) if (pcSnapPage(p)) map[p >> 3] |= 1 << (p & 7);
  for (int d = 0; d < DISKCOUNT; d++) disk[d] = g_pcxtMachine.disk(d) ? g_pcxtMachine.diskSize(d) : 0;

  io.tag("PCID");
  uint8_t  fLayout = layout, fMap[32];
  uint64_t fDisk[DISKCOUNT];
  memcpy(fMap, map, sizeof(map));
  memcpy(fDisk, disk, sizeof(disk));
  io.val(fLayout);
  io.arr(fMap);
  io.arr(fDisk);
  if (!io.ok) return false;
  bool floppyChanged[DISKCOUNT] = {};
  if (io.loading) {
    if (fLayout != layout || memcmp(fMap, map, sizeof(map)) != 0) {
      io.reject("Snapshot has another RAM size"); return false;
    }
    for (int d = 2; d < DISKCOUNT; d++)
      if (fDisk[d] != disk[d]) { io.reject("Mount the same hard disk first"); return false; }
    for (int d = 0; d < 2; d++) floppyChanged[d] = fDisk[d] != disk[d];
  }

  // ---- chipset + CPU ----
  PcSnap s;
  s.move = pcSnapMove; s.mark = pcSnapMark; s.ctx = &io; s.loading = io.loading;
  g_pcxtMachine.snapshot(s);

  io.tag("MOUS");
  pcxtMouseSnapshot(io);

  // ---- guest memory, streamed page by page straight from / into the guest pages ----
  io.tag("RAM ");
  for (int p = 0; p < 256 && io.ok; p++) {
    uint8_t* pg = pcSnapPage(p);
    if (pg && !pcSnapMirror(p)) io.bytes(pg, 4096);
  }
#if !PCXT_PAGED_MEM
  io.bytes(g_pcxtMachine.videoMemory(), PCXT_VIDEOMEM_SIZE);
#endif

  if (io.loading && io.ok) {
    for (int d = 0; d < 2; d++)
      if (floppyChanged[d]) g_pcxtMachine.markMediaChanged(d);
    pcxtForceRedraw();
  }
  return io.ok;
}
#endif // !(BOARD_PICOCALC && RP2040)
