// dirent.h - PicoCalc shim for POSIX directory enumeration.
//
// Arduino-ESP32 mounts the SD card into a global VFS, so the file browsers scan it with raw
// opendir()/readdir() (src/apple2/disk.cpp, hd.cpp, atari_cart.cpp, c64_disk.cpp, msx.cpp,
// nes_cart.cpp, sms.cpp, ...). That choice was deliberate and worth preserving: readdir() hands
// back the name AND the file/dir flag in one step, whereas Arduino's openNextFile() fopen()s
// every entry and re-walks the path from the FS root each time -- hundreds of milliseconds per
// entry on a slow SPI card.
//
// arduino-pico has no VFS and no POSIX readdir(), but fs::Dir (SDFS.openDir()) has exactly the
// right shape: next() advances, fileName()/isDirectory() report without opening anything. So the
// shim is a thin adapter and all nine call sites stay byte-identical across every board.
//
// This shadows the newlib <dirent.h> (which is a "not supported" stub on arm-none-eabi) because
// the build task puts this directory first on the include path.
#pragma once

#include <FS.h>
#include <SDFS.h>
#include <string.h>

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

struct dirent {
  unsigned char d_type;
  char          d_name[256];   // SDFS returns long filenames; 255 + NUL
};

typedef struct DIR {
  fs::Dir       dir;
  struct dirent ent;
} DIR;

// path may be "" (SD_VFS_ROOT is empty on this board) or any SD-relative directory; both mean
// the same thing to SDFS once normalised to a leading slash.
static inline DIR *opendir(const char *path) {
  const char *p = (path && path[0]) ? path : "/";
  DIR *d = new DIR();
  d->dir = SDFS.openDir(p);
  return d;   // an empty/nonexistent dir simply yields no entries from next()
}

static inline struct dirent *readdir(DIR *d) {
  if (!d || !d->dir.next()) return nullptr;
  String name = d->dir.fileName();
  strncpy(d->ent.d_name, name.c_str(), sizeof(d->ent.d_name) - 1);
  d->ent.d_name[sizeof(d->ent.d_name) - 1] = '\0';
  d->ent.d_type = d->dir.isDirectory() ? DT_DIR : DT_REG;
  return &d->ent;
}

static inline int closedir(DIR *d) {
  if (!d) return -1;
  delete d;
  return 0;
}

static inline void rewinddir(DIR *d) { if (d) d->dir.rewind(); }
