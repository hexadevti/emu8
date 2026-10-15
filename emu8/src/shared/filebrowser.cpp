#include "../../emu.h"
#include "filebrowser.h"
#include <dirent.h>   // raw POSIX readdir(): name + dir flag in one pass, no per-entry stat()
#include <algorithm>
#include <new>          // std::bad_alloc -- see fbScan()
#include <strings.h>

// ---------------------------------------------------------------------------
// FileList
// ---------------------------------------------------------------------------
std::string FileList::operator[](size_t i) const
{
  const char *nm = pool + idx[i];
  if (nm[0] == '.' && nm[1] == '.' && !nm[2]) return std::string("..");
  std::string s;
  s.reserve(prefix.size() + strlen(nm));
  s += prefix;
  s += nm;
  return s;
}

void FileList::release()
{
  free(pool); pool = nullptr; poolLen = poolCap = 0;
  free(idx);  idx = nullptr;  n = cap = 0;
}

bool FileList::growIdx(size_t want)
{
  if (want <= cap) return true;
  if (want > 0xffff) want = 0xffff;
  if (want <= cap) return false;
  void *p = realloc(idx, want * sizeof(uint16_t));
  if (!p) return false;
  idx = (uint16_t *)p; cap = (uint16_t)want;
  return true;
}

// Offsets are 16-bit, so the pool stops at 64KB -- far past FB_MAX_ENTRIES of real names.
bool FileList::growPool(size_t want)
{
  if (want <= poolCap) return true;
  if (want > 0x10000) return false;
  void *p = realloc(pool, want);
  if (!p) return false;
  pool = (char *)p; poolCap = (uint32_t)want;
  return true;
}

bool FileList::reserve(size_t entries, size_t poolBytes)
{
  bool ok = growPool(poolBytes);     // the bigger block first, while the heap is least cut up
  return growIdx(entries) && ok;
}

bool FileList::add(const char *name, bool isDir)
{
  size_t len = strlen(name), need = len + (isDir ? 1 : 0) + 1;
  if (n == cap && !growIdx(cap ? (size_t)cap * 2 : 16) && !growIdx((size_t)cap + 1)) return false;
  if (poolLen + need > poolCap) {
    size_t w = poolCap ? (size_t)poolCap * 2 : 512;
    if (w < poolLen + need) w = poolLen + need;
    if (!growPool(w) && !growPool(poolLen + need)) return false;
  }
  char *d = pool + poolLen;
  memcpy(d, name, len);
  if (isDir) d[len++] = '/';
  d[len] = 0;
  idx[n++] = (uint16_t)poolLen;
  poolLen += need;
  return true;
}

void FileList::erase(size_t i)
{
  if (i >= n) return;
  memmove(idx + i, idx + i + 1, (n - i - 1) * sizeof(uint16_t));
  n--;
}

// realloc() to a smaller size stays in place, so this only hands the tail back -- it never needs
// a second block. The scan sized the list for every entry it counted; verify() may drop some.
void FileList::shrink()
{
  if (pool && poolLen && poolLen < poolCap) { void *p = realloc(pool, poolLen); if (p) { pool = (char *)p; poolCap = poolLen; } }
  if (idx && n && n < cap) { void *p = realloc(idx, n * sizeof(uint16_t)); if (p) { idx = (uint16_t *)p; cap = n; } }
}

// ---------------------------------------------------------------------------
// Entry helpers
// ---------------------------------------------------------------------------
// On a STORED (bare) name: ".." or "name/".
static bool fbNameIsDir(const char *e) {
  size_t l = strlen(e);
  return !strcmp(e, "..") || (l && e[l - 1] == '/');
}

// Whether a directory entry is listed at all. Shared by the count pass and the add pass, so the
// count is exact and the list can be sized once.
static bool fbWanted(FileBrowser &b, const char *name, bool isDir)
{
  if (!name || !*name) return false;
  if (name[0] == '.') return false;       // "." / ".." and the hidden junk FAT cards accumulate
  if (!strcasecmp(name, "System Volume Information")) return false;
  if (isDir) return true;
  return !b.accept || b.accept(std::string(name));
}

// ".." first, then directories, then files; case-insensitive within each group. FAT hands
// entries back in creation order, which is unreadable once subdirectories are mixed in.
// Every entry shares one prefix, so ordering the bare names orders the full paths.
static bool fbLess(const char *a, const char *b)
{
  bool ua = !strcmp(a, ".."), ub = !strcmp(b, "..");
  if (ua || ub) return ua && !ub;
  bool da = fbNameIsDir(a), db = fbNameIsDir(b);
  if (da != db) return da;
  return strcasecmp(a, b) < 0;
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------
// Free heap the scan leaves alone. On the PicoCalc's RP2040 the C64 starts a scan with ~10KB free
// and each listed entry costs ~40-60 bytes, so a folder of a few hundred files used to run the
// heap dry, throw std::bad_alloc and lose the WHOLE list (an empty browser, or just ".."). Now the
// scan stops short of this reserve and shows what it has -- the reserve is what the machine still
// needs afterwards (the C64's per-entry problem codes, SD File objects to mount the pick, the UI).
// ESP32 boards keep more: WiFi/BT and the USB host allocate at run time.
#if defined(BOARD_PICOCALC)
#define FB_HEAP_RESERVE 3072
#else
#define FB_HEAP_RESERVE 16384
#endif

// Free heap is a sum of holes on the RP2040 (rp2040.getFreeHeap()), not the largest block, so this
// is a margin check, not a guarantee -- an add that still fails is caught in fbTryAdd().
static bool fbHeapLow() { return ESP.getFreeHeap() < FB_HEAP_RESERVE; }

// Add one wanted entry; false = stop the scan here (heap low, or the list could not grow).
// The reserve is only checked when the add has to GROW the list: the count pass already sized it
// to leave FB_HEAP_RESERVE free, so the heap sits right at the reserve after that reservation and
// testing it on every add stopped a big folder at its first entry (an empty list).
static bool fbTryAdd(FileBrowser &b, const char *name, bool isDir)
{
  if (!b.out->fits(name, isDir) && fbHeapLow()) return false;
  return b.out->add(name, isDir);
}

// The browsers scanned so far, so fbListTruncated() can answer for a bare list.
static FileBrowser *fbSeen[16];

static void fbRemember(FileBrowser &b)
{
  for (FileBrowser *&p : fbSeen) {
    if (p == &b) return;
    if (!p) { p = &b; return; }
  }
}

bool fbListTruncated(const FileList &list)
{
  for (FileBrowser *p : fbSeen)
    if (p && p->out == &list) return p->truncated;
  return false;
}

// True between taking gBusLock and giving it back. fbScan()'s catch needs it: a std::bad_alloc
// thrown mid-enumeration would otherwise leave the SD bus locked for good.
static bool fbHoldsBus = false;

// The real work; fbScan() below wraps it in the out-of-heap guard. Nothing else calls it.
static void fbScanBody(FileBrowser &b)
{
  FileList &out = *b.out;
  out.release();
  out.setPrefix((b.dir == "/") ? std::string("/") : (std::string(b.dir.c_str()) + "/"));
  bool truncated = false, heapLow = false;
  int scanned = 0;
  unsigned long t0 = millis();

  // Entry trace. While this runs the settings menu is unresponsive (the caller blocks), so if
  // it stalls or dies the machine looks locked up -- these two lines are what says how far it
  // got. Printed before the bus is taken, so a log stopping here rules the SD layer out.
  sprintf(buf, "%s: scan start %.60s", b.tag, b.dir.c_str());
  printLog(buf);
  // Heap figure on its OWN line and AFTER the one above: ESP.getFreeHeap() walks the heap via
  // mallinfo() and takes the malloc lock, so it is itself a suspect whenever a log stops here.
  sprintf(buf, "%s: free heap=%u", b.tag, (unsigned)ESP.getFreeHeap());
  printLog(buf);

  // Hold the bus for the whole enumeration. This can run while the other core is reading the
  // card, and SDFS -- unlike the ESP32's VFS FATFS layer -- has no lock of its own: two cores
  // inside SdFat at once corrupt its shared volume cache and the FAT walk never returns.
  //
  // Timed, NOT busTake()'s portMAX_DELAY. On the PicoCalc this function is reached from the
  // settings menu via the keyboard pump, which runs inside the render task -- so blocking here
  // forever stops the panel AND the keyboard together and the only way out is a power cycle.
  // Five seconds is far beyond any legitimate SD transaction; if it expires something is wrong
  // and a log line the user can send is worth more than a dead machine.
  if (gBusLock && xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) != pdTRUE) {
    sprintf(buf, "%s: BUS LOCK TIMEOUT after 5s -- scan abandoned, list left empty", b.tag);
    printLog(buf);
    return;
  }
  fbHoldsBus = true;

  String vfsPath = SD_VFS_ROOT;
  if (b.dir != "/") vfsPath += b.dir;

  // Fast path: raw POSIX readdir(). Each call returns the entry name AND a file/dir flag
  // (d_type) straight from the FAT directory record, advancing the directory read once (O(n)
  // total). Arduino's File::openNextFile() instead does a full fopen()+stat() per entry, which
  // re-walks the path from the FS root every time -- 0.2-2s each on a 4MHz SPI SD, so listing
  // one folder took *seconds* and tripped the watchdog.
  DIR *dp = opendir(vfsPath.c_str());
  if (dp) {
    struct dirent *de;
    // Count first (no allocation) and size the list once: offsets and names, exactly. Growing it
    // by doubling needs the old AND the new block live at the same moment, which on the RP2040's
    // ~10KB is what ended long lists early. If the whole thing is more than the heap can spare
    // (FB_HEAP_RESERVE stays free) only a part is asked for, and the list stops when that is full.
    size_t count = 1, bytes = 3;                              // + the ".." entry
    while ((de = readdir(dp)) != nullptr && (int)count < b.maxEntries)
      if (fbWanted(b, de->d_name, de->d_type == DT_DIR)) { count++; bytes += strlen(de->d_name) + 2; }
    closedir(dp);
    long avail = (long)ESP.getFreeHeap() - FB_HEAP_RESERVE;
    while (count > 1 && (long)(count * sizeof(uint16_t) + bytes) > avail) { count -= count / 4 + 1; bytes -= bytes / 4 + 1; }
    while (count > 1 && !out.reserve(count, bytes)) { count /= 2; bytes /= 2; }
    dp = opendir(vfsPath.c_str());
  }
  if (b.dir != "/" && !out.add("..", false)) {               // go-up entry
    if (dp) closedir(dp);
    fbHoldsBus = false; busGive();
    b.truncated = true;
    sprintf(buf, "%s: OUT OF HEAP before scan (free heap=%u)", b.tag, (unsigned)ESP.getFreeHeap());
    printLog(buf);
    return;
  }
  if (dp) {
    struct dirent *de;
    while ((de = readdir(dp)) != nullptr) {
      bool isDir = de->d_type == DT_DIR;
      if (fbWanted(b, de->d_name, isDir) && !fbTryAdd(b, de->d_name, isDir)) { truncated = heapLow = true; break; }
      if ((++scanned & 0x3f) == 0) uiDirScanProgress((int)out.size());   // progress bar + yield
      if ((int)out.size() >= b.maxEntries) { truncated = true; break; }
    }
    closedir(dp);
  } else {
    // Fallback: the mountpoint is not the assumed SD_VFS_ROOT. Use the (slow) Arduino API so
    // the browser still works; the core-0 watchdog is disabled (see videoSetup) so it cannot
    // reboot us here.
    File dir = FSTYPE.open(b.dir.c_str(), "r");
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      fbHoldsBus = false; busGive();
      sprintf(buf, "%s: cannot open %.80s", b.tag, b.dir.c_str());
      printLog(buf);
      return;
    }
    File file = dir.openNextFile();
    while (file) {
      const char *nmc = file.name();                          // a full path on some cores
      if (nmc && *nmc) {
        const char *base = strrchr(nmc, '/');
        const char *nm = base ? base + 1 : nmc;
        bool isDir = file.isDirectory();
        if (fbWanted(b, nm, isDir) && !fbTryAdd(b, nm, isDir)) { truncated = heapLow = true; break; }
      }
      file = dir.openNextFile();
      if ((++scanned & 0x0f) == 0) uiDirScanProgress((int)out.size());
      else                         vTaskDelay(1);
      if ((int)out.size() >= b.maxEntries) { truncated = true; break; }
    }
    file.close();
    dir.close();
  }
  fbHoldsBus = false; busGive();

  // Content check, outside the lock: verify() opens the file, and the predicates take the bus
  // lock themselves (gBusLock is a plain mutex, so taking it twice would deadlock).
  if (b.verify) {
    for (size_t i = 0; i < out.size(); ) {
      if (!fbNameIsDir(out.name(i)) && !b.verify(out[i].c_str())) out.erase(i);
      else i++;
    }
  }

  out.sortIdx(fbLess);
  out.shrink();
  b.truncated = truncated;

  // %.80s: buf is 255 bytes shared globally and a nested path can be long.
  sprintf(buf, "%s: %.80s -> %d entr(ies) in %lums (scanned %d)%s free heap=%u", b.tag,
          b.dir.c_str(), (int)out.size(), millis() - t0, scanned,
          !truncated ? "" : heapLow ? " (TRUNCATED: heap low)" : " (TRUNCATED)",
          (unsigned)ESP.getFreeHeap());
  printLog(buf);

#if defined(BOARD_PICOCALC)
  // Words of the CALLING task's stack never touched. From the settings menu that is the render
  // task, which has only RENDER_TASK_STACK (2048) words in total. A margin check, not a
  // diagnosis -- an actual overflow would be reported by fault_picocalc.cpp rather than being
  // silent. If this ever gets close to 0 the scan has to move off the render task.
  sprintf(buf, "%s: caller stack headroom=%u words", b.tag,
          (unsigned)uxTaskGetStackHighWaterMark(NULL));
  printLog(buf);
#endif
}

// ---------------------------------------------------------------------------
// fbScan -- entry point
// ---------------------------------------------------------------------------
// The scan allocates: one std::string per accepted entry plus the vector's own growth, and on
// the PicoCalc that lands on a heap with very little left. A failure there is NOT the tidy
// FreeRTOS one: C++ operator new calls malloc() directly rather than pvPortMalloc(), so
// vApplicationMallocFailedHook() (src/picocalc/fault_picocalc.cpp) never runs and nothing is
// printed. The std::bad_alloc just propagates out of the settings menu into std::terminate and
// aborts the machine in silence -- with the panel and the keyboard dead together, because the
// menu runs inside the render task. That is precisely what "switching DEVICE to HD blocks
// everything and there is no way out of the menu" looked like.
//
// So catch it. An empty list plus a log line the user can send is recoverable; a silent abort
// is not. Exceptions are on in this build (the FQBN carries exceptions=Enabled).
void fbScan(FileBrowser &b)
{
  if (b.dir.length() == 0) b.dir = appsBaseDir();   // first scan: the general settings' apps folder
  fbRemember(b);
  b.truncated = false;
  try {
    fbScanBody(b);
  } catch (const std::bad_alloc &) {
    if (fbHoldsBus) { fbHoldsBus = false; busGive(); }   // died mid-walk: release the card
    b.out->release();                                    // give the partial list's heap back
    // Keep the go-up entry, or a folder too big to list would strand the user inside it.
    if (b.dir != "/") b.out->add("..", false);
    b.truncated = true;
    sprintf(buf, "%s: OUT OF HEAP during scan -- only \"..\" kept (free heap=%u)", b.tag,
            (unsigned)ESP.getFreeHeap());
    printLog(buf);
  }
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------
void fbEnter(FileBrowser &b, const char *path)
{
  if (!path || !*path) return;
  b.dir = path;
  while (b.dir.length() > 1 && b.dir.endsWith("/")) b.dir.remove(b.dir.length() - 1);
  fbScan(b);
}

void fbUp(FileBrowser &b)
{
  if (b.dir == "/") return;
  int sl = b.dir.lastIndexOf('/');
  b.dir = (sl <= 0) ? String("/") : b.dir.substring(0, sl);
  fbScan(b);
}
