#include "../../emu.h"
#if defined(BOARD_DESKTOP)
#include "../desktop/debug_bridge.h"   // dbgDiskRead: desktop disk-read heat map (no-op on device)
#endif
#include "c64.h"
#include "../shared/filebrowser.h"   // shared SD image browser (subdirectories + sorting)
// SD_VFS_ROOT (the SD mount path) is defined in emu.h.

// C64 program / disk loading.
//
//  * .prg  - raw memory image prefixed with a 2-byte little-endian load address.
//  * .d64  - 1541 disk image. We act as a "virtual drive": the KERNAL LOAD routine
//            is trapped (see c64_cpu.cpp, PC==$F49E) and serviced here by reading the
//            directory (track 18) and following the file's sector chain straight out
//            of the image on the SD card. LOAD"name",8 / LOAD"*",8,1 / LOAD"$",8 work.
//
// Loading from the menu happens while the CPU is paused (settings window open), so
// writing c64::ram directly is safe. The KERNAL trap runs from cpuLoop on the CPU core.

// ---------------------------------------------------------------------------
// Helpers shared by the .prg and .d64 paths
// ---------------------------------------------------------------------------
static bool endsWithCI(const std::string &name, const char *ext) {
  size_t n = strlen(ext);
  if (name.size() < n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower(name[name.size() - n + i]) != tolower(ext[i])) return false;
  return true;
}
static bool pathEndsCI(const char *path, const char *ext) {
  size_t n = strlen(ext), m = strlen(path);
  if (m < n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)path[m - n + i]) != tolower((unsigned char)ext[i])) return false;
  return true;
}

// SD access from the 6510's core (the LOAD and READY traps) must hold gBusLock like the settings
// UI's scans on the render task do: two cores inside SdFat at once corrupt its shared sector cache
// and the FAT walk never returns. And a running C64 has little heap, so a File open (SdFat
// allocates its handle) is guarded against bad_alloc: with exceptions enabled nothing else catches
// it and the board just stops. On success the lock stays held until c64CloseLocked().
static File c64OpenLocked(const char *path) {
  busTake();
  try { File f = FSTYPE.open(path, FILE_READ); if (f) return f; }
  catch (const std::bad_alloc &) { printLog("C64: out of heap opening a file"); }
  busGive();
  return File();
}
static void c64CloseLocked(File &f) { f.close(); busGive(); }

// After a non-KERNAL (direct) BASIC load, point VARTAB/ARYTAB/STREND past the program.
static void c64FixBasicVars(uint16_t end) {
  c64::ram[0x2d] = end & 0xff; c64::ram[0x2e] = end >> 8;   // VARTAB
  c64::ram[0x2f] = end & 0xff; c64::ram[0x30] = end >> 8;   // ARYTAB
  c64::ram[0x31] = end & 0xff; c64::ram[0x32] = end >> 8;   // STREND
}

// Queue "RUN<CR>" in the KERNAL keyboard buffer ($0277.., count $00C6).
static void c64QueueRun() {
  const uint8_t run[] = { 'R', 'U', 'N', 0x0d };
  for (int i = 0; i < 4; i++) c64::ram[0x0277 + i] = run[i];
  c64::ram[0x00c6] = 4;
}

// ---------------------------------------------------------------------------
// SD file browser (.prg/.d64/.crt + subdirectories)
// ---------------------------------------------------------------------------
// The browser shows the current directory: a ".." up-entry, subdirectories (stored with a
// trailing "/"), then matching files - all as full paths. The options UI navigates into a
// directory entry / ".." instead of selecting it.

// Cap the entry count so a huge directory can't exhaust the fragmented C64 heap (each entry
// is a heap-backed std::string). Beyond this, the listing is truncated - organise into
// subfolders to see the rest.
#define C64_MAX_FILES 250

// Every file is listed, not just the loadable ones: a folder showing one entry when it holds a
// dozen looks broken. What cannot be loaded is kept in the list and greyed out by the options UI,
// with the reason from c64FileProblem(). Everything else -- the ".." entry, subdirectories, the
// readdir fast path, the entry cap and the sort -- lives in the shared browser.
static bool c64Loadable(const std::string &n) {
  return endsWithCI(n, ".prg") || endsWithCI(n, ".d64") || endsWithCI(n, ".crt");
}
static FileBrowser c64Browser = { "C64", &c64Files, nullptr, nullptr, C64_MAX_FILES, "/" };

// C64F_* code per c64Files entry, rebuilt after every scan (same order, same length).
static std::vector<uint16_t> c64FileCodes;

// Opening a file is a FAT path walk, so only .crt headers are read, and only this many per folder;
// a .crt past the cap is checked when it is loaded instead.
#define C64_MAX_CRT_PROBES 64

static void c64ProbeFiles()
{
  try { c64FileCodes.assign(c64Files.size(), C64F_OK); }
  catch (const std::bad_alloc &) { std::vector<uint16_t>().swap(c64FileCodes); return; }
  int probes = 0;
  for (size_t i = 0; i < c64Files.size(); i++) {
    const std::string &e = c64Files[i];
    if (e == ".." || e.back() == '/') continue;
    if (!c64Loadable(e)) { c64FileCodes[i] = C64F_EXT; continue; }
    if (!endsWithCI(e, ".crt") || probes >= C64_MAX_CRT_PROBES) continue;
    probes++;
    // Timed lock, as in fbScanBody: on the PicoCalc this runs inside the render task.
    if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) break;
    try {
      File f = FSTYPE.open(e.c_str(), FILE_READ);
      c64FileCodes[i] = f ? c64CrtProbe(f) : C64F_OPEN;
      if (f) f.close();
    } catch (const std::bad_alloc &) { c64FileCodes[i] = C64F_NOMEM; }
    if (gBusLock) xSemaphoreGive(gBusLock);
    if ((probes & 7) == 0) uiDirScanProgress((int)c64Files.size());
  }
}

const char *c64FileProblem(size_t idx)
{
  if (idx >= c64FileCodes.size() || c64FileCodes[idx] == C64F_OK) return nullptr;
  return c64FileProblemText(c64FileCodes[idx]);
}

void c64SetFileProblem(size_t idx, uint16_t code)
{
  if (idx < c64FileCodes.size()) c64FileCodes[idx] = code;
}

static const char *crtTypeName(unsigned t)
{
  switch (t) {
    case 1:  return "Action Replay";   case 2:  return "KCS Power";
    case 3:  return "Final Cart III";  case 4:  return "Simons' BASIC";
    case 6:  return "Expert";          case 7:  return "Fun Play";
    case 8:  return "Super Games";     case 9:  return "Atomic Power";
    case 10: return "Epyx FastLoad";   case 11: return "Westermann";
    case 12: return "Rex";             case 13: return "Final Cart I";
    case 14: return "Magic Formel";    case 16: return "Warp Speed";
    case 17: return "Dinamic";         case 18: return "Zaxxon";
    case 20: return "Super Snapshot";  case 21: return "Comal-80";
    case 36: return "Retro Replay";    case 60: return "GMod2";
    default: return nullptr;
  }
}

const char *c64FileProblemText(uint16_t code)
{
  static char s[48];
  switch (code & 0xff) {
    case C64F_OK:        return "";
    case C64F_EXT:       return "Not a C64 image (.prg/.d64/.crt)";
    case C64F_OPEN:      return "Cannot open the file";
    case C64F_CRT_HDR:   return "Not a C64 cartridge (bad header)";
    case C64F_CRT_TYPE: {
      const unsigned t = code >> 8;
      const char *nm = crtTypeName(t);
      if (nm) snprintf(s, sizeof(s), "Cart type %u (%s) unsupported", t, nm);
      else    snprintf(s, sizeof(s), "Cart type %u unsupported", t);
      return s;
    }
    case C64F_CRT_EMPTY: return "Cartridge has no ROM data";
    case C64F_CRT_BIG:   return "Cartridge too big for flash";
    case C64F_NOMEM:     return "Out of memory";
    case C64F_FLASH:     return "Copy to flash failed";
    default:             return "Cannot load";
  }
}

void loadC64FilesSync()      { fbScan(c64Browser); c64ProbeFiles(); }
void c64BrowseEnter(const char *path) { fbEnter(c64Browser, path); c64ProbeFiles(); }
void c64BrowseUp()           { fbUp(c64Browser); c64ProbeFiles(); }

// ---------------------------------------------------------------------------
// .prg loader
// ---------------------------------------------------------------------------
bool c64LoadPRG(const char *path)
{
  if (!c64::ram) return false;
  File f = c64OpenLocked(path);
  if (!f) { snprintf(buf, sizeof(buf), "C64: cannot open %.100s", path); printLog(buf); return false; }

  uint8_t la[2];
  if (f.read(la, 2) != 2) { c64CloseLocked(f); printLog("C64: PRG too short"); return false; }

  uint16_t addr = (uint16_t)(la[0] | (la[1] << 8));
  // One block read straight into RAM (was a byte-at-a-time loop), clipped at the top of memory.
  int got = f.read(c64::ram + addr, 0x10000 - addr);
  c64CloseLocked(f);
  if (got < 0) got = 0;
  uint32_t a = addr + (uint32_t)got;
  uint16_t end = (uint16_t)a;

  if (addr == 0x0801) { c64FixBasicVars(end); c64QueueRun(); }   // BASIC program
  snprintf(buf, sizeof(buf), "C64: loaded %.100s @ $%04X..$%04X%s", path, addr, end,
          addr == 0x0801 ? " (RUN)" : "");
  printLog(buf);
  return true;
}

// ---------------------------------------------------------------------------
// .d64 virtual drive
// ---------------------------------------------------------------------------
// Mounted .d64 ("" = none). A fixed buffer, not a String: the copy used to be a heap allocation
// on the CPU core, and when it failed the String came back empty -- the image silently counted
// as not mounted ("no startable PRG" / DEVICE NOT PRESENT) depending on path length.
static char d64Path[256] = "";

// Sectors per track (1..40). 36-40 exist only on 40-track images (196608 / 197376 bytes).
static const uint8_t D64_SPT[41] = {
  0,
  21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,   // 1-17
  19,19,19,19,19,19,19,                                 // 18-24
  18,18,18,18,18,18,                                    // 25-30
  17,17,17,17,17,                                       // 31-35
  17,17,17,17,17                                        // 36-40
};
#define D64_MAX_TRACK 40

static long d64Offset(int track, int sector) {
  if (track < 1 || track > D64_MAX_TRACK) return -1;
  if (sector < 0 || sector >= D64_SPT[track]) return -1;
  long blocks = 0;
  for (int t = 1; t < track; t++) blocks += D64_SPT[t];
  return (blocks + sector) * 256L;
}

static bool d64ReadSector(File &f, int track, int sector, uint8_t *buf256) {
  long off = d64Offset(track, sector);
  if (off < 0 || (uint32_t)off + 256 > (uint32_t)f.size()) return false;   // past a 35-track image
  if (!f.seek(off)) return false;
  bool ok = f.read(buf256, 256) == 256;
#if defined(BOARD_DESKTOP)
  if (ok) dbgDiskRead(track - 1, sector);   // disk-read heat map: C64 track 1-35 -> ring 0-34
#endif
  return ok;
}

bool c64DiskMounted() { return d64Path[0] != 0; }
void c64MountD64(const char *path) {
  strncpy(d64Path, path, sizeof(d64Path) - 1);
  d64Path[sizeof(d64Path) - 1] = 0;
}

// Does the requested name match a 16-byte, $A0-padded directory filename? 1541 rules: a drive
// prefix ("0:", "@0:") is dropped, '?' matches any one character, '*' ends the pattern and matches
// the rest, and only the first 16 characters count.
static bool d64NameMatch(const uint8_t *name, uint8_t len, const uint8_t *entry16) {
  for (uint8_t i = 0; i < len; i++)
    if (name[i] == ':') { name += i + 1; len -= i + 1; break; }
  if (len == 0) return true;                  // "0:" alone: the first file, as with "*"
  if (len > 16) len = 16;
  for (uint8_t i = 0; i < len; i++) {
    if (name[i] == '*') return true;
    if (name[i] == '?') { if (entry16[i] == 0xa0) return false; continue; }
    if (name[i] != entry16[i]) return false;
  }
  return len == 16 || entry16[len] == 0xa0;   // exact: the rest of the field must be padding
}

// Walk the directory (track 18) for the first matching closed PRG; failing that, the first
// matching closed SEQ/USR (LOAD reads any file type on a real 1541). On success fills *ft/*fs
// with the first data block's track/sector.
static bool d64FindFile(File &f, const uint8_t *name, uint8_t len, int *ft, int *fs) {
  uint8_t sec[256];
  int anyT = 0, anyS = 0;
  int t = 18, s = 1, guard = 0;
  while (t != 0 && guard++ < 40) {
    if (!d64ReadSector(f, t, s, sec)) break;
    for (int e = 0; e < 8; e++) {
      int base = e * 32;
      uint8_t type = sec[base + 2];
      if (!(type & 0x80)) continue;                  // scratched or unclosed ("splat") file
      uint8_t kind = type & 0x07;
      if (kind < 1 || kind > 3) continue;            // SEQ, PRG, USR carry a sector chain
      if (!d64NameMatch(name, len, &sec[base + 5])) continue;
      if (kind == 2) { *ft = sec[base + 3]; *fs = sec[base + 4]; return true; }
      if (!anyT) { anyT = sec[base + 3]; anyS = sec[base + 4]; }
    }
    t = sec[0]; s = sec[1];                          // next directory sector
  }
  if (!anyT) return false;
  *ft = anyT; *fs = anyS;
  return true;
}

// Follow a file's sector chain from (ft,fs) into RAM. The first two data bytes are the
// program's load address; useFileAddr picks it, otherwise altAddr is used (relocated load).
// Returns 0 on success (0xFF*... KERNAL error otherwise: 4 = file/read error).
static int d64LoadChain(File &f, int ft, int fs, bool useFileAddr, uint16_t altAddr,
                        uint16_t *startAddr, uint16_t *endAddr) {
  uint8_t blk[256];
  int t = ft, s = fs, guard = 0;
  uint32_t a = 0;
  bool first = true;
  // 768 blocks is more than a 40-track disk holds: a longer chain is a loop in a corrupt image.
  // A link to a track/sector that is not on the image is an error, not a quiet end of file.
  for (;;) {
    if (guard++ >= 768 || !d64ReadSector(f, t, s, blk)) return 4;
    int nextT = blk[0], nextS = blk[1];
    bool last = (nextT == 0);
    int hi = last ? nextS : 255;                     // last used byte index in this block
    int i = 2;
    if (first) {
      if (hi < 3) return 4;
      uint16_t loadAddr = blk[2] | (blk[3] << 8);
      a = useFileAddr ? loadAddr : altAddr;
      *startAddr = (uint16_t)a;
      i = 4;                                          // skip the 2-byte load address
    }
    for (; i <= hi; i++) if (a <= 0xffff) c64::ram[a++] = blk[i];
    first = false;
    if (last) break;
    t = nextT; s = nextS;
  }
  *endAddr = (uint16_t)a;
  return 0;
}

// KERNAL-trap entry: load a named file (or "*") from the mounted .d64.
// Returns 0 on success, 4 = file not found / error (KERNAL's "FILE NOT FOUND").
int c64D64LoadByName(const uint8_t *name, uint8_t len, bool useFileAddr,
                     uint16_t altAddr, uint16_t *startAddr, uint16_t *endAddr)
{
  if (!c64::ram || !c64DiskMounted()) return 4;
  File f = c64OpenLocked(d64Path);
  if (!f) return 4;
  int ft = 0, fs = 0;
  if (!d64FindFile(f, name, len, &ft, &fs)) { c64CloseLocked(f); printLog("d64: file not found"); return 4; }
  int err = d64LoadChain(f, ft, fs, useFileAddr, altAddr, startAddr, endAddr);
  c64CloseLocked(f);
  if (!err) snprintf(buf, sizeof(buf), "d64: loaded @ $%04X..$%04X", *startAddr, *endAddr);
  else      snprintf(buf, sizeof(buf), "d64: read error in the file's sector chain");
  printLog(buf);
  return err;
}

// Emit one BASIC directory "line" into RAM and advance.  *a points at the link field.
static void dirEmitLine(uint32_t *a, uint16_t lineNo, const uint8_t *text, int tlen) {
  uint32_t here = *a;
  uint32_t next = here + 2 + 2 + tlen + 1;            // link + line# + text + 0
  if (next + 2 > 0x10000) return;                     // would run off the top of the 64K RAM
  c64::ram[here]     = next & 0xff;
  c64::ram[here + 1] = (next >> 8) & 0xff;
  c64::ram[here + 2] = lineNo & 0xff;
  c64::ram[here + 3] = (lineNo >> 8) & 0xff;
  for (int i = 0; i < tlen; i++) c64::ram[here + 4 + i] = text[i];
  c64::ram[here + 4 + tlen] = 0x00;
  *a = next;
}

// LOAD"$",8 - build a BASIC program from the directory so LIST shows it.
bool c64D64LoadDirectory(uint16_t altAddr, uint16_t *endAddr)
{
  if (!c64::ram || !c64DiskMounted()) return false;
  File f = c64OpenLocked(d64Path);
  if (!f) return false;

  uint8_t bam[256];
  if (!d64ReadSector(f, 18, 0, bam)) { c64CloseLocked(f); return false; }

  uint32_t a = altAddr;
  uint8_t line[40];
  int n;

  // header: reverse-video  "DISK NAME       " ID 2A   (disk name at $90, id at $A2)
  n = 0;
  line[n++] = 0x12; line[n++] = '"';
  for (int i = 0; i < 16; i++) { uint8_t c = bam[0x90 + i]; line[n++] = (c == 0xa0) ? ' ' : c; }
  line[n++] = '"'; line[n++] = ' ';
  line[n++] = bam[0xa2]; line[n++] = bam[0xa3];
  line[n++] = ' '; line[n++] = '2'; line[n++] = 'A';
  dirEmitLine(&a, 0, line, n);

  // file entries
  static const char *types[] = { "DEL", "SEQ", "PRG", "USR", "REL" };
  uint8_t sec[256];
  int t = 18, s = 1, guard = 0;
  while (t != 0 && guard++ < 40) {
    if (!d64ReadSector(f, t, s, sec)) break;
    for (int e = 0; e < 8; e++) {
      int base = e * 32;
      uint8_t type = sec[base + 2];
      if ((type & 0x0f) == 0 && type == 0) continue;   // empty slot
      int tn = type & 0x07; if (tn > 4) tn = 0;
      uint16_t blocks = sec[base + 30] | (sec[base + 31] << 8);
      n = 0;
      line[n++] = ' '; line[n++] = '"';
      int nl = 0;
      for (int i = 0; i < 16; i++) { uint8_t c = sec[base + 5 + i]; if (c == 0xa0) break; line[n++] = c; nl++; }
      line[n++] = '"';
      for (int i = nl; i < 16; i++) line[n++] = ' ';
      line[n++] = ' ';
      for (const char *p = types[tn]; *p; p++) line[n++] = *p;
      dirEmitLine(&a, blocks, line, n);
    }
    t = sec[0]; s = sec[1];
  }

  // footer: blocks free (sum of per-track free counts in the BAM, skipping track 18)
  int free = 0;
  for (int tk = 1; tk <= 35; tk++) if (tk != 18) free += bam[4 + (tk - 1) * 4];
  n = 0;
  const char *bf = "BLOCKS FREE.";
  for (const char *p = bf; *p; p++) line[n++] = *p;
  dirEmitLine(&a, (uint16_t)free, line, n);

  if (a + 2 <= 0x10000) { c64::ram[a] = 0x00; c64::ram[a + 1] = 0x00; a += 2; }   // end of program
  *endAddr = (uint16_t)a;
  c64CloseLocked(f);
  return true;
}

// ---------------------------------------------------------------------------
// Menu dispatch: load the highlighted image (.crt mounts & resets into the cartridge;
// .d64 mounts and auto-loads/runs the first program "*"; .prg loads & runs).
//
// This is the IMMEDIATE loader: it writes straight into c64::ram and queues RUN in the KERNAL
// keyboard buffer, both of which only make sense at a clean BASIC prompt. Call it from the
// BASIC-READY trap (cpuLoop) or at boot -- everything else goes through c64LoadAndRun() below.
// ---------------------------------------------------------------------------
bool c64LoadSelected(const char *path)
{
  if (pathEndsCI(path, ".crt"))
    return c64LoadCRT(path);                      // mounts + requests a reset into the cart

  c64CartUnmount();                               // a .prg/.d64 needs normal BASIC/RAM mapping
  if (pathEndsCI(path, ".d64")) {
    c64MountD64(path);
    const uint8_t star[1] = { '*' };
    uint16_t start = 0, end = 0;
    if (c64D64LoadByName(star, 1, true, 0, &start, &end) != 0) {
      printLog("d64: mounted, no startable PRG");
      return false;
    }
    if (start == 0x0801) { c64FixBasicVars(end); c64QueueRun(); }   // BASIC boot -> RUN
    snprintf(buf, sizeof(buf), "d64: mounted %.100s, started @ $%04X", path, start);
    printLog(buf);
    return true;
  }
  return c64LoadPRG(path);
}

// Swap in a different image while the machine is already running something. The immediate loader
// above assumes a machine sitting at READY: a game that is already running owns the memory map
// ($01), the IRQ vectors and the VIC bank, and it never reads the keyboard buffer c64QueueRun()
// writes "RUN" into. Dropping a second image on top of that is what made every load after the
// first one misbehave -- and made loading it twice (the first attempt crashing back to BASIC,
// the second landing on a clean prompt) the only way through.
//
// So reset the machine and hand the load to the same BASIC-READY trap the boot autoload uses:
// the KERNAL re-inits itself, reaches $A480, and cpuLoop then calls c64LoadSelected() on a clean
// machine. A .crt is unchanged -- it mounts and autostarts through its own reset.
bool c64LoadAndRun(const char *path)
{
  if (pathEndsCI(path, ".crt")) return c64LoadCRT(path);

  // Unmount before the reset, not after: with a cart still mapped the KERNAL would autostart it
  // instead of booting to BASIC, and the deferred load would never see its READY prompt.
  c64CartUnmount();
  selectedC64FileName = path;          // what the trap (and AUTOLOAD) will load
  c64AutoloadPending  = true;          // cpuLoop loads it once the KERNAL reaches $A480 (READY)
  c64::c64ResetReq    = true;
  snprintf(buf, sizeof(buf), "C64: reset, then load %.100s at READY", path);
  printLog(buf);
  return true;
}
