// filebrowser.h - one directory browser, shared by every core's SD image picker.
//
// Each core used to carry its own near-identical loadXxxFilesSync(): opendir(SD_VFS_ROOT),
// readdir, match an extension, push "/" + name. They all skipped directories outright, so the
// picker could only ever see the card's root. The C64 browser was the exception -- it already
// walked subdirectories -- and this file is that implementation generalised so every core gets
// the same behaviour from one place.
//
// Entry format (unchanged from the C64 browser, which the options UI already understands):
//   ".."          the go-up entry, present in every directory except the root
//   "/games/"     a subdirectory -- full path, TRAILING SLASH is the directory marker
//   "/games/a.do" a file -- full path, no trailing slash
// optionsui.cpp keys off exactly that: ouiIsDir() tests for ".." or a trailing "/", and
// ouiDisplayName() strips the path and wraps directories in [brackets].
#pragma once

#include <Arduino.h>
#include <vector>
#include <string>
#include <algorithm>

// The entry list. Reads like a std::vector<std::string> of the entries above (size(), [i],
// range-for), but [i] builds the full path on the fly: the list itself is ONE malloc'd pool of
// bare names plus 16-bit offsets into it, and the directory prefix is stored once.
//
// Why not just a vector<std::string>: a full path ("/apple2/Karateka.dsk") is past the 15-char
// small-string buffer, so every entry cost a 24-byte string PLUS its own ~32-byte heap block --
// ~56 bytes, and the vector's doubling needed old+new blocks live at once. The PicoCalc's IIe,
// C64 and MSX open the browser with ~10KB free, so a folder of a few hundred images ran out after
// ~100. Here an entry costs its name + 3 bytes.
//
// [i] returns BY VALUE: never keep .c_str() of it past the statement -- copy it into a
// std::string first (const std::string &e = list[i] is fine, the temporary's life is extended).
class FileList {
public:
  FileList() {}
  ~FileList() { release(); }
  FileList(const FileList &) = delete;
  FileList &operator=(const FileList &) = delete;

  size_t size() const  { return n; }
  bool   empty() const { return n == 0; }
  std::string operator[](size_t i) const;        // ".." or prefix + name (dirs end in "/")
  const char *name(size_t i) const { return pool + idx[i]; }   // bare stored name
  void release();                                // empty the list AND give its heap back

  struct const_iterator {
    const FileList *l; size_t i;
    std::string operator*() const { return (*l)[i]; }
    const_iterator &operator++() { ++i; return *this; }
    bool operator!=(const const_iterator &o) const { return i != o.i; }
  };
  const_iterator begin() const { return { this, 0 }; }
  const_iterator end() const   { return { this, n }; }

  // Building / editing (filebrowser.cpp, snapshot.cpp).
  void setPrefix(const std::string &p) { prefix = p; }
  bool reserve(size_t entries, size_t poolBytes);   // best effort; false = could not
  bool add(const char *name, bool isDir);           // false = out of heap / pool full
  bool fits(const char *name, bool isDir) const {    // add() would not need to allocate
    return n < cap && poolLen + strlen(name) + (isDir ? 2 : 1) <= poolCap;
  }
  void erase(size_t i);
  void shrink();                                    // hand the unused tail back
  template <class Cmp> void sortIdx(Cmp cmp);       // cmp(const char *bareA, const char *bareB)

private:
  bool growIdx(size_t want);
  bool growPool(size_t want);
  char     *pool = nullptr;
  uint16_t *idx  = nullptr;
  uint32_t  poolLen = 0, poolCap = 0;
  uint16_t  n = 0, cap = 0;
  std::string prefix;
};

struct FileBrowser {
  const char *tag;                            // log prefix, e.g. "NES"
  FileList *out;                              // the entry list the options UI renders
  bool (*accept)(const std::string &name);    // filter on the BARE name; nullptr accepts every file
  bool (*verify)(const char *fullPath);       // optional content check, or nullptr -- see below
  int   maxEntries;                           // cap (FB_MAX_ENTRIES); the free heap may stop it sooner
  String dir;                                 // current directory: "/" or "/a/b" (never a trailing /);
                                              // "" = not scanned yet, starts in appsBaseDir()
  bool  truncated;                            // last scan stopped early (cap or heap): more on the card
};

// Every core's entry cap. Kept well above any real folder: a cap that bites hides files from all
// over the alphabet (it applies in FAT order, before the sort). Low heap stops a scan earlier --
// see FB_HEAP_RESERVE in filebrowser.cpp.
#define FB_MAX_ENTRIES 1000

// True when the list was cut short by its last scan (the options UI shows the count as "N+").
bool fbListTruncated(const FileList &list);

// Rescan b.dir into *b.out. Directories are listed first, then files, each sorted
// case-insensitively, with ".." pinned to the top.
//
// accept() runs while the SD bus lock is held, so it must not touch the card -- it only sees the
// name. verify() runs AFTER the lock is released and is handed the full path, so it may open the
// file (PC-XT uses it to check for a 0x55AA boot signature); an entry it rejects is dropped.
void fbScan(FileBrowser &b);

// Navigate into a directory entry (full path, trailing "/" optional) and rescan.
void fbEnter(FileBrowser &b, const char *path);

// Navigate to the parent directory and rescan. No-op at the root.
void fbUp(FileBrowser &b);

template <class Cmp> void FileList::sortIdx(Cmp cmp)
{
  const char *p = pool;
  std::sort(idx, idx + n, [p, &cmp](uint16_t a, uint16_t b) { return cmp(p + a, p + b); });
}
