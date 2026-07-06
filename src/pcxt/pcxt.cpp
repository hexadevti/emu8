// pcxt.cpp - device-side glue for the PC-XT (Intel 8086) platform: memory
// allocation, the core-1 run loop, the core-0 CGA render push, USB-keyboard ->
// XT scancode injection, and the settings (disk browser / mount) hooks. This is
// the only PC-XT file that pulls in emu.h (Arduino/board); the machine core in
// src/pcxt/fabgl/ stays host-portable. Mirrors sms.cpp.
//
// Boot path: the BIOS ROM (embedded in src/pcxt/fabgl/biosrom.h, installed by
// BIOS::init) runs from reset; POST text appears in the CGA buffer (rendered
// here) before any disk is touched.

#include "../../emu.h"
#include "pcxt.h"
#include "fabgl/machine.h"
#include <dirent.h>
#include <string>

// 1 MB main RAM (PSRAM) + 64 KB CGA/video window (internal: read every frame).
static uint8_t* pcRam   = nullptr;
static uint8_t* pcVRam  = nullptr;
static volatile bool pcResetReq = false;
static bool pcInitDone = false;

#if defined(BOARD_DESKTOP)
// Desktop emulator framebuffer for PC-XT: 640x400 (VGA-ish 80x25 @ 8x16 cells). Big enough to render
// the authentic IBM 8x8 font crisply (80 cols x 8px = 640) instead of squishing it into 320 px.
#define PCXT_DESK_W 640
#define PCXT_DESK_H 400
void desktopSetEmuResolution(int w, int h);   // display_sdl.cpp — size the fb before begin()
#endif

static uint8_t* pcAllocFast(size_t n) {                 // internal SRAM first, PSRAM fallback
  uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t*)ps_malloc(n);
  return p;
}

// Read /roms/pcxt/bios.bin off the SD card into a malloc'd buffer (kept for the session). Defined
// here because the fabgl BIOS TU (fabgl/bios.cpp, GPLv3) doesn't include emu.h / the Arduino SD API;
// BIOS::init + pcBiosFont8x8 call this to fetch what used to be the embedded `biosrom` array.
uint8_t* pcxtReadBiosRom(size_t* outLen) {
  *outLen = 0;
  busTake();
  File f = FSTYPE.open("/roms/pcxt/bios.bin", FILE_READ);
  int len = f ? (int)f.size() : 0;
  if (!f || len <= 0 || len > 64 * 1024) {
    if (f) f.close(); busGive();
    printLog("PC-XT: /roms/pcxt/bios.bin missing or bad size - cannot boot");
    return nullptr;
  }
  uint8_t* b = (uint8_t*)ps_malloc(len);
  if (!b) b = (uint8_t*)malloc(len);
  if (!b) { f.close(); busGive(); printLog("PC-XT: BIOS alloc failed"); return nullptr; }
  int rd = 0;
  while (rd < len) { int n = f.read(b + rd, (len - rd > 8192) ? 8192 : (len - rd)); if (n <= 0) break; rd += n; }
  f.close();
  busGive();
  if (rd != len) { free(b); return nullptr; }
  *outLen = (size_t)len;
  sprintf(buf, "PC-XT: BIOS loaded from /roms/pcxt/bios.bin (%d bytes)", len); printLog(buf);
  return b;
}
static bool pcEndsCI(const std::string& s, const char* suf) {
  size_t n = strlen(suf); if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i])) return false;
  return true;
}
static bool pcIsDiskImage(const std::string& s) {
  return pcEndsCI(s, ".img") || pcEndsCI(s, ".ima") || pcEndsCI(s, ".dsk") ||
         pcEndsCI(s, ".vhd") || pcEndsCI(s, ".hdd");
}

// A real PC disk image has the 0x55AA boot signature at offset 510 (FAT boot sector / MBR). The
// user's IIgs/Apple .img/.dsk images lack it, so this keeps them OUT of the PC browser -> they can't
// be mounted by accident and corrupted by write-back. busTake() guards the HSPI (touch shares it).
static bool pcLooksLikePcDisk(const char* path) {
  bool ok = false;
  busTake();
  File f = FSTYPE.open(path, FILE_READ);
  if (f) {
    if (f.size() >= 512 && f.seek(510)) {
      uint8_t s[2] = {0, 0};
      if (f.read(s, 2) == 2) ok = (s[0] == 0x55 && s[1] == 0xAA);
    }
    f.close();
  }
  busGive();
  return ok;
}

// Scan SD root for PC disk images into pcFiles (extension match + 0x55AA boot signature).
#define PCXT_MAX_FILES 200
void loadPcxtFilesSync() {
  pcFiles.clear();
  pcFiles.reserve(PCXT_MAX_FILES);
  DIR* dp = opendir(SD_VFS_ROOT);
  if (dp) {
    struct dirent* de; int scanned = 0;
    while ((de = readdir(dp)) != nullptr) {
      if (de->d_type == DT_DIR) continue;
      std::string nm = de->d_name;
      if (pcIsDiskImage(nm)) {
        std::string full = std::string("/") + nm;
        if (pcLooksLikePcDisk(full.c_str())) pcFiles.push_back(full);
      }
      if ((++scanned & 0x3f) == 0) ::uiDirScanProgress((int)pcFiles.size());
      if ((int)pcFiles.size() >= PCXT_MAX_FILES) break;
    }
    closedir(dp);
  }
  sprintf(buf, "PCXT: %d PC disk image(s) on SD root", (int)pcFiles.size());
  printLog(buf);
}

// ---- SD/File disk backend ----
// The stdio FILE* path (fopen/fseek/ftell over the ESP32 SD VFS) reported size 0 and
// unreliable seeks, so disk images go through the Arduino SD File API (card-relative
// paths like "/dos.img"), which has reliable size()/seek()/read()/write(). Called from
// the BIOS INT 13h handler, already wrapped in busTake/busGive by the machine.
static File pcDiskPool[4];
static bool pcRwSafe = false;                  // set by the setup probe: is "r+" non-truncating here?

static void* pcDiskOpen(const char* path, uint64_t* sizeOut) {
  int slot = -1;
  for (int i = 0; i < 4; i++) if (!pcDiskPool[i]) { slot = i; break; }
  if (slot < 0) return nullptr;
  File f = pcRwSafe ? FSTYPE.open(path, "r+")   // read+write (no truncate) when probe says it's safe
                    : FSTYPE.open(path, FILE_READ);
  if (!f) f = FSTYPE.open(path, FILE_READ);     // fall back to read-only if r+ is refused
  if (!f) { sprintf(buf, "pcDiskOpen: open FAILED %s", path); printLog(buf); return nullptr; }
  pcDiskPool[slot] = f;
  if (sizeOut) *sizeOut = (uint64_t)pcDiskPool[slot].size();
  return &pcDiskPool[slot];
}

static int pcDiskIo(void* ctx, uint64_t pos, uint8_t* bufp, uint32_t count, bool write) {
  if (!ctx) return 0;
  File* f = (File*)ctx;
  if (!f->seek((uint32_t)pos)) return 0;
  if (write) { int n = (int)f->write(bufp, count); f->flush(); return n; }  // flush so writes persist
  return (int)f->read(bufp, count);
}

static void pcDiskClose(void* ctx) {
  if (ctx) ((File*)ctx)->close();
}

// Does the disk in `drive` contain a bootable DOS system (IO.SYS/IBMBIO.COM in the root dir)?
// Reads the FAT boot sector + first root-dir sector through the already-open backend handle.
static bool pcDriveIsBootable(int drive) {
  if (!g_pcxtMachine.disk(drive)) return false;
  bool ok = false;
  busTake();
  uint8_t bs[512];
  if (g_pcxtMachine.diskRead(drive, 0, bs, 512) == 512 && bs[510] == 0x55 && bs[511] == 0xAA) {
    uint16_t bps  = bs[11] | (bs[12] << 8);
    uint8_t  nfat = bs[16];
    uint16_t rsvd = bs[14] | (bs[15] << 8);
    uint16_t rent = bs[17] | (bs[18] << 8);
    uint16_t spf  = bs[22] | (bs[23] << 8);
    if (bps == 512 && nfat >= 1 && rent > 0 && spf > 0) {
      uint32_t rootSec = rsvd + (uint32_t)nfat * spf;        // FAT12/16 root dir follows the FATs
      uint8_t rd[512];
      if (g_pcxtMachine.diskRead(drive, (uint64_t)rootSec * 512, rd, 512) == 512) {
        for (int e = 0; e < 16; e++) {
          const uint8_t* d = rd + e * 32;
          if (d[0] == 0x00) break;          // end of directory
          if (d[0] == 0xE5) continue;       // deleted entry
          if (!memcmp(d, "IO      SYS", 11) || !memcmp(d, "IBMBIO  COM", 11) ||
              !memcmp(d, "MSDOS   SYS", 11) || !memcmp(d, "IBMDOS  COM", 11)) { ok = true; break; }
        }
      }
    }
  }
  busGive();
  return ok;
}

// Choose the boot drive: a bootable system floppy in A: wins; otherwise a hard disk in C:; otherwise
// whatever floppy is there. So a NON-system disk in A: with a hard disk present auto-boots C:.
static void pcUpdateBootDrive() {
  int bd;
  if (g_pcxtMachine.disk(0) && pcDriveIsBootable(0)) bd = 0;        // bootable floppy -> A:
  else if (g_pcxtMachine.disk(2))                    bd = 2;        // else a hard disk -> C:
  else                                               bd = 0;        // else A: (or nothing)
  g_pcxtMachine.setBootDrive(bd);
}

// Mount `path` into a specific drive slot (0 = A: floppy, 2 = C: hard disk), WITHOUT rebooting.
// Updates that slot's saved name and the boot order (floppy first if present, else hard disk).
// A: media changes are seen live by DOS; a newly-added C: is recognised only after a reboot.
static bool pcMountInto(const char* path, int slot) {
  if (!pcInitDone) return false;
  std::string p = path ? path : "";
  if (!pcIsDiskImage(p)) { printLog("PCXT: unsupported file (.img/.ima/.dsk/.vhd)"); return false; }
  if (!pcLooksLikePcDisk(path)) { sprintf(buf, "PCXT: %s is not a PC disk (no 55AA) - refused", path); printLog(buf); return false; }
  g_pcxtMachine.setDriveImage(slot, path);
  if (slot == 0) selectedPcFileName = path; else selectedPcHdFileName = path;
  pcUpdateBootDrive();   // floppy first, else boot the hard disk
  sprintf(buf, "PCXT: mounted %s into %s", path, slot == 0 ? "A:" : "C:");
  printLog(buf);
  return true;
}

// ---- PC mouse: the emulator serves INT 33h directly from the USB mouse (no MOUSE.COM / PS2 HW) ----
// INT 33h is THE standard DOS mouse API; apps use it either by polling (fn 3) or by registering an
// event handler (fn 0Ch) that the driver must FAR-CALL on each event. QBASIC uses fn 0Ch, so besides
// answering polls we inject a far-call into the app's handler from the per-instruction step hook.
static volatile int  pcMouseX = 320, pcMouseY = 100;     // virtual coords (0..639 x, 0..199 y)
static volatile int  pcMouseBtns = 0;                    // bit0=left, bit1=right, bit2=middle
static volatile int  pcMouseAccX = 0, pcMouseAccY = 0;   // raw motion since the last fn 0x0B
static volatile bool pcMouseShown = false;
static int pcMouseMinX = 0, pcMouseMaxX = 639, pcMouseMinY = 0, pcMouseMaxY = 199;
// latched button press/release counts + positions (INT 33h fn 5/6)
static volatile int pcBtnPrev = 0;
static volatile int pcBtnPressCnt[3] = {0,0,0}, pcBtnRelCnt[3] = {0,0,0};
static volatile int pcBtnPressX[3] = {0,0,0}, pcBtnPressY[3] = {0,0,0};
static volatile int pcBtnRelX[3]   = {0,0,0}, pcBtnRelY[3]   = {0,0,0};
// INT 33h fn 0Ch event handler (the app's far procedure, called by us on matching events)
static volatile uint16_t pcCbSeg = 0, pcCbOff = 0, pcCbMask = 0;  // handler seg:off + event mask
static volatile uint16_t pcEvCond = 0;                            // pending event condition bits
static volatile bool     pcEvPending = false;
static bool     pcInCb    = false;                               // currently running an injected callback
static uint16_t pcCbRetCS = 0, pcCbRetIP = 0;                    // return CS:IP, to detect the RETF
static uint16_t pcSavAX, pcSavBX, pcSavCX, pcSavDX, pcSavSI, pcSavDI, pcSavBP, pcSavDS, pcSavES, pcSavFL; // full ctx saved across the call

static void pcMouseClearCb() { pcCbSeg = pcCbOff = pcCbMask = 0; pcEvPending = false; pcEvCond = 0; pcInCb = false; }

// Called from the USB host with relative movement + button bitmap.
void pcxtMouseInput(int dx, int dy, uint8_t buttons) {
  if (currentPlatform != PLATFORM_PCXT) return;
  int nx = pcMouseX + dx * 2, ny = pcMouseY + dy * 2;    // ~2 px per mouse unit
  if (nx < pcMouseMinX) nx = pcMouseMinX; else if (nx > pcMouseMaxX) nx = pcMouseMaxX;
  if (ny < pcMouseMinY) ny = pcMouseMinY; else if (ny > pcMouseMaxY) ny = pcMouseMaxY;
  pcMouseX = nx; pcMouseY = ny;
  pcMouseAccX += dx * 2; pcMouseAccY += dy * 2;
  int nb = buttons & 0x07, prev = pcBtnPrev;
  uint16_t cond = (dx || dy) ? 0x01 : 0;                 // INT 33h event condition mask
  for (int b = 0; b < 3; b++) {                          // latch press/release transitions
    int m = 1 << b;
    if ((nb & m) && !(prev & m)) { pcBtnPressCnt[b]++; pcBtnPressX[b] = nx; pcBtnPressY[b] = ny; cond |= 0x02 << (b * 2); }
    if (!(nb & m) && (prev & m)) { pcBtnRelCnt[b]++;   pcBtnRelX[b]   = nx; pcBtnRelY[b]   = ny; cond |= 0x04 << (b * 2); }
  }
  pcBtnPrev = nb;
  pcMouseBtns = nb;
  uint16_t masked = cond & pcCbMask;                     // queue an event for the app's fn 0Ch handler
  if (masked && (pcCbSeg || pcCbOff)) { pcEvCond |= masked; pcEvPending = true; }
}

// Per-instruction hook (Machine::run): inject a far-call to the app's INT 33h event handler when an
// event is pending. Mirrors how a real mouse driver calls the handler from its hardware IRQ.
static uint32_t pcCbAge = 0;   // instructions since the current callback was injected (stuck-watchdog)
static void pcMouseStepHook() {
  if (!pcCbSeg && !pcCbOff) return;                      // no handler installed -> nothing to do
  if (pcInCb) {                                          // waiting for the injected handler to RETF
    if (fabgl::i8086::CS() == pcCbRetCS && fabgl::i8086::IP() == pcCbRetIP) {
      // handler returned: restore the FULL interrupted-code context. A real mouse driver protects the
      // caller across the callback (pusha/popa for GP regs + saved DS/ES + IRET-restored FLAGS), so the
      // app's handler may freely trash any register. Mirror that or the interrupted code corrupts.
      fabgl::i8086::setAX(pcSavAX); fabgl::i8086::setBX(pcSavBX); fabgl::i8086::setCX(pcSavCX);
      fabgl::i8086::setDX(pcSavDX); fabgl::i8086::setSI(pcSavSI); fabgl::i8086::setDI(pcSavDI);
      fabgl::i8086::setBP(pcSavBP); fabgl::i8086::setDS(pcSavDS); fabgl::i8086::setES(pcSavES);
      fabgl::i8086::setFlagsWord(pcSavFL);
      pcInCb = false;
    }
    else if (++pcCbAge > 300000) pcInCb = false;         // watchdog: never lock out if a return is missed
    return;
  }
  if (!pcEvPending) return;
  if (!fabgl::i8086::flagIF()) return;                   // inject only with interrupts enabled (IRQ-like)
  if (fabgl::i8086::CS() == 0xF000) return;              // not while inside the magic-interrupt BIOS

  uint16_t cond = pcEvCond; pcEvCond = 0; pcEvPending = false;
  pcCbAge = 0;
  uint8_t * mem  = g_pcxtMachine.memory();
  uint32_t  ssb  = (uint32_t)fabgl::i8086::SS() << 4;
  uint16_t  sp   = fabgl::i8086::SP();
  uint16_t  curCS = fabgl::i8086::CS(), curIP = fabgl::i8086::IP();
  // push CS then IP (far-call frame; the handler ends with RETF, returning to curCS:curIP)
  sp -= 2; { uint32_t a = (ssb + sp) & 0xFFFFF; mem[a] = curCS & 0xFF; mem[(a + 1) & 0xFFFFF] = curCS >> 8; }
  sp -= 2; { uint32_t a = (ssb + sp) & 0xFFFFF; mem[a] = curIP & 0xFF; mem[(a + 1) & 0xFFFFF] = curIP >> 8; }
  fabgl::i8086::setSP(sp);
  pcCbRetCS = curCS; pcCbRetIP = curIP;
  // save the FULL interrupted-code context so we can restore it when the handler returns
  pcSavAX = fabgl::i8086::AX(); pcSavBX = fabgl::i8086::BX(); pcSavCX = fabgl::i8086::CX();
  pcSavDX = fabgl::i8086::DX(); pcSavSI = fabgl::i8086::SI(); pcSavDI = fabgl::i8086::DI();
  pcSavBP = fabgl::i8086::BP(); pcSavDS = fabgl::i8086::DS(); pcSavES = fabgl::i8086::ES();
  pcSavFL = fabgl::i8086::flagsWord();
  // event registers per the INT 33h callback ABI
  fabgl::i8086::setAX(cond);
  fabgl::i8086::setBX(pcMouseBtns);
  fabgl::i8086::setCX(pcMouseX);
  fabgl::i8086::setDX(pcMouseY);
  fabgl::i8086::setDI((uint16_t)(int16_t)pcMouseAccY);   // mickeys (best-effort; SI not settable)
  fabgl::i8086::setCS(pcCbSeg);                          // jump into the app's handler
  fabgl::i8086::setIP(pcCbOff);
  pcInCb = true;
}

// INT 33h service (AX = function). Returns true (always handled).
static bool pcInt33Service() {
  switch (fabgl::i8086::AX()) {
    case 0x00:  // reset driver + get status
      fabgl::i8086::setAX(0xFFFF); fabgl::i8086::setBX(2);
      pcMouseShown = false; pcMouseX = 320; pcMouseY = 100; pcMouseBtns = 0; pcBtnPrev = 0;
      pcMouseMinX = 0; pcMouseMaxX = 639; pcMouseMinY = 0; pcMouseMaxY = 199;
      for (int b = 0; b < 3; b++) { pcBtnPressCnt[b] = 0; pcBtnRelCnt[b] = 0; }
      pcMouseClearCb();
      return true;
    case 0x01: pcMouseShown = true;  return true;    // show cursor
    case 0x02: pcMouseShown = false; return true;    // hide cursor
    case 0x03:  // get position + buttons
      fabgl::i8086::setCX(pcMouseX); fabgl::i8086::setDX(pcMouseY); fabgl::i8086::setBX(pcMouseBtns);
      return true;
    case 0x04:  // set position
      pcMouseX = fabgl::i8086::CX(); pcMouseY = fabgl::i8086::DX(); return true;
    case 0x05: {  // button press info: BX in = button (0=L,1=R,2=M); returns press count + last-press pos
      int b = fabgl::i8086::BX() & 3; if (b > 2) b = 0;
      fabgl::i8086::setAX(pcMouseBtns); fabgl::i8086::setBX(pcBtnPressCnt[b]);
      fabgl::i8086::setCX(pcBtnPressX[b]); fabgl::i8086::setDX(pcBtnPressY[b]);
      pcBtnPressCnt[b] = 0; return true;
    }
    case 0x06: {  // button release info
      int b = fabgl::i8086::BX() & 3; if (b > 2) b = 0;
      fabgl::i8086::setAX(pcMouseBtns); fabgl::i8086::setBX(pcBtnRelCnt[b]);
      fabgl::i8086::setCX(pcBtnRelX[b]); fabgl::i8086::setDX(pcBtnRelY[b]);
      pcBtnRelCnt[b] = 0; return true;
    }
    case 0x07: pcMouseMinX = fabgl::i8086::CX(); pcMouseMaxX = fabgl::i8086::DX(); return true; // X range
    case 0x08: pcMouseMinY = fabgl::i8086::CX(); pcMouseMaxY = fabgl::i8086::DX(); return true; // Y range
    case 0x0B:  // read motion counters (mickeys since last call)
      fabgl::i8086::setCX((uint16_t)(int16_t)pcMouseAccX);
      fabgl::i8086::setDX((uint16_t)(int16_t)pcMouseAccY);
      pcMouseAccX = 0; pcMouseAccY = 0; return true;
    case 0x0C:  // install event handler: CX = event mask, ES:DX = handler far address
      pcCbMask = fabgl::i8086::CX();
      pcCbSeg  = fabgl::i8086::ES();
      pcCbOff  = fabgl::i8086::DX();
      pcEvPending = false; pcEvCond = 0; pcInCb = false;
      return true;
    case 0x21:  // software reset
      fabgl::i8086::setAX(0x21FF); fabgl::i8086::setBX(2);
      pcMouseClearCb();
      return true;
    default: return true;   // ack everything else so no call ever vectors to a null handler
  }
}

// ============================ platform entry points =============================================
void pcxtSetup() {
  printLog("PCXT Setup... (Intel 8086 + CGA, BIOS = 8086tiny-plus)");

  menuScreen = (unsigned char*)malloc(0x546);
  menuColor  = (unsigned char*)malloc(0x546);

#if defined(BOARD_DESKTOP)
  desktopSetEmuResolution(PCXT_DESK_W, PCXT_DESK_H);     // render CGA text/graphics at native PC res
#endif

  pcRam  = (uint8_t*)ps_malloc(PCXT_RAM_SIZE);            // 1 MB main RAM -> PSRAM
  pcVRam = pcAllocFast(PCXT_VIDEOMEM_SIZE);               // 64 KB video RAM -> internal preferred
  if (!pcRam || !pcVRam) {
    sprintf(buf, "PCXT: ALLOC FAIL ram=%p vram=%p (need 1MB PSRAM + 64KB)", pcRam, pcVRam);
    printLog(buf);
    return;
  }

  g_pcxtMachine.setMemoryBuffers(pcRam, pcVRam);
  g_pcxtMachine.setBootDrive(0);                          // floppy A: by default
  g_pcxtMachine.init();                                   // installs BIOS ROM, wires chipset
  g_pcxtMachine.setDiskBackend(pcDiskOpen, pcDiskIo, pcDiskClose);   // SD/File disk backend
  // disk INT 13h runs on core 1; serialize SD/HSPI against core-0 touch/render
  g_pcxtMachine.setDiskLock([]{ busTake(); }, []{ busGive(); });
  // PC-speaker: mirror PIT ch2 freq + port-0x61 gate into globals the audio ISR reads (speaker.cpp)
  g_pcxtMachine.setSpeakerCallback([](int freq, bool on){ g_pcSpkFreq = freq; g_pcSpkOn = on; });
  g_pcxtMachine.setInt33Handler(pcInt33Service);   // mouse: serve INT 33h from the USB mouse
  g_pcxtMachine.setStepHook(pcMouseStepHook);      // mouse: inject the fn 0Ch event-handler far-call
  pcInitDone = true;

  // Probe whether the SD library's "r+" preserves the file size (does NOT truncate). Only then enable
  // write-back to disk images (so DOS can save/install); otherwise keep them read-only to protect data.
  // Uses a throwaway temp file, so no risk to user images.
  {
    const char* tp = "/pcxt_wt.tmp";
    File w = FSTYPE.open(tp, FILE_WRITE);
    if (w) {
      uint8_t z[8] = {1,2,3,4,5,6,7,8};
      w.write(z, 8); w.close();
      File rw = FSTYPE.open(tp, "r+");
      pcRwSafe = (rw && rw.size() == 8);
      if (rw) rw.close();
      FSTYPE.remove(tp);
    }
    printLog(pcRwSafe ? "PCXT: disk write-back ENABLED (r+ is non-truncating)"
                      : "PCXT: disk READ-ONLY (r+ truncates on this SD)");
  }

#if defined(BOARD_DESKTOP)
  // Desktop: choose the boot image via env (mirrors tiny386's EMU_T386_HDA/FDA) so a fresh run with no
  // persisted EEPROM selection can still boot straight into a disk. EMU_PCXT_A = A: floppy, EMU_PCXT_C = C:.
  if (const char* a = getenv("EMU_PCXT_A")) selectedPcFileName   = a;
  if (const char* c = getenv("EMU_PCXT_C")) selectedPcHdFileName = c;
#endif
  if (selectedPcFileName.length() > 1 && selectedPcFileName != "/")
    pcMountInto(selectedPcFileName.c_str(), 0);     // A: floppy
  if (selectedPcHdFileName.length() > 1 && selectedPcHdFileName != "/")
    pcMountInto(selectedPcHdFileName.c_str(), 2);   // C: hard disk
  pcUpdateBootDrive();

  sprintf(buf, "PCXT ready. internal free=%u, spiram free=%u",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  printLog(buf);
}

// The 8086 runs continuously; the PIT (real-time via FRC1Timer) drives IRQ0 at
// 18.2 Hz independent of emulation speed, so there is no fixed frame to pace. We
// just run the CPU in chunks and yield so core 0 can render and the watchdog is
// fed. A one-time boot benchmark reports the effective 8086 speed.
void pcxtLoop() {
  if (!pcInitDone) { for (;;) vTaskDelay(pdMS_TO_TICKS(200)); }

  // uncapped benchmark (~0.5 s real time)
  {
    uint32_t t0 = millis();
    uint64_t iters = 0;
    while ((uint32_t)(millis() - t0) < 500) { g_pcxtMachine.run(20000); iters += 20000; }
    uint32_t dt = millis() - t0;
    double instrPerSec = dt > 0 ? (double)iters / ((double)dt / 1000.0) : 0.0;
    // A real 4.77MHz 8088 averages ~12 cycles/instruction -> ~400 kIPS. Report throughput relative to that.
    double xtRatio = instrPerSec / 400000.0;
    pcMeasuredMhz = (float)(xtRatio * 4.77);
    sprintf(buf, "PCXT: %.0f kIPS = ~%.1f MHz-equiv (%.0f%% of a real 4.77MHz XT)",
            instrPerSec / 1000.0, pcMeasuredMhz, xtRatio * 100.0);
    printLog(buf);
  }

  for (;;) {
    if (OptionsWindow) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    if (pcResetReq)    { pcResetReq = false; g_pcxtMachine.trigReset(); }
#if defined(BOARD_DESKTOP)
    g_pcxtMachine.run(400000);   // desktop: bigger batch — the device's 1ms-per-40k throttle (for WDT/
                                 // bus sharing) just slows the 8086 here (render/input are other threads)
#else
    g_pcxtMachine.run(40000);
#endif
    // Give INT 33h a non-null vector so DOS programs (e.g. QBASIC) detect a mouse. The emulator
    // intercepts the actual INT 33h, so this target is never executed. Set only when null (the POST
    // clears the IVT at boot); leave a real driver's vector alone if one is ever installed.
    uint8_t* mem = g_pcxtMachine.memory();
    if (mem && mem[0xCC] == 0 && mem[0xCD] == 0 && mem[0xCE] == 0 && mem[0xCF] == 0) {
      mem[0xCC] = 0x33; mem[0xCD] = 0x00; mem[0xCE] = 0x00; mem[0xCF] = 0xF0;   // IVT[0x33] = F000:0033
    }
#if defined(BOARD_DESKTOP)
    taskYIELD();             // desktop: cooperative yield (separate render/input threads) — run near full speed
#else
    vTaskDelay(1);          // feed WDT, let core 0 render
#endif
  }
}

// ---- CGA text render (M1: 80x25 / 40x25 text only; graphics modes come in M3) ----
// Reads the CGA text buffer at videoMemory()+0x8000 (char at even byte, attribute
// odd) and blits with the built-in 6x8 font, like iigsRenderText. 80 cols * 6px =
// 480px = full native panel width.
static const uint16_t kCgaRgb565[16] = {
  0x0000, 0x0015, 0x0540, 0x0555, 0xA800, 0xA815, 0xAAA0, 0xAD55,
  0x52AA, 0x52BF, 0x57EA, 0x57FF, 0xFAAA, 0xFABF, 0xFFEA, 0xFFFF
};

static uint16_t* pcGfxScratch = nullptr;   // 320x8 RGB565 band for graphics modes

// ---- CGA graphics render (M3): 320x200x4 and 640x200x2 ----
// CGA graphics memory is interleaved: even scanlines at 0xB8000, odd at 0xBA000 (+0x2000), each
// bank 100 lines of 80 bytes. We decode straight to RGB565 8-line bands and push them centered
// in the 320-logical video rect (fill-screen scales to the panel), like the C64 path.
static void pcxtRenderGraphics(fabgl::GraphicsAdapter::Emulation emu) {
  if (!pcGfxScratch) pcGfxScratch = (uint16_t*)malloc(320 * 8 * sizeof(uint16_t));
  if (!pcGfxScratch) return;
  fabgl::GraphicsAdapter* ga = g_pcxtMachine.graphicsAdapter();
  const uint8_t* vm = g_pcxtMachine.videoMemory() + 0x8000;
  bool mode640 = (emu == fabgl::GraphicsAdapter::Emulation::PC_Graphics_640x200_2Colors);

  uint16_t pal[4];
  if (!mode640) {
    static const uint8_t PC[4][3] = {{2,4,6},{10,12,14},{3,5,7},{11,13,15}};  // CGA 4-colour palettes
    int pi = ga->graphPalette() & 3;
    pal[0] = kCgaRgb565[ga->graphBackgroundIndex() & 0x0F];
    pal[1] = kCgaRgb565[PC[pi][0]];
    pal[2] = kCgaRgb565[PC[pi][1]];
    pal[3] = kCgaRgb565[PC[pi][2]];
  } else {
    pal[0] = kCgaRgb565[0];                                                    // mono: 0 = black
    int fg = ga->graphForegroundIndex() & 0x0F;
    pal[1] = kCgaRgb565[fg ? fg : 15];                                         // 1 = fg colour (def. white)
  }

  displaySetUiMode(false);
  displaySetVideoRect(20, 200);         // 200 active lines centered in 240
  displaySetVideoFill(0, 320, true);    // full-width, stretched to the panel
  tft.setSwapBytes(true);
  for (int oy = 0; oy < 200; ) {
    int n = 0;
    while (oy + n < 200 && n < 8) {
      int sy = oy + n;
      const uint8_t* srow = vm + (sy & 1) * 0x2000 + (sy >> 1) * 80;
      uint16_t* dst = pcGfxScratch + n * 320;
      if (!mode640) {
        for (int x = 0; x < 320; x++)
          dst[x] = pal[(srow[x >> 2] >> (6 - (x & 3) * 2)) & 3];
      } else {
        for (int x = 0; x < 320; x++) {                  // 640->320: OR pixel pairs so thin lines survive
          int sx = x * 2;
          uint8_t p0 = (srow[sx >> 3]       >> (7 - (sx & 7)))       & 1;
          uint8_t p1 = (srow[(sx + 1) >> 3] >> (7 - ((sx + 1) & 7))) & 1;
          dst[x] = pal[(p0 | p1) ? 1 : 0];
        }
      }
      n++;
    }
    tft.pushImage(0, 20 + oy, 320, n, pcGfxScratch);
    oy += n;
  }
  tft.setSwapBytes(false);
}

// ---- CP437 line/block/shade glyphs (0xB0-0xDF) drawn as CONNECTING primitives ----
// The 5x7 GFX font draws box-drawing chars too small to touch cell edges, so borders don't join.
// These 48 codes are pure graphics: draw them as bars that reach the cell edges (adjacent cells share
// the boundary coordinate, so the lines connect — true in native too, since the shared logical edge
// maps to one native pixel). Arm table for the line chars 0xB3..0xDA: 2 bits/arm (0=none,1=single,
// 2=double) in U,D,L,R order.
#define ARM(u,d,l,r) ((u)|((d)<<2)|((l)<<4)|((r)<<6))
static const uint8_t kCgaArms[0xDA - 0xB3 + 1] = {
  /*B3*/ARM(1,1,0,0),/*B4*/ARM(1,1,1,0),/*B5*/ARM(1,1,2,0),/*B6*/ARM(2,2,1,0),
  /*B7*/ARM(0,2,1,0),/*B8*/ARM(0,1,2,0),/*B9*/ARM(2,2,2,0),/*BA*/ARM(2,2,0,0),
  /*BB*/ARM(0,2,2,0),/*BC*/ARM(2,0,2,0),/*BD*/ARM(2,0,1,0),/*BE*/ARM(1,0,2,0),
  /*BF*/ARM(0,1,1,0),/*C0*/ARM(1,0,0,1),/*C1*/ARM(1,0,1,1),/*C2*/ARM(0,1,1,1),
  /*C3*/ARM(1,1,0,1),/*C4*/ARM(0,0,1,1),/*C5*/ARM(1,1,1,1),/*C6*/ARM(1,1,0,2),
  /*C7*/ARM(2,2,0,1),/*C8*/ARM(2,0,0,2),/*C9*/ARM(0,2,0,2),/*CA*/ARM(2,0,2,2),
  /*CB*/ARM(0,2,2,2),/*CC*/ARM(2,2,0,2),/*CD*/ARM(0,0,2,2),/*CE*/ARM(2,2,2,2),
  /*CF*/ARM(1,0,2,2),/*D0*/ARM(2,0,1,1),/*D1*/ARM(0,1,2,2),/*D2*/ARM(0,2,1,1),
  /*D3*/ARM(2,0,0,1),/*D4*/ARM(1,0,0,2),/*D5*/ARM(0,1,0,2),/*D6*/ARM(0,2,0,1),
  /*D7*/ARM(2,2,1,1),/*D8*/ARM(1,1,2,2),/*D9*/ARM(1,0,1,0),/*DA*/ARM(0,1,0,1),
};
#undef ARM

static inline bool pcIsCgaGraphic(uint8_t ch) { return ch >= 0xB0 && ch <= 0xDF; }

static uint16_t pcBlend565(uint16_t fg, uint16_t bg, int num) {   // num/4 of fg over bg
  int r = (((fg >> 11) & 31) * num + ((bg >> 11) & 31) * (4 - num)) / 4;
  int g = (((fg >> 5) & 63) * num + ((bg >> 5) & 63) * (4 - num)) / 4;
  int b = ((fg & 31) * num + (bg & 31) * (4 - num)) / 4;
  return (r << 11) | (g << 5) | b;
}

// Draw a CP437 graphic glyph in the cell logical rect [x,y,w,h] with fg over the already-filled bg.
static void pcDrawCgaGraphic(uint8_t ch, uint16_t fg, uint16_t bg, int x, int y, int w, int h) {
  if (ch <= 0xB2) { tft.fillRect(x, y, w, h, pcBlend565(fg, bg, ch - 0xB0 + 1)); return; }  // ░▒▓
  if (ch >= 0xDB) {                                                                          // blocks
    switch (ch) {
      case 0xDB: tft.fillRect(x, y, w, h, fg); break;                       // █ full
      case 0xDC: tft.fillRect(x, y + h / 2, w, h - h / 2, fg); break;       // ▄ lower
      case 0xDD: tft.fillRect(x, y, (w + 1) / 2, h, fg); break;             // ▌ left
      case 0xDE: tft.fillRect(x + w / 2, y, w - w / 2, h, fg); break;       // ▐ right
      case 0xDF: tft.fillRect(x, y, w, (h + 1) / 2, fg); break;             // ▀ upper
    }
    return;
  }
  uint8_t a = kCgaArms[ch - 0xB3];                                          // line-draw chars
  int arm[4] = { a & 3, (a >> 2) & 3, (a >> 4) & 3, (a >> 6) & 3 };         // U,D,L,R thickness
  int cx = x + w / 2, cy = y + h / 2, xr = x + w - 1, yb = y + h - 1;
  for (int k = 0; k < 2; k++) {                       // vertical arms: U (edge->centre), D (centre->edge)
    if (!arm[k]) continue;
    int y0 = (k == 0) ? y : cy, y1 = (k == 0) ? cy : yb;
    if (arm[k] == 1) tft.fillRect(cx, y0, 1, y1 - y0 + 1, fg);
    else { tft.fillRect(cx - 1, y0, 1, y1 - y0 + 1, fg); tft.fillRect(cx + 1, y0, 1, y1 - y0 + 1, fg); }
  }
  for (int k = 2; k < 4; k++) {                       // horizontal arms: L, R
    if (!arm[k]) continue;
    int x0 = (k == 2) ? x : cx, x1 = (k == 2) ? cx : xr;
    if (arm[k] == 1) tft.fillRect(x0, cy, x1 - x0 + 1, 1, fg);
    else { tft.fillRect(x0, cy - 1, x1 - x0 + 1, 1, fg); tft.fillRect(x0, cy + 1, x1 - x0 + 1, 1, fg); }
  }
}

#if BOARD_PANEL_DSI || defined(BOARD_DESKTOP)
const uint8_t *pcBiosFont8x8();   // the authentic IBM CP437 8x8 font, pulled from the BIOS (bios.cpp)

// Render CGA text with the ORIGINAL IBM 8x8 font, each cell = one glyph scaled to fill it, into a
// PW x PH target. Authentic look + real box-drawing/shading glyphs (no GFX-font approximation or
// connect-the-borders hack). Used on the JC1060P470 panel (1024x600 canvas) AND the desktop SDL
// framebuffer (640x400) — both have a real drawGlyph8. Returns false if the BIOS font isn't found.
static bool pcxtRenderTextGlyph(int PW, int PH) {
  const uint8_t *font = pcBiosFont8x8();
  if (!font) return false;
  bool text80 = (g_pcxtMachine.graphicsAdapter()->emulation()
                 == fabgl::GraphicsAdapter::Emulation::PC_Text_80x25_16Colors);
  int cols = text80 ? 80 : 40;
  const uint8_t* vbuf = g_pcxtMachine.videoMemory() + 0x8000 + g_pcxtMachine.cgaMemOffset();

  for (int r = 0; r < 25; r++) {
    int y0 = r * PH / 25, y1 = (r + 1) * PH / 25;
    for (int c = 0; c < cols; c++) {
      uint8_t ch   = vbuf[(r * cols + c) * 2];
      uint8_t attr = vbuf[(r * cols + c) * 2 + 1];
      int x0 = c * PW / cols, x1 = (c + 1) * PW / cols;
      tft.drawGlyph8(x0, y0, x1 - x0, y1 - y0, font + ch * 8,
                     kCgaRgb565[attr & 0x0F], kCgaRgb565[(attr >> 4) & 0x07]);
    }
  }
  // hardware cursor (6845): blink the underline glyph (0x5F) transparently over the cursor cell.
  fabgl::GraphicsAdapter* ga = g_pcxtMachine.graphicsAdapter();
  if (ga->cursorVisible() && ((millis() / 400) & 1) == 0) {
    int cr = ga->cursorRow(), cc = ga->cursorCol();
    if (cr >= 0 && cr < 25 && cc >= 0 && cc < cols) {
      int x0 = cc * PW / cols, x1 = (cc + 1) * PW / cols;
      int y0 = cr * PH / 25,   y1 = (cr + 1) * PH / 25;
      tft.drawGlyph8(x0, y0, x1 - x0, y1 - y0, font + 0x5F * 8, kCgaRgb565[15], 0, true);
    }
  }
  // mouse cursor: reverse-video block at the mouse cell (INT 33h served from the USB mouse). Reverse
  // video = draw the cell glyph with fg/bg swapped (opaque bg paints the whole cell), matching the
  // standard text-mode mouse driver block.
  if (pcMouseShown) {
    int mc = pcMouseX / 8, mr = pcMouseY / 8;
    if (mc >= 0 && mc < cols && mr >= 0 && mr < 25) {
      uint8_t ch = vbuf[(mr * cols + mc) * 2];
      uint8_t a  = vbuf[(mr * cols + mc) * 2 + 1];
      int x0 = mc * PW / cols, x1 = (mc + 1) * PW / cols;
      int y0 = mr * PH / 25,   y1 = (mr + 1) * PH / 25;
      tft.drawGlyph8(x0, y0, x1 - x0, y1 - y0, font + ch * 8,
                     kCgaRgb565[(a >> 4) & 0x07], kCgaRgb565[a & 0x0F]);   // fg<->bg (reverse)
    }
  }
  return true;   // the target is pushed 1:1 (P4 canvas / desktop fb)
}
#endif

// ---- CGA text render: 80x25 / 40x25 with the built-in 6x8 font (like iigsRenderText) ----
static void pcxtRenderText() {
#if BOARD_PANEL_DSI
  if (pcxtRenderTextGlyph(PANEL_NATIVE_W, PANEL_NATIVE_H)) return;   // P4: original IBM 8x8 font, full-screen
#elif defined(BOARD_DESKTOP)
  if (pcxtRenderTextGlyph(PCXT_DESK_W, PCXT_DESK_H)) return;          // desktop: same authentic font, 640x400 fb
#endif
  tft.setUiMode(true);
  bool osk = oskActive();
  int kbdTop = osk ? oskRasterHeight() : 240;

  bool text80 = (g_pcxtMachine.graphicsAdapter()->emulation()
                 == fabgl::GraphicsAdapter::Emulation::PC_Text_80x25_16Colors);
  int cols = text80 ? 80 : 40;

  const uint8_t* vbuf = g_pcxtMachine.videoMemory() + 0x8000 + g_pcxtMachine.cgaMemOffset();

  if (osk) tft.fillRect(0, 0, 320, kbdTop, TFT_BLACK);
  else     tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  // Each glyph advances 6 NATIVE px = 4 logical units (drawString draws unscaled glyphs). So a column
  // is logical X0 + c*4, and the text block is cols*4 logical wide (80->320 full width, 40->160 centered).
  const int LCW = 4;                          // logical units per char cell (= 6 native px)
  int X0 = text80 ? 0 : (320 - cols * LCW) / 2;
  float rowH = (kbdTop - 1.0f) / 25.0f;
  char line[81];
  // Render per attribute-RUN: fill the run's background over the FULL row band (+1px overlap so the
  // logical->native x272/240 rounding leaves no black stripe), then draw the glyphs transparently on
  // top. Per-run bg means each cell keeps its own colour -> coloured text and inverse video (e.g.
  // 0x70 = black-on-white) just work, AND the gaps between rows are filled with the matching bg
  // (fixes the stripes that returned when bg was filled per-row-first-cell only).
  for (int r = 0; r < 25; r++) {
    int yTop = (int)(r * rowH + 0.5f);
    int yBot = (int)((r + 1) * rowH + 0.5f);
    int c = 0;
    while (c < cols) {
      uint8_t attr = vbuf[(r * cols + c) * 2 + 1];
      int start = c, n = 0;
      while (c < cols && vbuf[(r * cols + c) * 2 + 1] == attr) {
        uint8_t ch = vbuf[(r * cols + c) * 2];
        // The GFX classic font is the full 256-glyph CP437 set (box-drawing, shading, etc.), so pass
        // the byte through; only 0x00 (would end the C-string) and 0x0A/0x0D (the font writer treats
        // them as newline/CR) must be mapped to a blank.
        // CP437 line/block/shade chars (0xB0-0xDF) are drawn as connecting primitives below, so blank
        // them here; 0x00 ends the C-string and 0x0A/0x0D are taken as newline/CR by the font writer.
        line[n++] = (ch == 0x00 || ch == 0x0A || ch == 0x0D || pcIsCgaGraphic(ch)) ? ' ' : (char)ch;
        c++;
      }
      line[n] = 0;
      tft.fillRect(X0 + start * LCW, yTop, n * LCW, (yBot - yTop) + 1, kCgaRgb565[(attr >> 4) & 0x07]);
      tft.setTextColor(kCgaRgb565[attr & 0x0F]);   // transparent glyph over the run's bg
      tft.drawString(line, X0 + start * LCW, yTop, 1);
    }
    // overlay CP437 line/block/shade glyphs as connecting primitives (bg already filled above)
    for (int cc = 0; cc < cols; cc++) {
      uint8_t ch = vbuf[(r * cols + cc) * 2];
      if (!pcIsCgaGraphic(ch)) continue;
      uint8_t at = vbuf[(r * cols + cc) * 2 + 1];
      pcDrawCgaGraphic(ch, kCgaRgb565[at & 0x0F], kCgaRgb565[(at >> 4) & 0x07],
                       X0 + cc * LCW, yTop, LCW, (yBot - yTop) + 1);
    }
  }

  // hardware cursor (6845): blinking underline ('_') at the cursor cell.
  // drawString maps the START (x,y) logical->native (x1.5) but advances each glyph by 6 NATIVE px,
  // so column cc lands at logical X0 + cc*(6*320/480) = X0 + cc*4. Drawing the cursor as a glyph (not
  // a logical fillRect, which would be x1.5 too wide and drift right) keeps it aligned + sized like the text.
  fabgl::GraphicsAdapter* ga = g_pcxtMachine.graphicsAdapter();
  if (ga->cursorVisible() && ((millis() / 400) & 1) == 0) {
    int cr = ga->cursorRow(), cc = ga->cursorCol();
    if (cr >= 0 && cr < 25 && cc >= 0 && cc < cols) {
      tft.setTextColor(kCgaRgb565[15]);                       // bright, transparent overlay (no bg)
      tft.drawString("_", X0 + cc * LCW, (int)(cr * rowH + 0.5f), 1);
    }
  }

  // mouse cursor: a reverse-video block at the mouse cell (INT 33h served from the USB mouse)
  if (pcMouseShown) {
    int mc = pcMouseX / 8, mr = pcMouseY / 8;
    if (mc >= 0 && mc < cols && mr >= 0 && mr < 25) {
      int yT = (int)(mr * rowH + 0.5f), yB = (int)((mr + 1) * rowH + 0.5f);
      uint8_t a  = vbuf[(mr * cols + mc) * 2 + 1];
      uint8_t ch = vbuf[(mr * cols + mc) * 2];
      char s[2] = { (char)((ch == 0x00 || ch == 0x0A || ch == 0x0D) ? ' ' : ch), 0 };
      tft.fillRect(X0 + mc * LCW, yT, LCW, (yB - yT) + 1, kCgaRgb565[a & 0x0F]);  // bg := fg (reverse)
      tft.setTextColor(kCgaRgb565[(a >> 4) & 0x07]);                             // fg := bg
      tft.drawString(s, X0 + mc * LCW, yT, 1);
    }
  }
}

#if defined(BOARD_DESKTOP)
// Desktop CGA graphics into the 640x400 fb: 320x200x4 -> 2x both; 640x200x2 -> 1x wide, 2x tall.
// Native resolution (no 640->320 downscale like the device path), pushed as 2-row bands.
static void pcxtRenderGraphicsDesktop(fabgl::GraphicsAdapter::Emulation emu) {
  static uint16_t* band = nullptr;
  if (!band) band = (uint16_t*)malloc(PCXT_DESK_W * 2 * sizeof(uint16_t));
  if (!band) return;
  fabgl::GraphicsAdapter* ga = g_pcxtMachine.graphicsAdapter();
  const uint8_t* vm = g_pcxtMachine.videoMemory() + 0x8000;
  bool mode640 = (emu == fabgl::GraphicsAdapter::Emulation::PC_Graphics_640x200_2Colors);

  uint16_t pal[4];
  if (!mode640) {
    static const uint8_t PC[4][3] = {{2,4,6},{10,12,14},{3,5,7},{11,13,15}};
    int pi = ga->graphPalette() & 3;
    pal[0] = kCgaRgb565[ga->graphBackgroundIndex() & 0x0F];
    pal[1] = kCgaRgb565[PC[pi][0]]; pal[2] = kCgaRgb565[PC[pi][1]]; pal[3] = kCgaRgb565[PC[pi][2]];
  } else {
    int fg = ga->graphForegroundIndex() & 0x0F;
    pal[0] = kCgaRgb565[0]; pal[1] = kCgaRgb565[fg ? fg : 15];
  }
  tft.setSwapBytes(true);
  for (int sy = 0; sy < 200; sy++) {                       // 200 CGA lines -> rows 2*sy, 2*sy+1
    const uint8_t* srow = vm + (sy & 1) * 0x2000 + (sy >> 1) * 80;
    for (int x = 0; x < PCXT_DESK_W; x++) {
      if (!mode640) { int sx = x >> 1; band[x] = pal[(srow[sx >> 2] >> (6 - (sx & 3) * 2)) & 3]; }      // 640->320 (2x)
      else          { band[x] = pal[(srow[x >> 3] >> (7 - (x & 7))) & 1]; }                              // 1:1
    }
    memcpy(band + PCXT_DESK_W, band, PCXT_DESK_W * sizeof(uint16_t));   // duplicate -> 2x vertical
    tft.pushImage(0, sy * 2, PCXT_DESK_W, 2, band);
  }
  tft.setSwapBytes(false);
}
#endif

// Dispatch by CGA mode (text vs graphics). Returns true if it (re)drew, false if the picture was
// unchanged and rendering was skipped. The render loop uses that to SKIP the QSPI flush when nothing
// changed, freeing the shared MSPI bus for the core-1 8086 -> big speedup when the screen is static.
static uint32_t pcRenderSig = 1;
static int      pcRenderGfx = -1;

void pcxtForceRedraw() { pcRenderSig = 1; pcRenderGfx = -1; }   // after a menu/clear: force a repaint

bool pcxtRenderFrame() {
  if (!pcInitDone) return false;
  fabgl::GraphicsAdapter* ga = g_pcxtMachine.graphicsAdapter();
  auto emu = ga->emulation();
  bool gfx = (emu == fabgl::GraphicsAdapter::Emulation::PC_Graphics_320x200_4Colors ||
              emu == fabgl::GraphicsAdapter::Emulation::PC_Graphics_640x200_2Colors);

  // Signature of everything that affects the picture (video bytes + mode/colour/cursor/blink).
  const uint8_t* vbuf = g_pcxtMachine.videoMemory() + 0x8000 + g_pcxtMachine.cgaMemOffset();
  const uint32_t* w32 = (const uint32_t*)vbuf;
  int nwords = (gfx ? 16000 : 4000) / 4;
  uint32_t sig = 2166136261u;
  for (int i = 0; i < nwords; i++) sig = (sig ^ w32[i]) * 16777619u;
  sig ^= (uint32_t)emu * 2654435761u;
  sig ^= (uint32_t)g_pcxtMachine.cgaColorReg() << 3;
  sig ^= (uint32_t)(ga->cursorRow() * 256 + ga->cursorCol()) << 11;
  if (!gfx && ga->cursorVisible() && ((millis() / 400) & 1)) sig ^= 0xCAFEF00Du;  // blink phase (text)
  if (pcMouseShown) sig ^= (uint32_t)(pcMouseX * 643 + pcMouseY + 1) * 2246822519u;  // mouse cursor moved

  bool modeChanged = ((int)gfx != pcRenderGfx);
  if (sig == pcRenderSig && !modeChanged) return false;   // unchanged -> skip render + flush
  pcRenderSig = sig;

  if (modeChanged) {                         // text <-> graphics switch: wipe canvas + panel border
    pcRenderGfx = (int)gfx;
    tft.setUiMode(true);
    tft.fillScreen(TFT_BLACK);
#if BOARD_DISPLAY_GFX
    tft.fillPanelBlack();
#endif
  }
  if (gfx) {
#if defined(BOARD_DESKTOP)
    pcxtRenderGraphicsDesktop(emu);          // native 640x400 (no 640->320 downscale)
#else
    pcxtRenderGraphics(emu);
#endif
  } else   pcxtRenderText();
  return true;
}

bool pcxtRenderLoadWarning() {
  // Always run the BIOS (it shows POST + a "no boot disk" message itself), so we
  // never block the screen. Returning false lets pcxtRenderFrame draw the CGA text.
  return false;
}

// ---- input: USB HID usage -> XT (set-1) scancode -> i8042 ----
// Returns set-1 make code (break = code|0x80). *e0 set for E0-prefixed extended keys.
static uint8_t hidToScan(uint8_t usage, bool* e0) {
  *e0 = false;
  if (usage >= 0x04 && usage <= 0x1D) {     // A..Z (HID order) -> set-1 letter codes
    static const uint8_t L[26] = {
      0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
      0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
    return L[usage - 0x04];
  }
  if (usage >= 0x1E && usage <= 0x26) return (uint8_t)(0x02 + (usage - 0x1E));  // 1..9
  if (usage == 0x27) return 0x0B;           // 0
  switch (usage) {
    case 0x28: return 0x1C;  // Enter
    case 0x29: return 0x01;  // Esc
    case 0x2A: return 0x0E;  // Backspace
    case 0x2B: return 0x0F;  // Tab
    case 0x2C: return 0x39;  // Space
    case 0x2D: return 0x0C;  // -
    case 0x2E: return 0x0D;  // =
    case 0x2F: return 0x1A;  // [
    case 0x30: return 0x1B;  // ]
    case 0x31: return 0x2B;  // backslash
    case 0x33: return 0x27;  // ;
    case 0x34: return 0x28;  // '
    case 0x35: return 0x29;  // `
    case 0x36: return 0x33;  // ,
    case 0x37: return 0x34;  // .
    case 0x38: return 0x35;  // /
    case 0x39: return 0x3A;  // CapsLock
    case 0x3A: return 0x3B;  case 0x3B: return 0x3C;  case 0x3C: return 0x3D;  // F1..F3
    case 0x3D: return 0x3E;  case 0x3E: return 0x3F;  case 0x3F: return 0x40;  // F4..F6
    case 0x40: return 0x41;  case 0x41: return 0x42;  case 0x42: return 0x43;  // F7..F9
    case 0x43: return 0x44;  case 0x44: return 0x57;  case 0x45: return 0x58;  // F10..F12
    case 0x4F: return 0x4D;  // Right (keypad 6; no E0 = classic XT arrow, NumLock off)
    case 0x50: return 0x4B;  // Left  (keypad 4)
    case 0x51: return 0x50;  // Down  (keypad 2)
    case 0x52: return 0x48;  // Up    (keypad 8)
    case 0xE0: return 0x1D;  // LCtrl
    case 0xE1: return 0x2A;  // LShift
    case 0xE2: return 0x38;  // LAlt
    case 0xE4: *e0 = true; return 0x1D;  // RCtrl
    case 0xE5: return 0x36;  // RShift
    default:   return 0x00;
  }
}

void pcxtKeyDown(uint8_t hidUsage, bool /*shift*/, bool /*ctrl*/, bool /*alt*/) {
  bool e0; uint8_t sc = hidToScan(hidUsage, &e0);
  if (!sc) return;
  if (e0) g_pcxtMachine.injectScancode(0xE0);
  g_pcxtMachine.injectScancode(sc);
}

void pcxtKeyUp(uint8_t hidUsage) {
  bool e0; uint8_t sc = hidToScan(hidUsage, &e0);
  if (!sc) return;
  if (e0) g_pcxtMachine.injectScancode(0xE0);
  g_pcxtMachine.injectScancode(sc | 0x80);
}

// gamepad -> arrow keys + Enter/Esc (active-low mask: b0 up,b1 down,b2 left,b3 right,b4 A,b5 B)
void pcxtSetInput(uint8_t joyMask) {
  static uint8_t prev = 0xFF;
  struct { uint8_t bit; uint8_t usage; } map[] = {
    {0x01, 0x52}, {0x02, 0x51}, {0x04, 0x50}, {0x08, 0x4F}, {0x10, 0x28}, {0x20, 0x29} };
  for (auto& m : map) {
    bool nowDown  = !(joyMask & m.bit);
    bool prevDown = !(prev   & m.bit);
    if (nowDown && !prevDown) pcxtKeyDown(m.usage, false, false, false);
    else if (!nowDown && prevDown) pcxtKeyUp(m.usage);
  }
  prev = joyMask;
}

void pcxtHardReset() { pcResetReq = true; }

// ---- settings hooks ----
void pcxtScanFiles() { loadPcxtFilesSync(); }

// Mount the selected image explicitly into A: (floppy) or C: (hard disk), from the settings menu.
// No reboot: an A: change is seen live by DOS; a new C: needs a reboot to be recognised.
bool pcxtMountA(const char* path) { return pcMountInto(path, 0); }
bool pcxtMountC(const char* path) { return pcMountInto(path, 2); }

// Route a PC disk image by SIZE: a floppy (<= 2.88MB) -> A:, a larger image -> C: (hard disk). The
// desktop "Load" menu (and the device disk browser) use this so a hard-disk image like DOSHDD.IMG
// lands on C:, not A:. A new C: needs a re-POST for the BIOS to detect it, so request one.
bool pcxtMountAuto(const char* path) {
  if (!pcInitDone || !path) return false;
  uint32_t sz = 0;
  busTake();
  File f = FSTYPE.open(path, FILE_READ);
  if (f) { sz = (uint32_t)f.size(); f.close(); }
  busGive();
  if (sz > 2949120u) {                    // > 2.88MB (largest floppy) -> hard disk C:
    if (selectedPcFileName == path) pcxtUnmount(0);   // same image stuck on A: (old mis-route) -> clear it
    bool ok = pcxtMountC(path);
    if (ok) pcResetReq = true;            // re-POST so the BIOS scans the new C: (and boots it if no A:)
    return ok;
  }
  if (selectedPcHdFileName == path) pcxtUnmount(2);    // same image stuck on C: -> clear it
  return pcxtMountA(path);                 // floppy -> A: (live media change)
}

// Eject the disk in a slot (0 = A:, 2 = C:): close the image and clear the saved name.
void pcxtUnmount(int slot) {
  if (!pcInitDone) return;
  g_pcxtMachine.setDriveImage(slot, nullptr);      // null filename -> close + leave the slot empty
  if (slot == 0) selectedPcFileName = ""; else selectedPcHdFileName = "";
  pcUpdateBootDrive();
  printLog(slot == 0 ? "PCXT: ejected A:" : "PCXT: ejected C:");
}
