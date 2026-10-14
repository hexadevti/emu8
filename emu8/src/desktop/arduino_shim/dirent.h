// dirent.h — desktop shim. MinGW-w64's <dirent.h> here lacks POSIX d_type/DT_DIR (the loaders use
// de->d_type == DT_DIR). Provide a complete opendir/readdir/closedir over <io.h>'s _findfirst on
// Windows (no <windows.h>, so no min/max macro pollution). On Linux, defer to the real header.
#pragma once

#if defined(_WIN32)

#include <io.h>
#include <cstdint>
#include <cstring>
#include <string>

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4

struct dirent {
  unsigned char d_type;
  char          d_name[260];
};

typedef struct DIR {
  intptr_t           handle;
  struct _finddata_t fd;
  bool               first;
  bool               done;
  struct dirent      ent;
} DIR;

static inline DIR *opendir(const char *path) {
  DIR *d = new DIR();
  std::string pattern = std::string(path) + "\\*";
  d->handle = _findfirst(pattern.c_str(), &d->fd);
  if (d->handle == -1) { delete d; return nullptr; }
  d->first = true;
  d->done = false;
  return d;
}

static inline struct dirent *readdir(DIR *d) {
  if (!d || d->done) return nullptr;
  if (!d->first) {
    if (_findnext(d->handle, &d->fd) != 0) { d->done = true; return nullptr; }
  }
  d->first = false;
  strncpy(d->ent.d_name, d->fd.name, sizeof(d->ent.d_name) - 1);
  d->ent.d_name[sizeof(d->ent.d_name) - 1] = '\0';
  d->ent.d_type = (d->fd.attrib & _A_SUBDIR) ? DT_DIR : DT_REG;
  return &d->ent;
}

static inline int closedir(DIR *d) {
  if (!d) return -1;
  if (d->handle != -1) _findclose(d->handle);
  delete d;
  return 0;
}

static inline void rewinddir(DIR *) {}

#else
#include_next <dirent.h>
#endif
