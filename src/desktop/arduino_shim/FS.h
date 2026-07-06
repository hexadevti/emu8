// FS.h — desktop shim of the Arduino fs::FS / fs::File API, backed by the host filesystem.
//
// The SD card is emulated by a host directory (g_sdRoot, default ./sdcard, set in sd_host.cpp).
// SD-relative paths ("/game.rom", "/sub/x.dsk") are mapped under that root. File is a lightweight
// handle (shared FILE*/DIR* like Arduino's), so `g_diskFile = SD.open(...)` reassignment works.
//
// Built 32-bit; uses <dirent.h>/<sys/stat.h> (POSIX — present on MinGW-w64 and Linux).
#pragma once

#include <Arduino.h>
#include <cstdio>
#include <string>
#include <memory>
#include <dirent.h>
#include <sys/stat.h>

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

// Root of the emulated SD card (host path). Defined + initialised in sd_host.cpp.
extern std::string g_sdRoot;
// Join an SD-relative path ("/x", "x", or already-absolute host path) onto g_sdRoot.
std::string sdHostPath(const char *sdRelative);

// Force BINARY fopen mode. On Windows "r"/"w" are TEXT mode (CRLF translation + 0x1A treated as EOF),
// which truncates/corrupts binary ROM/disk images. The ESP32/POSIX cores assume binary, so add 'b'.
inline std::string sdBinaryMode(const char *mode) {
  std::string m = mode ? mode : "r";
  if (m.find('b') == std::string::npos) m += 'b';
  return m;
}

namespace fs {

class File {
  std::shared_ptr<FILE> f_;
  std::shared_ptr<DIR>  dir_;
  std::string path_;      // SD-relative path (leading '/')
  std::string name_;      // basename
  bool isDir_ = false;
public:
  File() {}
  // regular file
  File(FILE *fp, const std::string &sdRel) : f_(fp, [](FILE *p){ if (p) fclose(p); }) { setPaths(sdRel); }
  // directory
  File(const std::string &sdRel, bool /*dirTag*/) : isDir_(true) { setPaths(sdRel); }

  explicit operator bool() const { return (bool)f_ || isDir_; }
  bool isDirectory() const { return isDir_; }
  const char *name() const { return name_.c_str(); }
  const char *path() const { return path_.c_str(); }

  int    read() { if (!f_) return -1; int c = fgetc(f_.get()); return c; }
  size_t read(uint8_t *buf, size_t n) { return f_ ? fread(buf, 1, n, f_.get()) : 0; }
  size_t readBytes(char *buf, size_t n) { return f_ ? fread(buf, 1, n, f_.get()) : 0; }
  int    peek() { if (!f_) return -1; int c = fgetc(f_.get()); if (c != EOF) ungetc(c, f_.get()); return c; }

  size_t write(uint8_t b) { return f_ ? fwrite(&b, 1, 1, f_.get()) : 0; }
  size_t write(const uint8_t *buf, size_t n) { return f_ ? fwrite(buf, 1, n, f_.get()) : 0; }

  bool   seek(uint32_t pos, SeekMode mode = SeekSet) {
    if (!f_) return false;
    int origin = (mode == SeekCur) ? SEEK_CUR : (mode == SeekEnd) ? SEEK_END : SEEK_SET;
    return fseek(f_.get(), (long)pos, origin) == 0;
  }
  uint32_t position() { return f_ ? (uint32_t)ftell(f_.get()) : 0; }
  uint32_t size() {
    if (!f_) return 0;
    long cur = ftell(f_.get());
    fseek(f_.get(), 0, SEEK_END);
    long end = ftell(f_.get());
    fseek(f_.get(), cur, SEEK_SET);
    return (uint32_t)end;
  }
  int  available() { if (!f_) return 0; long cur = ftell(f_.get()); uint32_t s = size(); return (int)((long)s - cur); }
  void flush() { if (f_) fflush(f_.get()); }
  void close() { f_.reset(); dir_.reset(); isDir_ = false; }

  // Directory iteration. Lazily opens the DIR* on first call.
  File openNextFile(const char *mode = FILE_READ);
  void rewindDirectory() { dir_.reset(); }

private:
  void setPaths(const std::string &sdRel) {
    path_ = sdRel.empty() ? "/" : sdRel;
    size_t s = path_.find_last_of('/');
    name_ = (s == std::string::npos) ? path_ : path_.substr(s + 1);
    if (name_.empty()) name_ = path_;
  }
};

// Base filesystem: open() maps an SD-relative path under g_sdRoot. SD derives from this.
class FS {
public:
  File open(const char *path, const char *mode = FILE_READ) {
    std::string sdRel = path ? path : "/";
    if (sdRel.empty() || sdRel[0] != '/') sdRel = "/" + sdRel;
    std::string host = sdHostPath(sdRel.c_str());
    struct stat st;
    if (stat(host.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      return File(sdRel, true);                 // directory handle
    FILE *fp = fopen(host.c_str(), sdBinaryMode(mode).c_str());
    if (!fp) return File();
    return File(fp, sdRel);
  }
  File open(const String &path, const char *mode = FILE_READ) { return open(path.c_str(), mode); }
  bool exists(const char *path) { struct stat st; return stat(sdHostPath(path).c_str(), &st) == 0; }
  bool remove(const char *path) { return ::remove(sdHostPath(path).c_str()) == 0; }
  bool mkdir(const char *path);
  bool rmdir(const char *path) { return ::rmdir(sdHostPath(path).c_str()) == 0; }
};

// --- inline out-of-class definitions (header-only; inline = no ODR clash across TUs) ---
inline File File::openNextFile(const char *mode) {
  if (!isDir_) return File();
  if (!dir_) {
    DIR *d = opendir(sdHostPath(path_.c_str()).c_str());
    if (!d) return File();
    dir_.reset(d, [](DIR *p) { if (p) closedir(p); });
  }
  struct dirent *ent;
  while ((ent = readdir(dir_.get())) != nullptr) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    std::string childRel = path_;
    if (childRel.empty() || childRel.back() != '/') childRel += '/';
    childRel += ent->d_name;
    struct stat st;
    if (stat(sdHostPath(childRel.c_str()).c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) return File(childRel, true);
    FILE *fp = fopen(sdHostPath(childRel.c_str()).c_str(), sdBinaryMode(mode).c_str());
    if (!fp) continue;
    return File(fp, childRel);
  }
  return File();   // end of directory
}

inline bool FS::mkdir(const char *path) {
#if defined(_WIN32)
  return ::mkdir(sdHostPath(path).c_str()) == 0;
#else
  return ::mkdir(sdHostPath(path).c_str(), 0777) == 0;
#endif
}

} // namespace fs

using fs::File;
using fs::FS;
