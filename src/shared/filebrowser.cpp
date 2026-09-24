#include "../../emu.h"
#include "filebrowser.h"
#include <dirent.h>   // raw POSIX readdir(): name + dir flag in one pass, no per-entry stat()
#include <algorithm>
#include <new>          // std::bad_alloc -- see fbScan()
#include <strings.h>

// ---------------------------------------------------------------------------
// Entry helpers
// ---------------------------------------------------------------------------
static bool fbEntryIsDir(const std::string &e) {
  return e == ".." || (!e.empty() && e.back() == '/');
}

// Add one entry, given its bare name and dir flag. `prefix` is the current directory's path
// prefix ("/" at the root, "/games/" inside one). Shared by the readdir path and the fallback.
static void fbAdd(FileBrowser &b, const std::string &prefix, const char *name, bool isDir)
{
  if (!name || !*name) return;
  if (name[0] == '.') return;             // "." / ".." and the hidden junk FAT cards accumulate
  if (!strcasecmp(name, "System Volume Information")) return;
  if (isDir) { b.out->push_back(prefix + name + "/"); return; }
  if (b.accept && !b.accept(std::string(name))) return;
  b.out->push_back(prefix + name);
}

// ".." first, then directories, then files; case-insensitive within each group. FAT hands
// entries back in creation order, which is unreadable once subdirectories are mixed in.
static bool fbLess(const std::string &a, const std::string &b)
{
  if (a == "..") return b != "..";
  if (b == "..") return false;
  bool da = fbEntryIsDir(a), db = fbEntryIsDir(b);
  if (da != db) return da;
  return strcasecmp(a.c_str(), b.c_str()) < 0;
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------
// True between taking gBusLock and giving it back. fbScan()'s catch needs it: a std::bad_alloc
// thrown mid-enumeration would otherwise leave the SD bus locked for good.
static bool fbHoldsBus = false;

// The real work; fbScan() below wraps it in the out-of-heap guard. Nothing else calls it.
static void fbScanBody(FileBrowser &b)
{
  std::vector<std::string> &out = *b.out;
  out.clear();
  // Reserve a working amount, NOT maxEntries. maxEntries is a *cap* ("a huge folder cannot
  // exhaust the heap"), and reserving it up front did the exact opposite: 250 std::strings is
  // ~8KB claimed in one contiguous block for a folder that usually holds a handful of images.
  // Let the vector grow; the cap is still enforced in the loop below.
  out.reserve(32);
  if (b.dir != "/") out.push_back("..");                      // go-up entry

  std::string prefix = (b.dir == "/") ? std::string("/")
                                      : (std::string(b.dir.c_str()) + "/");
  bool truncated = false;
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
    while ((de = readdir(dp)) != nullptr) {
      fbAdd(b, prefix, de->d_name, de->d_type == DT_DIR);
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
        fbAdd(b, prefix, base ? base + 1 : nmc, file.isDirectory());
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
      if (!fbEntryIsDir(out[i]) && !b.verify(out[i].c_str())) out.erase(out.begin() + i);
      else i++;
    }
  }

  std::sort(out.begin(), out.end(), fbLess);

  // %.80s: buf is 255 bytes shared globally and a nested path can be long.
  sprintf(buf, "%s: %.80s -> %d entr(ies) in %lums (scanned %d)%s free heap=%u", b.tag,
          b.dir.c_str(), (int)out.size(), millis() - t0, scanned,
          truncated ? " (TRUNCATED)" : "", (unsigned)ESP.getFreeHeap());
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
  try {
    fbScanBody(b);
  } catch (const std::bad_alloc &) {
    if (fbHoldsBus) { fbHoldsBus = false; busGive(); }   // died mid-walk: release the card
    b.out->clear();
    sprintf(buf, "%s: OUT OF HEAP during scan -- list left empty (free heap=%u)", b.tag,
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
