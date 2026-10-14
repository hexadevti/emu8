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

struct FileBrowser {
  const char *tag;                            // log prefix, e.g. "NES"
  std::vector<std::string> *out;              // the entry list the options UI renders
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
bool fbListTruncated(const std::vector<std::string> &list);

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
