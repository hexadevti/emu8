// debug_bridge.cpp — desktop debug facade implementation (see debug_bridge.h). Dispatches on
// currentPlatform to reach each core's CPU registers / memory. Apple II (6502) is fully wired; the
// other platforms light up as their accessors are added (they currently report "(unsupported)").
#if defined(BOARD_DESKTOP)

#include "debug_bridge.h"
#include "../../emu.h"      // currentPlatform, PLATFORM_*, the 6502 globals (A/X/Y/PC/STP/SR, ram[]...),
                            // the soft-switch flags, read8(), and the Arduino ESP shim (ESP.restart())
#include "../c64/c64.h"     // namespace c64: 6510 registers + banking state for the C64 debug panels
#include "../msx/msx.h"     // namespace msx: Z80 register file + memory map for the MSX debug panels
#include "../msx/msx_disk.h"// namespace msx: diskPeek() (side-effect-free slot-2 read)
#include <cstdlib>
#include <cstring>

void desktopUiSaveState();   // ui_imgui.cpp — flush window positions + settings before any process re-exec
static void rebootInto(int p);   // re-exec into platform p (defined below; used by dbgReset fallback)
// Per-platform in-process reset requests (serviced by each core's loop) — reboot the EMULATOR only.
namespace c64   { extern volatile bool c64ResetReq; }
namespace atari { extern volatile bool atariResetReq; }
extern volatile bool msxResetReq;   // src/msx/msx.cpp — serviced at the top of msxLoop (race-free, same thread)
// NES core symbols the debug facade reads/writes (registers + the memory the peek/poke decode needs).
// Declared here (not via nes.h) to match this file's existing per-core extern style; must mirror the
// definitions in src/nes/nes_cpu.cpp + nes core.
namespace nes {
  extern volatile bool nesResetReq;
  extern unsigned short PC;
  extern unsigned char STP, A, X, Y, SR;
  extern uint8_t *cpuRam;     // 2K internal RAM ($0000-$07FF, mirrored)
  extern uint8_t *prgRam;     // optional 8K PRG-RAM at $6000
  extern uint8_t *prgMap[4];  // four 8K PRG windows for $8000/$A000/$C000/$E000
  extern uint8_t  mirrorMode; // 0=horizontal,1=vertical,2=single0,3=single1
  extern uint8_t  mapperNum;  // iNES mapper number
  extern bool     chrIsRam;   // CHR is 8K RAM (vs ROM)
  extern uint8_t  paletteRam[0x20];      // palette RAM ($3F00-$3F1F)
  extern uint8_t  oam[0x100];            // 64 sprites x 4 bytes
  extern const uint16_t nesPalette[64];  // 64-colour master palette (RGB565)
  void dbgPpuSnapshot(uint8_t *ctrl, uint8_t *mask, uint8_t *status, int *scanline,
                      uint16_t *v, uint8_t *finex);     // nes_ppu.cpp (desktop only)
  void dbgRenderPatternTable(int half, int pal4, uint32_t *out);  // 128x128 ABGR8888
  void dbgRenderNametables(uint32_t *out);                        // 512x480 ABGR8888
}
static uint8_t nesPeek(uint16_t a);   // NES side-effect-free peek (defined below; used by dbgSoftReset)

int dbgStepReq = 0;

// ---- heat map state ----
bool      g_dbgHeatOn = false;
uint32_t *g_dbgHeat[4] = {nullptr, nullptr, nullptr, nullptr};   // R / W / X / VIC-DMA

void dbgHeatEnable(bool on) {
  // Buffers are allocated once on first enable and kept for the session: disabling just clears the
  // flag (never frees), so the CPU thread can't deref a freed buffer mid-dbgBusTouch (no lock needed).
  if (on) {
    for (int k = 0; k < 4; k++)
      if (!g_dbgHeat[k]) g_dbgHeat[k] = (uint32_t *)calloc(0x10000, sizeof(uint32_t));
    g_dbgHeatOn = (g_dbgHeat[0] && g_dbgHeat[1] && g_dbgHeat[2] && g_dbgHeat[3]);
  } else {
    g_dbgHeatOn = false;
  }
}
bool dbgHeatEnabled() { return g_dbgHeatOn; }
void dbgHeatClear() {
  for (int k = 0; k < 4; k++) if (g_dbgHeat[k]) memset(g_dbgHeat[k], 0, 0x10000 * sizeof(uint32_t));
}
void dbgHeatDecay(float keep) {
  if (!g_dbgHeatOn) return;
  for (int k = 0; k < 4; k++)
    for (int i = 0; i < 0x10000; i++) g_dbgHeat[k][i] = (uint32_t)(g_dbgHeat[k][i] * keep);
}
const uint32_t *dbgHeatBuf(int kind) { return (kind >= 0 && kind < 4) ? g_dbgHeat[kind] : nullptr; }

// ---- disk read/write heat map ----
bool     g_dbgDiskHeatOn = false;
uint32_t g_dbgDiskHeat[DBG_DISK_TRACKS * DBG_DISK_BINS]  = {0};   // reads
uint32_t g_dbgDiskHeatW[DBG_DISK_TRACKS * DBG_DISK_BINS] = {0};   // writes
int      g_dbgDiskTrack = -1;
// Apple Disk II ($C0EC nibbles), the C64 .d64 virtual drive (d64ReadSector) and the MSX WD2793 FDC
// (Read/Write Sector commands) all feed the map. MSX only when a .dsk is actually mounted.
bool dbgDiskHeatSupported() {
  return currentPlatform == PLATFORM_APPLE2 || currentPlatform == PLATFORM_C64
      || (currentPlatform == PLATFORM_MSX && msx::diskPresent());
}
void dbgDiskHeatEnable(bool on) { g_dbgDiskHeatOn = on; }
bool dbgDiskHeatEnabled() { return g_dbgDiskHeatOn; }
// C64 cartridge ROM-access map (shares the disk Record toggle + the "Disk read" panel).
uint32_t g_dbgCartHeat[DBG_CART_BANKS * DBG_CART_BINS] = {0};
int      g_dbgCartBank = -1;
int      g_dbgCartMaxBank = 0;
bool dbgCartActive() { return currentPlatform == PLATFORM_C64 && c64::cartActive; }
int  dbgCartBankCount() { return dbgCartActive() ? c64CartBankCount() : 0; }

void dbgDiskHeatClear() {
  memset(g_dbgDiskHeat,  0, sizeof(g_dbgDiskHeat));
  memset(g_dbgDiskHeatW, 0, sizeof(g_dbgDiskHeatW));
  memset(g_dbgCartHeat,  0, sizeof(g_dbgCartHeat));
  g_dbgDiskTrack = -1; g_dbgCartBank = -1; g_dbgCartMaxBank = 0;
}
void dbgDiskHeatDecay(float keep) {
  for (int i = 0; i < DBG_DISK_TRACKS * DBG_DISK_BINS; i++) {
    g_dbgDiskHeat[i]  = (uint32_t)(g_dbgDiskHeat[i]  * keep);
    g_dbgDiskHeatW[i] = (uint32_t)(g_dbgDiskHeatW[i] * keep);
  }
  for (int i = 0; i < DBG_CART_BANKS * DBG_CART_BINS; i++)
    g_dbgCartHeat[i] = (uint32_t)(g_dbgCartHeat[i] * keep);
}
// A track/sector floppy is mounted (-> circular disk view) vs an HD/block image (-> grid). The Disk II
// nibble hook only feeds floppy reads (!HdDisk on the Apple II); the C64 .d64 and MSX .dsk are floppies.
bool dbgDiskIsFloppy() {
  return (currentPlatform == PLATFORM_APPLE2 && !HdDisk) || currentPlatform == PLATFORM_C64
      || currentPlatform == PLATFORM_MSX;
}
// Rings to draw: Apple/C64 keep the historical 35; MSX derives the cylinder count from the mounted
// .dsk geometry (720K = 80, 360K = 40), clamped to the heat array's track capacity.
int dbgDiskTrackCount() {
  if (currentPlatform == PLATFORM_MSX) {
    int n = msx::diskTrackCount();
    if (n < 1) n = 1; if (n > DBG_DISK_TRACKS) n = DBG_DISK_TRACKS;
    return n;
  }
  return 35;
}

// ---- breakpoint / watchpoint / run-control state ----
bool    g_dbgBpAny = false;
bool    g_dbgBp[0x10000] = {false};
bool    g_dbgBreakArmed = true;
int     g_dbgRunToPC = -1;
int     g_dbgRunUntilSP = -1;
bool    g_dbgWatchAny = false;
uint8_t g_dbgWatch[0x10000] = {0};
int     g_dbgWatchHit = -1;

// ================================ execution control ===============================================
void dbgSetPaused(bool p) {
  paused = p;
  if (!p) { dbgStepReq = 0; g_dbgBreakArmed = false; }   // on resume, don't instantly re-break at the current PC
  else { g_dbgBreakArmed = true; g_dbgRunToPC = -1; g_dbgRunUntilSP = -1; }  // manual pause cancels pending runs
}
bool dbgIsPaused()        { return paused; }

bool dbgStepSupported() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: case PLATFORM_C64:
    case PLATFORM_NES:    case PLATFORM_ATARI:
    case PLATFORM_MSX:    return true;   // these cores honor dbgStepReq
    default: return false;
  }
}
void dbgStep() {
  if (!paused) paused = true;     // step implies paused
  if (dbgStepSupported()) dbgStepReq++;
}
// Apple II cold reboot WITHOUT re-exec: invalidate the Monitor power-up byte so the ROM reset handler
// does a full COLD start (scans slots + boots the disk), force the disk to re-open, then reset the
// 6502 to its reset vector. The desktop app / ImGui windows stay exactly where they are.
static void a2ColdReboot() {
  bool wasPaused = paused;
  paused = true;
  ram[0x3F4] = 0x00;            // power-up byte mismatch -> Monitor cold-starts (re-boots a slot)
  diskChanged = true;          // next $C0EC read re-opens the disk image (re-seek from track 0)
  PC = read16(0xFFFC); STP = 0xFD; SR = 0x04;   // 6502 RESET
  paused = wasPaused;
}

// "Reboot" reboots the EMULATED MACHINE ONLY — it must NOT restart the desktop application (that would
// move the windows). In-process where the core supports it; the rest re-exec (state saved first).
void dbgReset() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: a2ColdReboot();             break;
    case PLATFORM_C64:    c64::c64ResetReq   = true;  break;
    case PLATFORM_NES:    nes::nesResetReq   = true;  break;
    case PLATFORM_ATARI:  atari::atariResetReq = true; break;
    case PLATFORM_MSX:    msxResetReq        = true;  break;   // in-process machine reset (keeps the debug session)
    default:              rebootInto(currentPlatform); break;   // SMS/PCXT/tiny386/IIgs: re-exec
  }
}

// --- machine model + adjustable clock (Apple II) ---
bool dbgAppleModelSupported() { return currentPlatform == PLATFORM_APPLE2; }
bool dbgGetAppleIIe() { return AppleIIe; }
void dbgSetAppleIIe(bool iie) {
  if (currentPlatform != PLATFORM_APPLE2 || iie == (bool)AppleIIe) return;
  AppleIIe = iie;
  activeFlags = AppleIIe ? flagsIIe : flagsIIplus;   // 6502 flag behavior differs (IIe vs II+)
  saveConfig();
  a2ColdReboot();                                    // re-boot so it comes up as the selected model
}
bool  dbgClockSupported()  { return currentPlatform == PLATFORM_APPLE2; }
bool  dbgGetThrottle()     { return !Fast1MhzSpeed; }              // throttled = the paced 1 MHz path
void  dbgSetThrottle(bool on) { Fast1MhzSpeed = !on; }
float dbgGetClockMhz()     { return appleClockMhz; }
void  dbgSetClockMhz(float mhz) { if (mhz < 0.05f) mhz = 0.05f; if (mhz > 16.0f) mhz = 16.0f; appleClockMhz = mhz; }
float dbgClockDefaultMhz() { return 1.0f; }                        // stock Apple II 6502 clock
float dbgGetMeasuredMhz()  { return appleMeasuredMhz; }

// --- full host speed (uncapped), for EVERY platform. The cores honor a per-platform "Fast" flag in
// their pacing loop (Apple II/C64/IIGS share Fast1MhzSpeed; NES/MSX/SMS have their own); Atari, PC-XT
// and tiny386 have no real-time throttle on the desktop, so they always run uncapped (Fixed = true). ---
bool dbgFullSpeedSupported() { return true; }   // every desktop platform can run at full host speed
bool dbgFullSpeedFixed() {
  switch (currentPlatform) {
    case PLATFORM_ATARI: case PLATFORM_PCXT: case PLATFORM_TINY386: return true;   // always uncapped on desktop
    default: return false;
  }
}
bool dbgGetFullSpeed() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: case PLATFORM_C64: case PLATFORM_IIGS: return Fast1MhzSpeed;
    case PLATFORM_NES: return nesFast;
    case PLATFORM_MSX: return msxFast;
    case PLATFORM_SMS: return smsFast;
    default: return true;                                                          // Atari/PCXT/tiny386
  }
}
void dbgSetFullSpeed(bool on) {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: case PLATFORM_C64: case PLATFORM_IIGS: Fast1MhzSpeed = on; break;
    case PLATFORM_NES: nesFast = on; break;
    case PLATFORM_MSX: msxFast = on; break;
    case PLATFORM_SMS: smsFast = on; break;
    default: return;                                                               // fixed-uncapped platforms
  }
  saveConfig();   // persist where the flag has EEPROM backing (Apple II's Fast1MhzSpeed)
}

// Higher-level run control — the 6502 cores (Apple II, C64, NES) and the Z80 (MSX) carry the
// run-target checks in their instruction loops.
bool dbgRunControlSupported() {
  return currentPlatform == PLATFORM_APPLE2 || currentPlatform == PLATFORM_C64
      || currentPlatform == PLATFORM_NES    || currentPlatform == PLATFORM_MSX;
}
// Current stack pointer of the active core, for step-out's frame test. 6502 cores use the 8-bit S
// (page 1); the Z80 (MSX) has a full 16-bit SP, so this returns the widest form and the loops compare
// against it in their native width.
static uint16_t dbgCurSP() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: return STP;
    case PLATFORM_C64:    return c64::STP;
    case PLATFORM_NES:    return nes::STP;
    case PLATFORM_MSX:    return msx::cpu.SP;
    default: return 0;
  }
}
void dbgStepOver() {
  if (!dbgRunControlSupported()) { dbgStep(); return; }
  uint16_t pc = (uint16_t)dbgGetPC();
  if (currentPlatform == PLATFORM_MSX) {       // Z80: step over CALL / CALL cc / RST (run to the return addr)
    uint8_t op = dbgPeek(pc);
    bool isCall = (op == 0xCD)                  // CALL nn
               || ((op & 0xC7) == 0xC4)         // CALL cc,nn (C4 CC D4 DC E4 EC F4 FC)
               || ((op & 0xC7) == 0xC7);        // RST p     (C7 CF D7 DF E7 EF F7 FF)
    if (isCall) {
      char tmp[40];
      int len = dbgDisasm(pc, tmp, sizeof(tmp));   // CALL nn = 3 bytes, RST = 1 byte
      g_dbgRunToPC = (pc + len) & 0xFFFF;
      dbgSetPaused(false);
    } else {
      dbgStep();
    }
    return;
  }
  if (dbgPeek(pc) == 0x20) {                  // JSR abs -> run the subroutine, break at the return addr
    g_dbgRunToPC = (pc + 3) & 0xFFFF;
    dbgSetPaused(false);
  } else {
    dbgStep();                                // anything else -> ordinary step-into
  }
}
void dbgStepOut() {
  if (!dbgRunControlSupported()) return;
  g_dbgRunUntilSP = dbgCurSP();                // pause once SP rises above the current frame (RTS popped)
  dbgSetPaused(false);
}
void dbgRunTo(uint32_t addr) {
  if (!dbgRunControlSupported()) return;
  g_dbgRunToPC = (int)(addr & 0xFFFF);
  dbgSetPaused(false);
}
bool dbgSoftResetSupported() {
  return currentPlatform == PLATFORM_APPLE2 || currentPlatform == PLATFORM_C64
      || currentPlatform == PLATFORM_NES    || currentPlatform == PLATFORM_MSX;
}
void dbgSoftReset() {
  if (!dbgSoftResetSupported()) return;
  g_dbgRunToPC = -1; g_dbgRunUntilSP = -1;
  switch (currentPlatform) {                     // 6502 RESET: load the reset vector, init SP, set I
    case PLATFORM_APPLE2: PC = read16(0xFFFC); STP = 0xFD; SR |= 0x04; break;
    case PLATFORM_C64:    c64::PC = c64::read16(0xFFFC); c64::STP = 0xFD; c64::SR |= 0x04; break;
    case PLATFORM_NES:    nes::PC = nesPeek(0xFFFC) | (nesPeek(0xFFFD) << 8); nes::STP = 0xFD; nes::SR |= 0x04; break;
    case PLATFORM_MSX:    msxResetReq = true; break;   // Z80+VDP+PPI+PSG reset (keeps RAM); serviced by msxLoop
    default: break;
  }
}

// ================================ platform control ================================================
// EMU_PLATFORM string the eprom.cpp parser accepts (so a re-exec boots this platform + skips splash).
static const char *platEnvName(int p) {
  switch (p) {
    case PLATFORM_APPLE2: return "apple2"; case PLATFORM_C64:  return "c64";
    case PLATFORM_NES:    return "nes";    case PLATFORM_ATARI: return "atari";
    case PLATFORM_IIGS:   return "iigs";   case PLATFORM_MSX:   return "msx";
    case PLATFORM_SMS:    return "sms";    case PLATFORM_PCXT:  return "pcxt";
    case PLATFORM_TINY386: return "tiny386"; default: return "apple2";
  }
}
static void rebootInto(int p) {
  desktopUiSaveState();    // CRITICAL: persist window positions + settings BEFORE the process re-execs
  static char buf[40];
  snprintf(buf, sizeof(buf), "EMU_PLATFORM=%s", platEnvName(p));
  putenv(buf);             // re-exec (below) inherits the env -> boots p, skips the splash
  ESP.restart();
}

int dbgPlatform()      { return currentPlatform; }
int dbgPlatformCount() { return PLATFORM_TINY386 + 1; }
const char *dbgPlatformName(int p) {
  switch (p) {
    case PLATFORM_APPLE2: return "Apple II"; case PLATFORM_C64:  return "Commodore 64";
    case PLATFORM_NES:    return "NES";      case PLATFORM_ATARI: return "Atari 2600";
    case PLATFORM_IIGS:   return "Apple IIGS"; case PLATFORM_MSX: return "MSX";
    case PLATFORM_SMS:    return "Master System"; case PLATFORM_PCXT: return "PC-XT (8086)";
    case PLATFORM_TINY386: return "PC (i386)"; default: return "?";
  }
}
void dbgSwitchPlatform(int p) {
  if (p < 0 || p > PLATFORM_TINY386 || p == currentPlatform) return;
  currentPlatform = p;
  saveConfig();            // persist so the re-exec'd setup() inits the chosen core
  rebootInto(p);
}

// Mount/load an SD file for the current platform. Most cores hot-load (no reboot); Apple II/IIGS
// re-exec so their boot path mounts the image (their disk path can't safely hot-swap mid-run).
bool dbgLoadFile(const char *path) {
  bool ok = false;
  switch (currentPlatform) {
    case PLATFORM_C64:     ok = c64LoadSelected(path); break;
    case PLATFORM_NES:     ok = nesLoadSelected(path); break;
    case PLATFORM_ATARI:   ok = atariLoadSelected(path); break;
    case PLATFORM_MSX:     ok = msxLoadSelected(path); break;
    case PLATFORM_SMS:     ok = smsLoadSelected(path); break;
    case PLATFORM_PCXT:    ok = pcxtMountAuto(path); break;   // floppy -> A:, hard disk (e.g. DOSHDD.IMG) -> C:
    case PLATFORM_TINY386: ok = tiny386MountC(path); break;
    case PLATFORM_IIGS:    iigsLoadDisk(path); return true;   // reboots internally (persists first)
    case PLATFORM_APPLE2:
    default:
      apple2InsertDisk(path);   // hot-swap, NO reboot — the running software reads it on next access
      ok = true; break;
  }
  if (ok) saveConfig();   // persist the just-loaded image so it auto-mounts next boot
  return ok;
}
const char *dbgFileExts() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2:  return "dsk nib do po woz 2mg hdv img";
    case PLATFORM_C64:     return "d64 prg t64 crt";
    case PLATFORM_NES:     return "nes";
    case PLATFORM_ATARI:   return "a26 bin";
    case PLATFORM_IIGS:    return "dsk po 2mg hdv";
    case PLATFORM_MSX:     return "rom dsk mx1 mx2";
    case PLATFORM_SMS:     return "sms bin";
    case PLATFORM_PCXT:    return "img dsk ima";
    case PLATFORM_TINY386: return "img vhd";
    default: return "";
  }
}

// The PC platforms expose two drive slots (A: floppy + C: hard disk) the UI can target separately.
bool dbgHasDriveSlots() { return currentPlatform == PLATFORM_PCXT || currentPlatform == PLATFORM_TINY386; }

// Mount `path` into a specific slot (0 = A: floppy, 1 = C: hard disk, -1 = auto-by-size). Lets the
// file browser put one image in A: and another in C:. Non-PC platforms ignore the slot.
bool dbgLoadFileToSlot(const char *path, int slot) {
  bool ok = false;
  switch (currentPlatform) {
    case PLATFORM_PCXT:
      if (slot == 0)      ok = pcxtMountA(path);                       // A: floppy (live media change)
      else if (slot == 1) { ok = pcxtMountC(path); if (ok) pcxtHardReset(); }  // C: HD: re-POST so the BIOS detects it
      else                ok = pcxtMountAuto(path);                    // auto-by-size (also re-POSTs for a HD)
      break;
    case PLATFORM_TINY386:
      ok = (slot == 0) ? tiny386MountA(path) : tiny386MountC(path);    // -1/1 -> C: (MountC re-POSTs internally)
      break;
    default:
      return dbgLoadFile(path);                                        // single-slot platforms
  }
  if (ok) saveConfig();   // persist so the slot auto-mounts next boot
  return ok;
}

// The image currently set in a slot (0 = A: floppy, 1 = C: hard disk), or "" if none. The file
// browser shows an [A:] / [C:] marker in front of the matching image.
const char *dbgMountedSlotPath(int slot) {
  switch (currentPlatform) {
    case PLATFORM_PCXT:    return (slot == 0 ? selectedPcFileName     : selectedPcHdFileName ).c_str();
    case PLATFORM_TINY386: return (slot == 0 ? selectedTiny386FileNameA : selectedTiny386FileName).c_str();
    default: return "";
  }
}

// Eject/unmount the image in a slot (0 = A: floppy, 1 = C: hard disk). A: ejects live; C: re-POSTs so
// the machine no longer sees the hard disk.
void dbgEjectSlot(int slot) {
  switch (currentPlatform) {
    case PLATFORM_PCXT:
      pcxtUnmount(slot == 0 ? 0 : 2);            // machine slots: 0 = A:, 2 = C:
      if (slot == 1) pcxtHardReset();            // re-POST so the BIOS drops the C: drive
      break;
    case PLATFORM_TINY386:
      if (slot == 0) tiny386MountA(""); else tiny386MountC("");   // "" = eject (MountC re-POSTs internally)
      break;
    default: return;
  }
  saveConfig();
}

// ================================ Apple II (6502) =================================================
// Side-effect-free read: mirrors read8()'s RAM/ROM decode but NEVER touches the $C000-$C0FF
// soft-switches or the $C100-$CFFF slot-ROM bank flags (which a memory viewer must not perturb).
static uint8_t a2peek(uint16_t a) {
  if (a < 0x0200) return AltZPOn_Off ? auxzp[a] : zp[a];
  if (a < 0xC000) {
    if (!Store80On_Off) return RAMReadOn_Off ? auxram[a] : ram[a];
    if (a >= 0x0400 && a < 0x0800) return (!Page1_Page2) ? auxram[a] : ram[a];     // text page
    if (a >= 0x2000 && a < 0x4000) {                                               // graphics page
      if (LoRes_HiRes)            return RAMReadOn_Off ? auxram[a] : ram[a];
      return (!Page1_Page2) ? auxram[a] : ram[a];
    }
    return RAMReadOn_Off ? auxram[a] : ram[a];
  }
  if (a < 0xD000) return 0;            // $C000-$CFFF: I/O + slot ROM — unsafe to read, shown as 0
  return read8(a);                     // $D000-$FFFF: language-card / ROM — side-effect-free
}
static void a2poke(uint16_t a, uint8_t v) {
  if (a < 0x0200) { (AltZPOn_Off ? auxzp : zp)[a] = v; return; }
  if (a < 0xC000) {
    if (!Store80On_Off) { (RAMReadOn_Off ? auxram : ram)[a] = v; return; }
    if (a >= 0x0400 && a < 0x0800) { ((!Page1_Page2) ? auxram : ram)[a] = v; return; }
    if (a >= 0x2000 && a < 0x4000) {
      if (LoRes_HiRes) { (RAMReadOn_Off ? auxram : ram)[a] = v; return; }
      ((!Page1_Page2) ? auxram : ram)[a] = v; return;
    }
    (RAMReadOn_Off ? auxram : ram)[a] = v; return;
  }
  // $C000+ (I/O / ROM): ignore pokes
}

// ================================ Commodore 64 (6510) ============================================
// Side-effect-free read mirroring c64::read8()'s banking, but NEVER clearing the read-once VIC
// latches ($D019/$D01E/$D01F) or touching the CIA/SID (which a memory viewer must not perturb).
// VIC + colour RAM come from their shadow arrays; SID / CIA / cartridge I/O read back as 0.
static uint8_t c64peek(uint16_t a) {
  if (!c64::ram) return 0;
  if (a == 0x0001) return c64::register1;
  if (c64::cartActive && a >= 0x8000) {
    bool ultimax = c64::cartExrom && !c64::cartGame;
    if (ultimax) {
      if (c64::cartROML && a <= 0x9fff) return c64::cartROML[a - 0x8000];
      if (c64::cartROMH && a >= 0xe000) return c64::cartROMH[a - 0xe000];
      if (a >= 0xa000 && a <= 0xcfff)   return c64::ram[a];
    } else {
      bool loram = c64::register1 & 1, hiram = c64::register1 & 2;
      if (c64::cartROML && !c64::cartExrom && loram && hiram && a <= 0x9fff) return c64::cartROML[a - 0x8000];
      if (c64::cartROMH && !c64::cartGame && hiram && a >= 0xa000 && a <= 0xbfff) return c64::cartROMH[a - 0xa000];
    }
  }
  if (!c64::bankARAM && a >= 0xa000 && a <= 0xbfff) return basic_rom  ? basic_rom[a - 0xa000]  : c64::ram[a];
  if (!c64::bankERAM && a >= 0xe000)               return kernal_rom ? kernal_rom[a - 0xe000] : c64::ram[a];
  if (c64::bankDIO && a >= 0xd000 && a <= 0xdfff) {
    if (a <= 0xd3ff) return c64::vicreg[(a - 0xd000) % 0x40];              // VIC (no read-latch clear)
    if (a <= 0xd7ff) return 0;                                            // SID: side-effect-free -> 0
    if (a <= 0xdbff) return c64::colormap ? c64::colormap[a - 0xd800] : 0; // colour RAM
    return 0;                                                            // CIA1 / CIA2 / cart I/O
  }
  if (!c64::bankDRAM && a >= 0xd000 && a <= 0xdfff) return charset_rom ? charset_rom[a - 0xd000] : c64::ram[a];
  return c64::ram[a];
}
// The C64 always has RAM under the ROM / I-O banks, so writes land in RAM (matching where real
// writes fall through) — except $01 (the CPU port), which we re-decode so banking updates live.
// The VIC / colour-RAM shadows are written when visible; CIA / SID are left untouched.
static void c64poke(uint16_t a, uint8_t v) {
  if (!c64::ram) return;
  if (a == 0x0001) { c64::register1 = v; c64::decodeRegister1(c64::register1 & 7); return; }
  if (c64::bankDIO && a >= 0xd000 && a <= 0xd3ff) { c64::vicreg[(a - 0xd000) % 0x40] = v; return; }
  if (c64::bankDIO && a >= 0xd800 && a <= 0xdbff) { if (c64::colormap) c64::colormap[a - 0xd800] = v; return; }
  c64::ram[a] = v;   // RAM under ROM / non-shadow I/O
}

// ================================ NES (Ricoh 2A03) ===============================================
// Side-effect-free read mirroring the CPU bus, but NEVER touching the PPU/APU/controller registers
// (reading $2002 would clear the VBlank flag + scroll latch; $4016 would advance the pad shift reg).
// RAM, PRG-RAM and PRG-ROM read back real bytes; the $2000-$401F I/O window reads as 0 (shown blank,
// like the Apple II $C0xx range).
static uint8_t nesPeek(uint16_t a) {
  if (!nes::cpuRam) return 0;
  if (a < 0x2000) return nes::cpuRam[a & 0x07FF];      // 2K RAM, mirrored to $1FFF
  if (a < 0x4020) return 0;                            // PPU/APU/controller I/O: side-effect-free -> 0
  if (a < 0x6000) return 0;                            // $4020-$5FFF expansion (open bus)
  if (a < 0x8000) return nes::prgRam ? nes::prgRam[a - 0x6000] : 0;
  uint8_t *p = nes::prgMap[(a >> 13) & 3];             // $8000-$FFFF: one of four 8K mapper windows
  return p ? p[a & 0x1FFF] : 0;
}
// Writes land in RAM / PRG-RAM where a real write would fall; PPU/APU I/O and ROM are left untouched
// (a memory editor must not trip PPU/mapper registers or scribble the cartridge image).
static void nesPoke(uint16_t a, uint8_t v) {
  if (!nes::cpuRam) return;
  if (a < 0x2000) { nes::cpuRam[a & 0x07FF] = v; return; }
  if (a >= 0x6000 && a < 0x8000) { if (nes::prgRam) nes::prgRam[a - 0x6000] = v; return; }
  // $2000-$401F (I/O) and $8000+ (PRG ROM): ignore
}

// ---- PPU / VRAM inspector facade (NES) — the analog of the C64 VIC-II panel ----
bool dbgNesPpuSupported() { return currentPlatform == PLATFORM_NES; }
int dbgNesReadPalette(uint8_t *out, int max) {       // 32 palette-RAM bytes ($3F00-$3F1F)
  if (currentPlatform != PLATFORM_NES) return 0;
  int n = max < 0x20 ? max : 0x20;
  for (int i = 0; i < n; i++) out[i] = nes::paletteRam[i];
  return n;
}
uint32_t dbgNesMasterRGBA(int idx) {                 // master palette[idx&63] -> ABGR8888 (for swatches)
  uint16_t c = nes::nesPalette[idx & 0x3F];
  uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
  uint8_t g = (uint8_t)(((c >> 5)  & 0x3F) * 255 / 63);
  uint8_t b = (uint8_t)(( c        & 0x1F) * 255 / 31);
  return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}
int dbgNesReadOam(uint8_t *out, int max) {           // 256 OAM bytes (64 sprites x 4)
  if (currentPlatform != PLATFORM_NES) return 0;
  int n = max < 0x100 ? max : 0x100;
  for (int i = 0; i < n; i++) out[i] = nes::oam[i];
  return n;
}
void dbgNesRenderPattern(int half, int pal4, uint32_t *out) {   // -> 128x128 ABGR8888
  if (currentPlatform == PLATFORM_NES) nes::dbgRenderPatternTable(half, pal4, out);
}
void dbgNesRenderNametables(uint32_t *out) {                    // -> 512x480 ABGR8888
  if (currentPlatform == PLATFORM_NES) nes::dbgRenderNametables(out);
}
void dbgNesViewport(int *x, int *y) {                // scroll top-left in the 512x480 nametable space
  uint8_t ct = 0, mk = 0, st = 0, fx = 0; int sl = 0; uint16_t v = 0;
  nes::dbgPpuSnapshot(&ct, &mk, &st, &sl, &v, &fx);
  if (x) *x = (int)(((v >> 10) & 1) * 256 + (v & 0x1F) * 8 + fx);          // NT-X + coarse X*8 + fine X
  if (y) *y = (int)(((v >> 11) & 1) * 240 + ((v >> 5) & 0x1F) * 8 + ((v >> 12) & 7)); // NT-Y + coarseY*8 + fineY
}

// ================================ MSX1 (Zilog Z80) ===============================================
// Side-effect-free read mirroring memRead8()'s primary-slot decode, but using diskPeek() for slot 2
// so the viewer never advances the WD2793 FDC data transfer or ticks its seek timer. ROM/cartridge
// reads are already side-effect-free; RAM reads return the byte.
static uint8_t msxPeek(uint16_t a) {
  if (!msx::ram) return 0xFF;
  switch (msx::ppiPageSlot(a >> 14)) {
    case 0:  return (a < (uint16_t)msx::biosLen && msx::bios) ? msx::bios[a] : 0xFF;   // BIOS ROM
    case 1:  return msx::cartRead(1, a);                                               // cartridge
    case 2:  return msx::diskPeek(a);                                                  // disk ROM + FDC (no advance)
    default: return msx::ram[a];                                                       // slot 3 = RAM
  }
}
// Writes land in slot-3 RAM where a real write would fall; ROM / cartridge / disk pages are left
// untouched (a memory editor must not trip a cartridge mapper bank-select or an FDC command).
static void msxPoke(uint16_t a, uint8_t v) {
  if (!msx::ram) return;
  if (msx::ppiPageSlot(a >> 14) == 3) msx::ram[a] = v;
}

// ---- VDP / VRAM inspector facade (MSX) — the analog of the NES PPU panel ----
// The TMS9918 VRAM is a separate 16K array (msx::vram), reached over ports $98/$99, so the CPU-space
// Memory panel can't show it. These give the UI direct (read-only-safe) access to it.
bool dbgMsxVdpSupported() { return currentPlatform == PLATFORM_MSX; }
uint8_t dbgMsxPeekVram(uint32_t a) { return msx::vram ? msx::vram[a & 0x3FFF] : 0xFF; }
void dbgMsxPokeVram(uint32_t a, uint8_t v) { if (msx::vram) msx::vram[a & 0x3FFF] = v; }   // shows next frame
int dbgMsxVdpRegs(uint8_t *out, int max) {                  // R0-R7 (the caller derives the table bases)
  if (currentPlatform != PLATFORM_MSX) return 0;
  int n = max < 8 ? max : 8;
  for (int i = 0; i < n; i++) out[i] = msx::vdpRegister(i);
  return n;
}
uint32_t dbgMsxPaletteRGBA(int idx) {                       // fixed 16-colour TMS9918 palette (RGB565 -> ABGR8888)
  uint16_t c = msx::MSX_PALETTE[idx & 0x0F];
  uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
  uint8_t g = (uint8_t)(((c >> 5)  & 0x3F) * 255 / 63);
  uint8_t b = (uint8_t)(( c        & 0x1F) * 255 / 31);
  return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}
// Render the pattern-generator table as a 16x16 grid of 8x8 tiles (128x128 ABGR8888), monochrome
// (set bit = white, clear = dark) so the font/tiles are visible regardless of the per-tile colour
// table. In Graphic 2 the generator is 768 patterns in 3 banks of 256; `bank` (0-2) picks one.
void dbgMsxRenderPatterns(uint32_t *out, int bank) {
  if (!msx::vram) { memset(out, 0, 128 * 128 * sizeof(uint32_t)); return; }
  uint8_t r0 = msx::vdpRegister(0), r4 = msx::vdpRegister(4);
  bool g2 = (r0 & 0x02) != 0;
  uint16_t base = g2 ? ((r4 & 0x04) ? 0x2000 : 0x0000) : (uint16_t)((r4 & 0x07) << 11);
  if (g2) { if (bank < 0) bank = 0; if (bank > 2) bank = 2; base = (uint16_t)((base + bank * 0x800) & 0x3FFF); }
  const uint32_t fg = dbgMsxPaletteRGBA(15), bg = 0xFF202020u;       // white on dark grey
  for (int t = 0; t < 256; t++) {
    int tx = (t & 15) * 8, ty = (t >> 4) * 8;
    for (int row = 0; row < 8; row++) {
      uint8_t pat = msx::vram[(base + t * 8 + row) & 0x3FFF];
      uint32_t *line = out + (ty + row) * 128 + tx;
      for (int px = 0; px < 8; px++) line[px] = (pat & (0x80 >> px)) ? fg : bg;
    }
  }
}

// ================================ CPU identity / registers ========================================
const char *dbgCpuName() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: return "MOS 6502";
    case PLATFORM_C64:    return "MOS 6510";
    case PLATFORM_NES:    return "Ricoh 2A03";
    case PLATFORM_ATARI:  return "MOS 6507";
    case PLATFORM_IIGS:   return "WDC 65C816";
    case PLATFORM_MSX:    return "Zilog Z80";
    case PLATFORM_SMS:    return "Zilog Z80";
    case PLATFORM_PCXT:   return "Intel 8086";
    case PLATFORM_TINY386: return "Intel i386";
    default: return "(unsupported)";
  }
}

int dbgGetRegs(DbgReg *o, int max) {
  int n = 0;
  auto add = [&](const char *nm, uint32_t v, uint8_t bits) { if (n < max) { o[n++] = {nm, v, bits}; } };
  switch (currentPlatform) {
    case PLATFORM_APPLE2:
      add("PC", PC, 16); add("A", A, 8); add("X", X, 8); add("Y", Y, 8);
      add("SP", 0x0100 | STP, 16); add("P", SR, 8);
      break;
    case PLATFORM_C64:
      add("PC", c64::PC, 16); add("A", c64::A, 8); add("X", c64::X, 8); add("Y", c64::Y, 8);
      add("SP", 0x0100 | c64::STP, 16); add("P", c64::SR, 8);
      break;
    case PLATFORM_NES:        // Ricoh 2A03 = NMOS 6502 register file
      add("PC", nes::PC, 16); add("A", nes::A, 8); add("X", nes::X, 8); add("Y", nes::Y, 8);
      add("SP", 0x0100 | nes::STP, 16); add("P", nes::SR, 8);
      break;
    case PLATFORM_MSX: {      // Zilog Z80: 16-bit pairs + index regs + I/R + the alternate set
      Z80 &z = msx::cpu;
      add("PC", z.PC, 16); add("SP", z.SP, 16);
      add("AF", z.AF(), 16); add("BC", z.BC(), 16); add("DE", z.DE(), 16); add("HL", z.HL(), 16);
      add("IX", z.IX(), 16); add("IY", z.IY(), 16);
      add("I", z.I, 8); add("R", (uint8_t)((z.R & 0x7F) | (z.R7 & 0x80)), 8);
      add("AF'", (z.A_ << 8) | z.F_, 16); add("BC'", (z.B_ << 8) | z.C_, 16);
      add("DE'", (z.D_ << 8) | z.E_, 16); add("HL'", (z.H_ << 8) | z.L_, 16);
      break;
    }
    default: break;   // other platforms: registers not yet wired
  }
  return n;
}

uint32_t dbgGetPC() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: return PC;
    case PLATFORM_C64:    return c64::PC;
    case PLATFORM_NES:    return nes::PC;
    case PLATFORM_MSX:    return msx::cpu.PC;
    default: return 0;
  }
}

// The Z80 (MSX) has a full 16-bit downward-growing stack; the 6502 cores use the fixed page-1 stack.
bool dbgStack16() { return currentPlatform == PLATFORM_MSX; }

bool dbgGetFlags(const char *const **labels, uint32_t *value, int *count) {
  static const char *const p6502[8] = {"N","V","-","B","D","I","Z","C"};   // MSB..LSB
  switch (currentPlatform) {
    case PLATFORM_APPLE2:
      if (labels) *labels = p6502;
      if (value)  *value  = SR;
      if (count)  *count  = 8;
      return true;
    case PLATFORM_C64:                       // 6510: same NMOS-6502 status layout
      if (labels) *labels = p6502;
      if (value)  *value  = c64::SR;
      if (count)  *count  = 8;
      return true;
    case PLATFORM_NES:                       // 2A03: same NMOS-6502 status layout
      if (labels) *labels = p6502;
      if (value)  *value  = nes::SR;
      if (count)  *count  = 8;
      return true;
    case PLATFORM_MSX: {                      // Z80 F register: S Z Y H X P/V N C (bit7..bit0)
      static const char *const pz80[8] = {"S","Z","Y","H","X","P","N","C"};
      if (labels) *labels = pz80;
      if (value)  *value  = msx::cpu.F;
      if (count)  *count  = 8;
      return true;
    }
    default: return false;
  }
}

// ================================ memory ==========================================================
bool dbgMemReadable() {
  return currentPlatform == PLATFORM_APPLE2 || currentPlatform == PLATFORM_C64
      || currentPlatform == PLATFORM_NES    || currentPlatform == PLATFORM_MSX;   // extended as cores are wired
}
uint32_t dbgMemSize() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: case PLATFORM_C64: case PLATFORM_NES:
    case PLATFORM_MSX: return 0x10000;   // 64K CPU address space
    default: return 0;
  }
}
uint8_t dbgPeek(uint32_t a) {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: return a2peek((uint16_t)a);
    case PLATFORM_C64:    return c64peek((uint16_t)a);
    case PLATFORM_NES:    return nesPeek((uint16_t)a);
    case PLATFORM_MSX:    return msxPeek((uint16_t)a);
    default: return 0;
  }
}
bool dbgPokeSupported() {
  return currentPlatform == PLATFORM_APPLE2 || currentPlatform == PLATFORM_C64
      || currentPlatform == PLATFORM_NES    || currentPlatform == PLATFORM_MSX;
}
void dbgPoke(uint32_t a, uint8_t v) {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: a2poke((uint16_t)a, v); break;
    case PLATFORM_C64:    c64poke((uint16_t)a, v); break;
    case PLATFORM_NES:    nesPoke((uint16_t)a, v); break;
    case PLATFORM_MSX:    msxPoke((uint16_t)a, v); break;
    default: break;
  }
}

// ================================ breakpoints =====================================================
// Apple II and C64 cpuLoops carry the dbgBpShouldBreak() hook; extending to NES/Atari is the same
// one-line add in their cpuLoop pause spins.
bool dbgBpSupported() {
  return currentPlatform == PLATFORM_APPLE2 || currentPlatform == PLATFORM_C64
      || currentPlatform == PLATFORM_NES    || currentPlatform == PLATFORM_MSX;
}
bool dbgBpAt(uint32_t a) { return g_dbgBp[a & 0xFFFF]; }
void dbgBpToggle(uint32_t a) {
  a &= 0xFFFF;
  g_dbgBp[a] = !g_dbgBp[a];
  if (g_dbgBp[a]) g_dbgBpAny = true;
  else { g_dbgBpAny = false; for (int i = 0; i < 0x10000; i++) if (g_dbgBp[i]) { g_dbgBpAny = true; break; } }
}
void dbgBpClearAll() { for (int i = 0; i < 0x10000; i++) g_dbgBp[i] = false; g_dbgBpAny = false; }
int dbgBpList(uint16_t *out, int max) {
  int n = 0;
  for (int i = 0; i < 0x10000 && n < max; i++) if (g_dbgBp[i]) out[n++] = (uint16_t)i;
  return n;
}

// ================================ watchpoints =====================================================
bool dbgWatchSupported() {   // needs the bus hooks — NES cpuRead/cpuWrite + MSX memRead8/memWrite8 carry dbgBusTouch
  return currentPlatform == PLATFORM_APPLE2 || currentPlatform == PLATFORM_C64
      || currentPlatform == PLATFORM_NES    || currentPlatform == PLATFORM_MSX;
}
uint8_t dbgWatchAt(uint32_t a) { return g_dbgWatch[a & 0xFFFF]; }
void dbgWatchToggle(uint32_t a, uint8_t kindMask) {
  a &= 0xFFFF;
  g_dbgWatch[a] ^= kindMask;
  g_dbgWatchAny = false;
  for (int i = 0; i < 0x10000; i++) if (g_dbgWatch[i]) { g_dbgWatchAny = true; break; }
}
void dbgWatchClearAll() { for (int i = 0; i < 0x10000; i++) g_dbgWatch[i] = 0; g_dbgWatchAny = false; g_dbgWatchHit = -1; }

// ================================ I/O / soft-switch state =========================================
// Apple II soft switches. Polarity = the flags' "true = first token" convention (verified against
// memory.cpp read8 + video.cpp render conditions). `active` highlights the non-default state.
// C64 banking + VIC state. The $01 CPU port (LORAM/HIRAM/CHAREN) decides which of BASIC/KERNAL/IO/
// CHAR-ROM is visible; the VIC sees one of four 16K banks (CIA2 port A). `active` flags a ROM/I-O
// that is currently mapped in (i.e. overriding RAM). Static scratch is fine: rebuilt on the UI thread.
static int c64IoState(DbgFlag *o, int max) {
  int n = 0;
  static char sReg[8], sRaster[8], sVic[16];
  auto add = [&](const char *lbl, const char *val, bool act) { if (n < max) o[n++] = {lbl, val, act}; };
  uint8_t r1 = c64::register1;
  snprintf(sReg, sizeof(sReg), "$%02X", r1);
  add("CPU port $01", sReg, false);
  add("LORAM",  (r1 & 1) ? "1" : "0", !(r1 & 1));
  add("HIRAM",  (r1 & 2) ? "1" : "0", !(r1 & 2));
  add("CHAREN", (r1 & 4) ? "1" : "0", !(r1 & 4));
  add("BASIC $A000",  !c64::bankARAM ? "ROM" : "RAM", !c64::bankARAM);
  add("KERNAL $E000", !c64::bankERAM ? "ROM" : "RAM", !c64::bankERAM);
  add("$D000",  c64::bankDIO ? "I/O" : (!c64::bankDRAM ? "CHAR ROM" : "RAM"), c64::bankDIO);
  int vbank = (c64::vicmem >> 14) & 3;
  snprintf(sVic, sizeof(sVic), "%d ($%04X)", vbank, c64::vicmem);
  add("VIC bank", sVic, false);
  snprintf(sRaster, sizeof(sRaster), "%u", (unsigned)c64::rasterline);
  add("Raster", sRaster, false);
  const char *cart = "none";
  if (c64::cartActive) cart = (c64::cartExrom && !c64::cartGame) ? "Ultimax"
                            : (!c64::cartExrom && !c64::cartGame) ? "16K" : "8K";
  add("Cartridge", cart, c64::cartActive);
  return n;
}

// NES PPU + cartridge state — the "I/O" panel analog (Apple II soft switches / C64 banking). PPU
// registers are read via the snapshot accessor (no $2002 side effect). `active` flags a non-idle
// state (NMI armed, rendering off, a status latch set) to draw the eye to it. Static scratch is fine:
// rebuilt on the UI thread each call.
static int nesIoState(DbgFlag *o, int max) {
  int n = 0;
  static char sCtrl[8], sMask[8], sStat[8], sLine[8], sScroll[16], sMap[12];
  auto add = [&](const char *lbl, const char *val, bool act) { if (n < max) o[n++] = {lbl, val, act}; };
  uint8_t ctrl = 0, mask = 0, status = 0, finex = 0; int scan = 0; uint16_t v = 0;
  nes::dbgPpuSnapshot(&ctrl, &mask, &status, &scan, &v, &finex);
  snprintf(sCtrl, sizeof sCtrl, "$%02X", ctrl); add("PPUCTRL $2000", sCtrl, false);
  add("NMI on VBlank", (ctrl & 0x80) ? "ON" : "off", ctrl & 0x80);
  add("Sprite size",   (ctrl & 0x20) ? "8x16" : "8x8", ctrl & 0x20);
  add("BG pattern",    (ctrl & 0x10) ? "$1000" : "$0000", false);
  add("Spr pattern",   (ctrl & 0x08) ? "$1000" : "$0000", false);
  snprintf(sMask, sizeof sMask, "$%02X", mask); add("PPUMASK $2001", sMask, false);
  add("Rendering",     (mask & 0x18) ? "ON" : "off", !(mask & 0x18));
  add("Show BG",       (mask & 0x08) ? "ON" : "off", false);
  add("Show sprites",  (mask & 0x10) ? "ON" : "off", false);
  snprintf(sStat, sizeof sStat, "$%02X", status); add("PPUSTATUS $2002", sStat, false);
  add("VBlank",        (status & 0x80) ? "1" : "0", status & 0x80);
  add("Sprite 0 hit",  (status & 0x40) ? "1" : "0", status & 0x40);
  add("Sprite ovfl",   (status & 0x20) ? "1" : "0", status & 0x20);
  snprintf(sLine, sizeof sLine, "%d", scan); add("Scanline", sLine, false);
  int coarseX = v & 0x1F, coarseY = (v >> 5) & 0x1F, fineY = (v >> 12) & 7;
  snprintf(sScroll, sizeof sScroll, "%d,%d", coarseX * 8 + finex, coarseY * 8 + fineY);
  add("Scroll x,y", sScroll, false);
  snprintf(sMap, sizeof sMap, "%u", (unsigned)nes::mapperNum); add("Mapper", sMap, false);
  const char *mir = nes::mirrorMode == 0 ? "Horizontal" : nes::mirrorMode == 1 ? "Vertical"
                  : nes::mirrorMode == 2 ? "Single 0"   : "Single 1";
  add("Mirroring", mir, false);
  add("CHR", nes::chrIsRam ? "RAM" : "ROM", nes::chrIsRam);
  return n;
}

// MSX1 state — the "I/O" panel analog: which primary slot each 16K page sees (the live memory map),
// the loaded media, and the TMS9918 VDP mode + register file. `active` flags a non-default state
// (display off, an IRQ armed). Static scratch is fine: rebuilt on the UI thread each call.
static int msxIoState(DbgFlag *o, int max) {
  int n = 0;
  static char sSlot[4][20], sReg[8][6], sMode[16], sSpr[12];
  auto add = [&](const char *lbl, const char *val, bool act) { if (n < max) o[n++] = {lbl, val, act}; };
  static const char *slotName[4] = {"BIOS", "cart", "disk", "RAM"};
  for (int pg = 0; pg < 4; pg++) {
    int s = msx::ppiPageSlot(pg) & 3;
    snprintf(sSlot[pg], sizeof sSlot[pg], "slot %d (%s)", s, slotName[s]);
  }
  add("Page 0 $0000", sSlot[0], false);  add("Page 1 $4000", sSlot[1], false);
  add("Page 2 $8000", sSlot[2], false);  add("Page 3 $C000", sSlot[3], false);
  add("Cartridge", msx::cartPresent(1) ? "yes" : "no", msx::cartPresent(1));
  add("Disk", msx::diskPresent() ? "mounted" : "none", msx::diskPresent());

  uint8_t r0 = msx::vdpRegister(0), r1 = msx::vdpRegister(1);
  bool m1 = r1 & 0x10, m2 = r1 & 0x08, m3 = r0 & 0x02;          // mode-select bits (TMS9918)
  snprintf(sMode, sizeof sMode, "%s", m1 ? "Text 1 (40)" : m2 ? "Multicolor" : m3 ? "Graphic 2" : "Graphic 1 (32)");
  add("VDP mode", sMode, false);
  add("Display",   (r1 & 0x40) ? "ON" : "off", !(r1 & 0x40));   // R1 bit6 = blank/enable
  add("VBlank IRQ", (r1 & 0x20) ? "ON" : "off", false);         // R1 bit5 = interrupt enable
  snprintf(sSpr, sizeof sSpr, "%s%s", (r1 & 0x02) ? "16x16" : "8x8", (r1 & 0x01) ? " x2" : "");
  add("Sprites", sSpr, false);
  for (int r = 0; r < 8; r++) snprintf(sReg[r], sizeof sReg[r], "$%02X", msx::vdpRegister(r));
  add("VDP R0", sReg[0], false);            add("VDP R1", sReg[1], false);
  add("VDP R2 (name)", sReg[2], false);     add("VDP R3 (color)", sReg[3], false);
  add("VDP R4 (patt)", sReg[4], false);     add("VDP R5 (spr attr)", sReg[5], false);
  add("VDP R6 (spr patt)", sReg[6], false); add("VDP R7 (border)", sReg[7], false);
  return n;
}

int dbgGetIoState(DbgFlag *o, int max) {
  if (currentPlatform == PLATFORM_C64) return c64IoState(o, max);
  if (currentPlatform == PLATFORM_NES) return nesIoState(o, max);
  if (currentPlatform == PLATFORM_MSX) return msxIoState(o, max);
  if (currentPlatform != PLATFORM_APPLE2) return 0;
  int n = 0;
  auto add = [&](const char *lbl, const char *val, bool act) { if (n < max) o[n++] = {lbl, val, act}; };
  add("Model",     AppleIIe ? "IIe" : "II+", false);
  add("Mode",      Graphics_Text ? "GRAPHICS" : "TEXT", Graphics_Text);
  add("Display",   DisplayFull_Split ? "FULL" : "MIXED", !DisplayFull_Split);
  add("Page",      Page1_Page2 ? "1" : "2", !Page1_Page2);
  add("Res",       LoRes_HiRes ? "LORES" : "HIRES", false);
  add("DHGR",      DHiResOn_Off ? "ON" : "off", DHiResOn_Off);
  add("Columns",   Cols40_80 ? "40" : "80", !Cols40_80);
  add("80STORE",   Store80On_Off ? "ON" : "off", Store80On_Off);
  add("RAMRD",     RAMReadOn_Off ? "aux" : "main", RAMReadOn_Off);
  add("RAMWRT",    RAMWriteOn_Off ? "aux" : "main", RAMWriteOn_Off);
  add("ALTZP",     AltZPOn_Off ? "aux" : "main", AltZPOn_Off);
  add("INTCXROM",  IntCXRomOn_Off ? "internal" : "slot", IntCXRomOn_Off);
  add("SLOTC3ROM", SlotC3RomOn_Off ? "slot" : "internal", SlotC3RomOn_Off);
  // language card ($D000-$FFFF bank); IIe and II+ keep separate flags
  bool lcRead  = AppleIIe ? IIEMemoryBankReadRAM_ROM      : MemoryBankReadRAM_ROM;
  bool lcWrite = AppleIIe ? IIEMemoryBankWriteRAM_NoWrite : MemoryBankWriteRAM_NoWrite;
  bool lcBank1 = AppleIIe ? IIEMemoryBankBankSelect1_2    : MemoryBankBankSelect1_2;
  add("LC read",   lcRead ? "RAM" : "ROM", lcRead);
  add("LC write",  lcWrite ? "RAM" : "off", lcWrite);
  add("LC bank",   lcBank1 ? "1" : "2", false);
  return n;
}

// ================================ 6502 disassembler ===============================================
namespace {
enum AM { IMP, ACC, IMM, ZP, ZPX, ZPY, IZX, IZY, ABS, ABX, ABY, IND, REL };
struct Op { const char *m; uint8_t am; };
const Op OPS[256] = {
  /*00*/ {"BRK",IMP},{"ORA",IZX},{"???",IMP},{"???",IMP},{"???",IMP},{"ORA",ZP},{"ASL",ZP},{"???",IMP},{"PHP",IMP},{"ORA",IMM},{"ASL",ACC},{"???",IMP},{"???",IMP},{"ORA",ABS},{"ASL",ABS},{"???",IMP},
  /*10*/ {"BPL",REL},{"ORA",IZY},{"???",IMP},{"???",IMP},{"???",IMP},{"ORA",ZPX},{"ASL",ZPX},{"???",IMP},{"CLC",IMP},{"ORA",ABY},{"???",IMP},{"???",IMP},{"???",IMP},{"ORA",ABX},{"ASL",ABX},{"???",IMP},
  /*20*/ {"JSR",ABS},{"AND",IZX},{"???",IMP},{"???",IMP},{"BIT",ZP},{"AND",ZP},{"ROL",ZP},{"???",IMP},{"PLP",IMP},{"AND",IMM},{"ROL",ACC},{"???",IMP},{"BIT",ABS},{"AND",ABS},{"ROL",ABS},{"???",IMP},
  /*30*/ {"BMI",REL},{"AND",IZY},{"???",IMP},{"???",IMP},{"???",IMP},{"AND",ZPX},{"ROL",ZPX},{"???",IMP},{"SEC",IMP},{"AND",ABY},{"???",IMP},{"???",IMP},{"???",IMP},{"AND",ABX},{"ROL",ABX},{"???",IMP},
  /*40*/ {"RTI",IMP},{"EOR",IZX},{"???",IMP},{"???",IMP},{"???",IMP},{"EOR",ZP},{"LSR",ZP},{"???",IMP},{"PHA",IMP},{"EOR",IMM},{"LSR",ACC},{"???",IMP},{"JMP",ABS},{"EOR",ABS},{"LSR",ABS},{"???",IMP},
  /*50*/ {"BVC",REL},{"EOR",IZY},{"???",IMP},{"???",IMP},{"???",IMP},{"EOR",ZPX},{"LSR",ZPX},{"???",IMP},{"CLI",IMP},{"EOR",ABY},{"???",IMP},{"???",IMP},{"???",IMP},{"EOR",ABX},{"LSR",ABX},{"???",IMP},
  /*60*/ {"RTS",IMP},{"ADC",IZX},{"???",IMP},{"???",IMP},{"???",IMP},{"ADC",ZP},{"ROR",ZP},{"???",IMP},{"PLA",IMP},{"ADC",IMM},{"ROR",ACC},{"???",IMP},{"JMP",IND},{"ADC",ABS},{"ROR",ABS},{"???",IMP},
  /*70*/ {"BVS",REL},{"ADC",IZY},{"???",IMP},{"???",IMP},{"???",IMP},{"ADC",ZPX},{"ROR",ZPX},{"???",IMP},{"SEI",IMP},{"ADC",ABY},{"???",IMP},{"???",IMP},{"???",IMP},{"ADC",ABX},{"ROR",ABX},{"???",IMP},
  /*80*/ {"???",IMP},{"STA",IZX},{"???",IMP},{"???",IMP},{"STY",ZP},{"STA",ZP},{"STX",ZP},{"???",IMP},{"DEY",IMP},{"???",IMP},{"TXA",IMP},{"???",IMP},{"STY",ABS},{"STA",ABS},{"STX",ABS},{"???",IMP},
  /*90*/ {"BCC",REL},{"STA",IZY},{"???",IMP},{"???",IMP},{"STY",ZPX},{"STA",ZPX},{"STX",ZPY},{"???",IMP},{"TYA",IMP},{"STA",ABY},{"TXS",IMP},{"???",IMP},{"???",IMP},{"STA",ABX},{"???",IMP},{"???",IMP},
  /*A0*/ {"LDY",IMM},{"LDA",IZX},{"LDX",IMM},{"???",IMP},{"LDY",ZP},{"LDA",ZP},{"LDX",ZP},{"???",IMP},{"TAY",IMP},{"LDA",IMM},{"TAX",IMP},{"???",IMP},{"LDY",ABS},{"LDA",ABS},{"LDX",ABS},{"???",IMP},
  /*B0*/ {"BCS",REL},{"LDA",IZY},{"???",IMP},{"???",IMP},{"LDY",ZPX},{"LDA",ZPX},{"LDX",ZPY},{"???",IMP},{"CLV",IMP},{"LDA",ABY},{"TSX",IMP},{"???",IMP},{"LDY",ABX},{"LDA",ABX},{"LDX",ABY},{"???",IMP},
  /*C0*/ {"CPY",IMM},{"CMP",IZX},{"???",IMP},{"???",IMP},{"CPY",ZP},{"CMP",ZP},{"DEC",ZP},{"???",IMP},{"INY",IMP},{"CMP",IMM},{"DEX",IMP},{"???",IMP},{"CPY",ABS},{"CMP",ABS},{"DEC",ABS},{"???",IMP},
  /*D0*/ {"BNE",REL},{"CMP",IZY},{"???",IMP},{"???",IMP},{"???",IMP},{"CMP",ZPX},{"DEC",ZPX},{"???",IMP},{"CLD",IMP},{"CMP",ABY},{"???",IMP},{"???",IMP},{"???",IMP},{"CMP",ABX},{"DEC",ABX},{"???",IMP},
  /*E0*/ {"CPX",IMM},{"SBC",IZX},{"???",IMP},{"???",IMP},{"CPX",ZP},{"SBC",ZP},{"INC",ZP},{"???",IMP},{"INX",IMP},{"SBC",IMM},{"NOP",IMP},{"???",IMP},{"CPX",ABS},{"SBC",ABS},{"INC",ABS},{"???",IMP},
  /*F0*/ {"BEQ",REL},{"SBC",IZY},{"???",IMP},{"???",IMP},{"???",IMP},{"SBC",ZPX},{"INC",ZPX},{"???",IMP},{"SED",IMP},{"SBC",ABY},{"???",IMP},{"???",IMP},{"???",IMP},{"SBC",ABX},{"INC",ABX},{"???",IMP},
};
} // namespace

static int disasm6502(uint16_t pc, char *out, int n) {
  uint8_t op = dbgPeek(pc);
  const Op &o = OPS[op];
  uint8_t b1 = dbgPeek((pc + 1) & 0xFFFF), b2 = dbgPeek((pc + 2) & 0xFFFF);
  uint16_t w = (uint16_t)(b1 | (b2 << 8));
  switch (o.am) {
    case IMP: snprintf(out, n, "%s",          o.m);     return 1;
    case ACC: snprintf(out, n, "%s A",        o.m);     return 1;
    case IMM: snprintf(out, n, "%s #$%02X",   o.m, b1); return 2;
    case ZP:  snprintf(out, n, "%s $%02X",    o.m, b1); return 2;
    case ZPX: snprintf(out, n, "%s $%02X,X",  o.m, b1); return 2;
    case ZPY: snprintf(out, n, "%s $%02X,Y",  o.m, b1); return 2;
    case IZX: snprintf(out, n, "%s ($%02X,X)",o.m, b1); return 2;
    case IZY: snprintf(out, n, "%s ($%02X),Y",o.m, b1); return 2;
    case ABS: snprintf(out, n, "%s $%04X",    o.m, w);  return 3;
    case ABX: snprintf(out, n, "%s $%04X,X",  o.m, w);  return 3;
    case ABY: snprintf(out, n, "%s $%04X,Y",  o.m, w);  return 3;
    case IND: snprintf(out, n, "%s ($%04X)",  o.m, w);  return 3;
    case REL: { uint16_t t = (uint16_t)(pc + 2 + (int8_t)b1); snprintf(out, n, "%s $%04X", o.m, t); return 2; }
  }
  return 1;
}

// ================================ Z80 disassembler (MSX) ==========================================
// Algorithmic decode (the standard x/y/z/p/q decomposition), covering the unprefixed table, CB
// (rotate/bit), ED (extended), DD/FD (IX/IY) and DDCB/FDCB. Reads bytes via dbgPeek and returns the
// full instruction length (prefixes + displacement + immediates included), so the panel can advance.
namespace {
const char *Z80_R[8]   = {"B","C","D","E","H","L","(HL)","A"};
const char *Z80_RP[4]  = {"BC","DE","HL","SP"};
const char *Z80_RP2[4] = {"BC","DE","HL","AF"};
const char *Z80_CC[8]  = {"NZ","Z","NC","C","PO","PE","P","M"};
const char *Z80_ALU[8] = {"ADD A,","ADC A,","SUB ","SBC A,","AND ","XOR ","OR ","CP "};
const char *Z80_ROT[8] = {"RLC","RRC","RL","RR","SLA","SRA","SLL","SRL"};
const char *Z80_IM[8]  = {"0","0/1","1","2","0","0/1","1","2"};
} // namespace

static int disasmZ80(uint16_t pc, char *out, int n) {
  uint16_t p = pc;
  auto rb = [&]() -> uint8_t { return dbgPeek(p++); };
  auto rw = [&]() -> uint16_t { uint8_t lo = rb(); uint8_t hi = rb(); return (uint16_t)(lo | (hi << 8)); };

  const char *ix = nullptr;                          // null = HL; else "IX" / "IY"
  uint8_t op = rb();
  if (op == 0xDD) { ix = "IX"; op = rb(); }
  else if (op == 0xFD) { ix = "IY"; op = rb(); }

  bool haveDisp = false; int8_t disp = 0;            // index displacement, read once on first (IX+d) use
  auto getDisp = [&]() -> int8_t { if (!haveDisp) { disp = (int8_t)rb(); haveDisp = true; } return disp; };
  // 8-bit operand z, honoring an IX/IY prefix. memInsn = the instruction also uses memory via (HL),
  // in which case H/L stay real (LD H,(IX+d)) and only the (HL) slot becomes (IX+d).
  auto reg8 = [&](int z, char *dst, bool memInsn) {
    if (z == 6) { if (ix) { int8_t d = getDisp(); snprintf(dst, 24, "(%s%+d)", ix, (int)d); } else snprintf(dst, 24, "(HL)"); }
    else if (ix && (z == 4 || z == 5) && !memInsn) snprintf(dst, 24, "%s%c", ix, z == 4 ? 'H' : 'L');  // IXH/IXL (undoc)
    else snprintf(dst, 24, "%s", Z80_R[z]);
  };
  auto rpN  = [&](int i) -> const char * { return (ix && i == 2) ? ix : Z80_RP[i]; };
  auto rp2N = [&](int i) -> const char * { return (ix && i == 2) ? ix : Z80_RP2[i]; };

  if (op == 0xCB) {                                  // ---- CB / DDCB / FDCB: rotate / bit / res / set
    int8_t d = 0; bool ddcb = (ix != nullptr);
    if (ddcb) d = (int8_t)rb();                      // DDCB: displacement precedes the sub-opcode
    uint8_t cb = rb();
    int x = cb >> 6, y = (cb >> 3) & 7, z = cb & 7;
    char tgt[24];
    if (ddcb) snprintf(tgt, sizeof(tgt), "(%s%+d)", ix, (int)d); else snprintf(tgt, sizeof(tgt), "%s", Z80_R[z]);
    const char *also = (ddcb && z != 6) ? Z80_R[z] : nullptr;   // DDCB also copies the result to r[z] (undoc)
    switch (x) {
      case 0: if (also) snprintf(out, n, "%s %s,%s", Z80_ROT[y], tgt, also); else snprintf(out, n, "%s %s", Z80_ROT[y], tgt); break;
      case 1: snprintf(out, n, "BIT %d,%s", y, tgt); break;
      case 2: if (also) snprintf(out, n, "RES %d,%s,%s", y, tgt, also); else snprintf(out, n, "RES %d,%s", y, tgt); break;
      default: if (also) snprintf(out, n, "SET %d,%s,%s", y, tgt, also); else snprintf(out, n, "SET %d,%s", y, tgt); break;
    }
    return (int)(p - pc);
  }

  if (op == 0xED) {                                  // ---- ED: extended opcodes
    uint8_t e = rb();
    int x = e >> 6, y = (e >> 3) & 7, z = e & 7, py = y >> 1, q = y & 1;
    if (x == 1) {
      switch (z) {
        case 0: if (y == 6) snprintf(out, n, "IN (C)"); else snprintf(out, n, "IN %s,(C)", Z80_R[y]); break;
        case 1: if (y == 6) snprintf(out, n, "OUT (C),0"); else snprintf(out, n, "OUT (C),%s", Z80_R[y]); break;
        case 2: snprintf(out, n, "%s HL,%s", q ? "ADC" : "SBC", Z80_RP[py]); break;
        case 3: { uint16_t nn = rw(); if (q) snprintf(out, n, "LD %s,($%04X)", Z80_RP[py], nn); else snprintf(out, n, "LD ($%04X),%s", nn, Z80_RP[py]); break; }
        case 4: snprintf(out, n, "NEG"); break;
        case 5: snprintf(out, n, "%s", y == 1 ? "RETI" : "RETN"); break;
        case 6: snprintf(out, n, "IM %s", Z80_IM[y]); break;
        default:                                     // z == 7
          switch (y) {
            case 0: snprintf(out, n, "LD I,A"); break; case 1: snprintf(out, n, "LD R,A"); break;
            case 2: snprintf(out, n, "LD A,I"); break; case 3: snprintf(out, n, "LD A,R"); break;
            case 4: snprintf(out, n, "RRD");    break; case 5: snprintf(out, n, "RLD");    break;
            default: snprintf(out, n, "NOP");   break;
          }
          break;
      }
    } else if (x == 2 && z <= 3 && y >= 4) {         // block transfer / search / I/O
      static const char *bli[4][4] = {
        {"LDI","CPI","INI","OUTI"}, {"LDD","CPD","IND","OUTD"},
        {"LDIR","CPIR","INIR","OTIR"}, {"LDDR","CPDR","INDR","OTDR"},
      };
      snprintf(out, n, "%s", bli[y - 4][z]);
    } else {
      snprintf(out, n, "DB $ED,$%02X", e);
    }
    return (int)(p - pc);
  }

  // ---- unprefixed main table (also reached for the body of a DD/FD-prefixed instruction)
  int x = op >> 6, y = (op >> 3) & 7, z = op & 7, py = y >> 1, q = y & 1;
  switch (x) {
    case 0:
      switch (z) {
        case 0:
          switch (y) {
            case 0: snprintf(out, n, "NOP"); break;
            case 1: snprintf(out, n, "EX AF,AF'"); break;
            case 2: { int8_t d = (int8_t)rb(); snprintf(out, n, "DJNZ $%04X", (uint16_t)(p + d)); break; }
            case 3: { int8_t d = (int8_t)rb(); snprintf(out, n, "JR $%04X", (uint16_t)(p + d)); break; }
            default: { int8_t d = (int8_t)rb(); snprintf(out, n, "JR %s,$%04X", Z80_CC[y - 4], (uint16_t)(p + d)); break; }
          }
          break;
        case 1:
          if (q == 0) { uint16_t nn = rw(); snprintf(out, n, "LD %s,$%04X", rpN(py), nn); }
          else snprintf(out, n, "ADD %s,%s", rpN(2), rpN(py));
          break;
        case 2:
          switch (y) {
            case 0: snprintf(out, n, "LD (BC),A"); break;  case 1: snprintf(out, n, "LD A,(BC)"); break;
            case 2: snprintf(out, n, "LD (DE),A"); break;  case 3: snprintf(out, n, "LD A,(DE)"); break;
            case 4: { uint16_t nn = rw(); snprintf(out, n, "LD ($%04X),%s", nn, rpN(2)); break; }
            case 5: { uint16_t nn = rw(); snprintf(out, n, "LD %s,($%04X)", rpN(2), nn); break; }
            case 6: { uint16_t nn = rw(); snprintf(out, n, "LD ($%04X),A", nn); break; }
            default: { uint16_t nn = rw(); snprintf(out, n, "LD A,($%04X)", nn); break; }
          }
          break;
        case 3: snprintf(out, n, "%s %s", q == 0 ? "INC" : "DEC", rpN(py)); break;
        case 4: { char a[24]; reg8(y, a, false); snprintf(out, n, "INC %s", a); break; }
        case 5: { char a[24]; reg8(y, a, false); snprintf(out, n, "DEC %s", a); break; }
        case 6: { char a[24]; reg8(y, a, false); uint8_t v = rb(); snprintf(out, n, "LD %s,$%02X", a, v); break; }
        default: { static const char *acc[8] = {"RLCA","RRCA","RLA","RRA","DAA","CPL","SCF","CCF"}; snprintf(out, n, "%s", acc[y]); break; }
      }
      break;
    case 1:
      if (y == 6 && z == 6) snprintf(out, n, "HALT");
      else { bool mem = (y == 6 || z == 6); char d[24], s[24]; reg8(y, d, mem); reg8(z, s, mem); snprintf(out, n, "LD %s,%s", d, s); }
      break;
    case 2: { char a[24]; reg8(z, a, false); snprintf(out, n, "%s%s", Z80_ALU[y], a); break; }
    default:   // x == 3
      switch (z) {
        case 0: snprintf(out, n, "RET %s", Z80_CC[y]); break;
        case 1:
          if (q == 0) snprintf(out, n, "POP %s", rp2N(py));
          else switch (py) {
            case 0: snprintf(out, n, "RET"); break;            case 1: snprintf(out, n, "EXX"); break;
            case 2: snprintf(out, n, "JP (%s)", rpN(2)); break; default: snprintf(out, n, "LD SP,%s", rpN(2)); break;
          }
          break;
        case 2: { uint16_t nn = rw(); snprintf(out, n, "JP %s,$%04X", Z80_CC[y], nn); break; }
        case 3:
          switch (y) {
            case 0: { uint16_t nn = rw(); snprintf(out, n, "JP $%04X", nn); break; }
            case 2: { uint8_t v = rb(); snprintf(out, n, "OUT ($%02X),A", v); break; }
            case 3: { uint8_t v = rb(); snprintf(out, n, "IN A,($%02X)", v); break; }
            case 4: snprintf(out, n, "EX (SP),%s", rpN(2)); break;  case 5: snprintf(out, n, "EX DE,HL"); break;
            case 6: snprintf(out, n, "DI"); break;                  case 7: snprintf(out, n, "EI"); break;
            default: snprintf(out, n, "DB $%02X", op); break;       // y == 1 is the CB prefix, handled above
          }
          break;
        case 4: { uint16_t nn = rw(); snprintf(out, n, "CALL %s,$%04X", Z80_CC[y], nn); break; }
        case 5:
          if (q == 0) snprintf(out, n, "PUSH %s", rp2N(py));
          else if (py == 0) { uint16_t nn = rw(); snprintf(out, n, "CALL $%04X", nn); }
          else snprintf(out, n, "DB $%02X", op);                    // py 1/2/3 are the DD/ED/FD prefixes
          break;
        case 6: { uint8_t v = rb(); snprintf(out, n, "%s$%02X", Z80_ALU[y], v); break; }
        default: snprintf(out, n, "RST $%02X", y * 8); break;       // z == 7
      }
      break;
  }
  return (int)(p - pc);
}

bool dbgDisasmSupported() {
  if (!dbgMemReadable()) return false;        // needs a working peek (lights up with each platform)
  switch (currentPlatform) {
    case PLATFORM_APPLE2: case PLATFORM_C64: case PLATFORM_NES: case PLATFORM_ATARI:
    case PLATFORM_MSX: return true;
    default: return false;
  }
}
int dbgDisasm(uint32_t addr, char *out, int outsz) {
  if (currentPlatform == PLATFORM_MSX) return disasmZ80((uint16_t)addr, out, outsz);
  if (dbgDisasmSupported()) return disasm6502((uint16_t)addr, out, outsz);
  if (out && outsz) out[0] = 0;
  return 1;
}

// ================================ named memory regions ===========================================
// Coarse "what lives where" map per platform, for the heat-map region overlay + hover tooltip.
int dbgMemRegions(const DbgRegion **out) {
  static const DbgRegion c64[] = {
    {0x0000, 0x0001, "CPU port"},        {0x0002, 0x00FF, "Zero page"},
    {0x0100, 0x01FF, "Stack"},           {0x0200, 0x03FF, "System/buffers"},
    {0x0400, 0x07FF, "Screen RAM"},      {0x0800, 0x9FFF, "BASIC program RAM"},
    {0xA000, 0xBFFF, "BASIC ROM/RAM"},   {0xC000, 0xCFFF, "RAM"},
    {0xD000, 0xDFFF, "I/O VIC/SID/CIA"}, {0xE000, 0xFFFF, "KERNAL ROM/RAM"},
  };
  static const DbgRegion a2[] = {
    {0x0000, 0x00FF, "Zero page"},       {0x0100, 0x01FF, "Stack"},
    {0x0200, 0x02FF, "Input buffer"},    {0x0300, 0x03FF, "Vectors/monitor"},
    {0x0400, 0x07FF, "Text/lo-res 1"},   {0x0800, 0x1FFF, "RAM (program)"},
    {0x2000, 0x3FFF, "Hi-res page 1"},   {0x4000, 0x5FFF, "Hi-res page 2"},
    {0x6000, 0xBFFF, "RAM"},             {0xC000, 0xC0FF, "I/O soft switches"},
    {0xC100, 0xCFFF, "Slot ROM"},        {0xD000, 0xFFFF, "ROM / lang card"},
  };
  static const DbgRegion nesr[] = {
    {0x0000, 0x00FF, "Zero page"},       {0x0100, 0x01FF, "Stack"},
    {0x0200, 0x07FF, "RAM"},             {0x0800, 0x1FFF, "RAM (mirror)"},
    {0x2000, 0x2007, "PPU registers"},   {0x2008, 0x3FFF, "PPU regs (mirror)"},
    {0x4000, 0x4017, "APU / I-O"},       {0x4018, 0x401F, "Test mode"},
    {0x4020, 0x5FFF, "Expansion"},       {0x6000, 0x7FFF, "PRG-RAM (SRAM)"},
    {0x8000, 0xFFFF, "PRG ROM"},
  };
  // MSX1: the four 16K pages are mapped to primary slots at run time (BIOS / cart / disk / RAM); this
  // coarse map names the page windows + the BIOS work-area at the top of RAM.
  static const DbgRegion msxr[] = {
    {0x0000, 0x3FFF, "Page 0 (BIOS)"},   {0x4000, 0x7FFF, "Page 1 (cart/disk)"},
    {0x8000, 0xBFFF, "Page 2 (cart)"},   {0xC000, 0xF37F, "Page 3 (RAM)"},
    {0xF380, 0xFFFF, "System work area"},
  };
  switch (currentPlatform) {
    case PLATFORM_C64:    *out = c64; return (int)(sizeof(c64) / sizeof(c64[0]));
    case PLATFORM_APPLE2: *out = a2;  return (int)(sizeof(a2)  / sizeof(a2[0]));
    case PLATFORM_NES:    *out = nesr; return (int)(sizeof(nesr) / sizeof(nesr[0]));
    case PLATFORM_MSX:    *out = msxr; return (int)(sizeof(msxr) / sizeof(msxr[0]));
    default:              *out = nullptr; return 0;
  }
}

// ================================ VIC-II inspector / control (C64) ================================
bool dbgVicSupported() { return currentPlatform == PLATFORM_C64; }
int dbgVicReadRegs(uint8_t *out, int max) {
  if (currentPlatform != PLATFORM_C64) return 0;
  int n = (max < 0x2F) ? max : 0x2F;          // 47 registers $D000-$D02E
  for (int i = 0; i < n; i++) out[i] = c64::vicreg[i];
  return n;
}
void dbgVicWriteReg(int reg, uint8_t val) {
  if (currentPlatform != PLATFORM_C64 || reg < 0 || reg >= 0x40) return;
  c64::write8((unsigned short)(0xD000 + reg), val);   // via the bus so $D011/$D016/$D018 effects apply
}
uint16_t dbgVicBankBase() { return currentPlatform == PLATFORM_C64 ? c64::vicmem : 0; }

// Tag the regions the VIC reads via DMA this frame into the heat map's VIC channel (rendered orange),
// so the memory map shows where screen/char/bitmap/sprite/colour data live. Cheap; a no-op unless heat
// recording is on and we're on the C64. Called once per frame from the VIC (c64_vic.cpp).
static inline void vicHeatMark(uint16_t addr, int len) {
  uint32_t *V = g_dbgHeat[DBG_HEAT_V];
  for (int i = 0; i < len; i++) V[(uint16_t)(addr + i)]++;
}
void dbgVicMarkFrame() {
  if (!g_dbgHeatOn || currentPlatform != PLATFORM_C64 || !g_dbgHeat[DBG_HEAT_V] || !c64::ram) return;
  uint16_t bank   = c64::vicmem;
  uint16_t screen = c64::screenmemstart;             // CPU address of the 1000-byte video matrix
  vicHeatMark(screen, 1000);                          // screen RAM (char codes, or per-cell colour in bitmap)
  vicHeatMark((uint16_t)(screen + 0x3F8), 8);         // sprite pointers
  vicHeatMark(0xD800, 1000);                          // colour RAM (low nibble read per cell)
  uint8_t d011 = c64::vicreg[0x11], d018 = c64::vicreg[0x18];
  if (d011 & 0x20) {                                  // bitmap mode
    vicHeatMark((uint16_t)(bank + ((d018 & 0x08) ? 0x2000 : 0x0000)), 8000);
  } else {                                            // text mode: char generator
    uint16_t charBase = (uint16_t)(bank + ((uint16_t)(d018 & 0x0E) << 10));
    bool charRom = (charBase == 0x1000 || charBase == 0x1800 ||   // char ROM windows aren't CPU RAM
                    charBase == 0x9000 || charBase == 0x9800);
    if (!charRom) vicHeatMark(charBase, 2048);        // 256 chars x 8 rows
  }
  uint8_t en = c64::vicreg[0x15];                     // sprite data for enabled sprites
  for (int s = 0; s < 8; s++) if (en & (1 << s)) {
    uint8_t ptr = c64::ram[(uint16_t)(screen + 0x3F8 + s)];
    vicHeatMark((uint16_t)(bank + ptr * 64), 63);
  }
}

// ================================ SID inspector / control (C64) ===================================
bool dbgSidSupported() { return currentPlatform == PLATFORM_C64; }
int dbgSidReadRegs(uint8_t *out, int max) {
  if (currentPlatform != PLATFORM_C64) return 0;
  return sidDebugRegs(out, max);
}
void dbgSidWriteReg(int reg, uint8_t val) {
  if (currentPlatform != PLATFORM_C64 || reg < 0 || reg >= 0x19) return;
  sidWrite((uint8_t)reg, val);                        // through the synth -> audible immediately
}
void dbgSidVoice(int v, float *env, uint8_t *state) {
  if (currentPlatform != PLATFORM_C64) { if (env) *env = 0; if (state) *state = 0; return; }
  sidDebugVoice(v, env, state);
}

#endif // BOARD_DESKTOP
