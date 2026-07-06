#include "../../emu.h"
#if defined(BOARD_DESKTOP)
#include "../desktop/debug_bridge.h"   // dbgDiskRead: desktop disk-read heat map (no-op on device)
#endif
#include "c64.h"
#include <dirent.h>   // raw POSIX directory enumeration (fast browser scan; see loadC64FilesSync)
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
static String c64BrowseDir = "/";

// Cap the entry count so a huge directory can't exhaust the fragmented C64 heap (each entry
// is a heap-backed std::string). Beyond this, the listing is truncated - organise into
// subfolders to see the rest.
#define C64_MAX_FILES 250

// Add one browser entry (skips "."/".." and self), given its basename + dir flag. `prefix`
// is the current directory's path prefix; dirs are stored with a trailing "/", files are
// filtered to .prg/.d64/.crt. Shared by the fast readdir path and the openNextFile fallback.
static void c64AddEntry(const std::string &prefix, const char *name, bool isDir) {
  if (!name || !*name || !strcmp(name, ".") || !strcmp(name, "..")) return;
  std::string full = prefix + name;
  if (isDir) c64Files.push_back(full + "/");                   // dir marker = trailing slash
  else if (endsWithCI(full, ".prg") || endsWithCI(full, ".d64") || endsWithCI(full, ".crt"))
    c64Files.push_back(full);
}

void loadC64FilesSync()
{
  c64Files.clear();
  c64Files.reserve(C64_MAX_FILES);                            // allocate the array once
  if (c64BrowseDir != "/") c64Files.push_back("..");          // go-up entry

  std::string prefix = (c64BrowseDir == "/") ? "/" : (std::string(c64BrowseDir.c_str()) + "/");
  bool truncated = false;
  int scanned = 0;
  unsigned long t0 = millis();

  // Fast path: enumerate with raw POSIX readdir(). Each call returns the entry name AND a
  // file/dir flag (d_type) directly from the FAT directory record, advancing the directory
  // read once (O(n) total). Arduino's File::openNextFile() instead does a full fopen()+stat()
  // per entry, which re-walks the path from the FS root every time - 0.2-2s each on this 4MHz
  // SPI SD, so listing a folder took *seconds* and tripped the watchdog. readdir avoids all
  // of that. (FatFS skips "."/".." itself; we guard anyway.)
  String vfsPath = SD_VFS_ROOT;
  if (c64BrowseDir != "/") vfsPath += c64BrowseDir;
  DIR *dp = opendir(vfsPath.c_str());
  if (dp) {
    struct dirent *de;
    while ((de = readdir(dp)) != nullptr) {
      c64AddEntry(prefix, de->d_name, de->d_type == DT_DIR);
      if ((++scanned & 0x3f) == 0) ::uiDirScanProgress((int)c64Files.size());  // progress + yield
      if ((int)c64Files.size() >= C64_MAX_FILES) { truncated = true; break; }
    }
    closedir(dp);
  } else {
    // Fallback: the mountpoint isn't the assumed "/sd". Use the (slow) Arduino API so the
    // browser still works; the core-0 watchdog is disabled (see videoSetup) so it can't reboot.
    File dir = FSTYPE.open(c64BrowseDir.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      sprintf(buf, "C64: cannot open %s", c64BrowseDir.c_str());
      printLog(buf);
      return;
    }
    File file = dir.openNextFile();
    while (file) {
      const char *nmc = file.name();                           // can be a full path on some cores
      if (nmc && *nmc) {
        const char *base = strrchr(nmc, '/');
        c64AddEntry(prefix, base ? base + 1 : nmc, file.isDirectory());
      }
      file = dir.openNextFile();
      if ((++scanned & 0x0f) == 0) ::uiDirScanProgress((int)c64Files.size());
      else                          vTaskDelay(1);
      if ((int)c64Files.size() >= C64_MAX_FILES) { truncated = true; break; }
    }
    file.close();
    dir.close();
  }

  sprintf(buf, "C64: %s -> %d entr(ies) in %lums (scanned %d)%s", c64BrowseDir.c_str(),
          (int)c64Files.size(), millis() - t0, scanned, truncated ? " (TRUNCATED)" : "");
  printLog(buf);
}

// Navigate into a directory entry (full path, optional trailing "/") and rescan.
void c64BrowseEnter(const char *path)
{
  c64BrowseDir = path;
  while (c64BrowseDir.length() > 1 && c64BrowseDir.endsWith("/"))
    c64BrowseDir.remove(c64BrowseDir.length() - 1);
  loadC64FilesSync();
}

// Navigate to the parent directory and rescan.
void c64BrowseUp()
{
  if (c64BrowseDir == "/") return;
  int sl = c64BrowseDir.lastIndexOf('/');
  c64BrowseDir = (sl <= 0) ? "/" : c64BrowseDir.substring(0, sl);
  loadC64FilesSync();
}

// ---------------------------------------------------------------------------
// .prg loader
// ---------------------------------------------------------------------------
bool c64LoadPRG(const char *path)
{
  if (!c64::ram) return false;
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { sprintf(buf, "C64: cannot open %s", path); printLog(buf); return false; }

  int lo = f.read(), hi = f.read();
  if (lo < 0 || hi < 0) { f.close(); printLog("C64: PRG too short"); return false; }

  uint16_t addr = (uint16_t)(lo | (hi << 8));
  uint32_t a = addr;
  int b;
  while (a <= 0xffff && (b = f.read()) >= 0) c64::ram[a++] = (uint8_t)b;
  f.close();
  uint16_t end = (uint16_t)a;

  if (addr == 0x0801) { c64FixBasicVars(end); c64QueueRun(); }   // BASIC program
  sprintf(buf, "C64: loaded %s @ $%04X..$%04X%s", path, addr, end,
          addr == 0x0801 ? " (RUN)" : "");
  printLog(buf);
  return true;
}

// ---------------------------------------------------------------------------
// .d64 virtual drive
// ---------------------------------------------------------------------------
static String d64Path = "";   // mounted .d64 ("" = none)

// Sectors per track (1..35). Tracks beyond 35 aren't used by the standard image.
static const uint8_t D64_SPT[36] = {
  0,
  21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,   // 1-17
  19,19,19,19,19,19,19,                                 // 18-24
  18,18,18,18,18,18,                                    // 25-30
  17,17,17,17,17                                        // 31-35
};

static long d64Offset(int track, int sector) {
  if (track < 1 || track > 35) return -1;
  if (sector < 0 || sector >= D64_SPT[track]) return -1;
  long blocks = 0;
  for (int t = 1; t < track; t++) blocks += D64_SPT[t];
  return (blocks + sector) * 256L;
}

static bool d64ReadSector(File &f, int track, int sector, uint8_t *buf256) {
  long off = d64Offset(track, sector);
  if (off < 0) return false;
  if (!f.seek(off)) return false;
  bool ok = f.read(buf256, 256) == 256;
#if defined(BOARD_DESKTOP)
  if (ok) dbgDiskRead(track - 1, sector);   // disk-read heat map: C64 track 1-35 -> ring 0-34
#endif
  return ok;
}

bool c64DiskMounted() { return d64Path.length() > 0; }
void c64MountD64(const char *path) { d64Path = path; }

// Does the requested name (PETSCII, len bytes, '*' = wildcard tail) match a 16-byte,
// $A0-padded directory filename?
static bool d64NameMatch(const uint8_t *name, uint8_t len, const uint8_t *entry16) {
  uint8_t matchLen = len;
  bool wild = false;
  for (uint8_t i = 0; i < len; i++) if (name[i] == '*') { matchLen = i; wild = true; break; }
  for (uint8_t i = 0; i < matchLen; i++)
    if (name[i] != entry16[i]) return false;
  if (!wild)                                  // exact: the rest of the field must be padding
    for (uint8_t i = matchLen; i < 16; i++)
      if (entry16[i] != 0xa0) return false;
  return true;
}

// Walk the directory (track 18) looking for the first matching PRG. On success fills
// *ft/*fs with the first data block's track/sector. Returns true if found.
static bool d64FindFile(File &f, const uint8_t *name, uint8_t len, int *ft, int *fs) {
  uint8_t sec[256];
  int t = 18, s = 1, guard = 0;
  while (t != 0 && guard++ < 40) {
    if (!d64ReadSector(f, t, s, sec)) return false;
    for (int e = 0; e < 8; e++) {
      int base = e * 32;
      uint8_t type = sec[base + 2];
      if ((type & 0x07) != 0x02) continue;          // only closed/normal PRG entries
      if (d64NameMatch(name, len, &sec[base + 5])) {
        *ft = sec[base + 3];
        *fs = sec[base + 4];
        return true;
      }
    }
    t = sec[0]; s = sec[1];                          // next directory sector
  }
  return false;
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
  while (t >= 1 && t <= 35 && guard++ < 4096) {
    if (!d64ReadSector(f, t, s, blk)) return 4;
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
  File f = FSTYPE.open(d64Path.c_str(), FILE_READ);
  if (!f) return 4;
  int ft = 0, fs = 0;
  if (!d64FindFile(f, name, len, &ft, &fs)) { f.close(); printLog("d64: file not found"); return 4; }
  int err = d64LoadChain(f, ft, fs, useFileAddr, altAddr, startAddr, endAddr);
  f.close();
  if (!err) { sprintf(buf, "d64: loaded @ $%04X..$%04X", *startAddr, *endAddr); printLog(buf); }
  return err;
}

// Emit one BASIC directory "line" into RAM and advance.  *a points at the link field.
static void dirEmitLine(uint32_t *a, uint16_t lineNo, const uint8_t *text, int tlen) {
  uint32_t here = *a;
  uint32_t next = here + 2 + 2 + tlen + 1;            // link + line# + text + 0
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
  File f = FSTYPE.open(d64Path.c_str(), FILE_READ);
  if (!f) return false;

  uint8_t bam[256];
  if (!d64ReadSector(f, 18, 0, bam)) { f.close(); return false; }

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

  c64::ram[a] = 0x00; c64::ram[a + 1] = 0x00;          // end of program
  a += 2;
  *endAddr = (uint16_t)a;
  f.close();
  return true;
}

// ---------------------------------------------------------------------------
// Menu dispatch: load the highlighted image (.crt mounts & resets into the cartridge;
// .d64 mounts and auto-loads/runs the first program "*"; .prg loads & runs).
// ---------------------------------------------------------------------------
bool c64LoadSelected(const char *path)
{
  std::string p = path;
  if (endsWithCI(p, ".crt"))
    return c64LoadCRT(path);                      // mounts + requests a reset into the cart

  c64CartUnmount();                               // a .prg/.d64 needs normal BASIC/RAM mapping
  if (endsWithCI(p, ".d64")) {
    c64MountD64(path);
    const uint8_t star[1] = { '*' };
    uint16_t start = 0, end = 0;
    if (c64D64LoadByName(star, 1, true, 0, &start, &end) != 0) {
      printLog("d64: mounted, no startable PRG");
      return false;
    }
    if (start == 0x0801) { c64FixBasicVars(end); c64QueueRun(); }   // BASIC boot -> RUN
    sprintf(buf, "d64: mounted %s, started @ $%04X", path, start);
    printLog(buf);
    return true;
  }
  return c64LoadPRG(path);
}
