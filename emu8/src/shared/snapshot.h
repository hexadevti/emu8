// snapshot.h - save states ("snapshots"), Ctrl-Shift-F5 on every system.
//
// A snapshot is one file on the SD card, <snapBaseDir()>/<system>/NNNN-<name>.snp: a fixed header
// (snapshot.cpp) and then the running core's state, streamed straight to / from the card -- no
// buffer, the RP2040 has no heap for one. Each core supplies ONE function that does both directions
// through SnapIO, so what is saved and what is read back can never drift apart:
//
//   bool c64Snapshot(SnapIO &io)
//   {
//     io.tag("CPU ");                  // section marker (checked on load)
//     io.val(PC); io.val(A); ...       // every field, in the same order both ways
//     io.bytes(RAM, 0x10000);
//     if (io.loading) { ...rebuild what was not saved (pointers, caches)... }
//     return io.ok;
//   }
//
// Called from the render task with the CPU parked (the snapshot page is a mode of the settings
// window, which pauses the machine) and with the SD bus lock HELD for the whole file: a core's
// function must not take gBusLock, open files or touch the card itself. Never save a pointer --
// save an index or offset and rebuild the pointer on load.
//
// Before anything is overwritten on load, a core checks what it can (Apple II model, cartridge
// size/checksum...) -- reading those into LOCALS -- and on a mismatch calls io.reject("...") and
// returns false WITHOUT changing the machine, so a wrong snapshot leaves the running one intact.
// A damaged file found later leaves the machine half-loaded; snapshot.cpp then resets it.
#pragma once
#include "../../emu.h"
#include <vector>
#include <string>

struct SnapIO {
  File *f;
  bool loading;             // false = saving
  bool ok = true;
  const char *err = nullptr;     // reason shown in the footer when ok turns false (static string)
  uint32_t count = 0;            // bytes moved so far

  // Write (saving) or read (loading) n raw bytes at p.
  void bytes(void *p, size_t n)
  {
    if (!ok || !n) return;
    size_t got = loading ? f->read((uint8_t *)p, n) : f->write((const uint8_t *)p, n);
    if (got != n) fail(loading ? "Snapshot file is short" : "SD card write failed");
    count += (uint32_t)got;
  }
  template <class T> void val(T &v) { bytes(&v, sizeof(v)); }
  template <class T, size_t N> void arr(T (&a)[N]) { bytes(a, sizeof(a)); }

  // A bool through a byte (sizeof(bool) is not pinned down; keep the file the same everywhere).
  void flag(bool &b) { uint8_t v = b ? 1 : 0; val(v); if (loading) b = v != 0; }

  // Four-character section marker: written on save, verified on load.
  void tag(const char *t)
  {
    char b[4];
    memcpy(b, t, 4);
    bytes(b, 4);
    if (ok && loading && memcmp(b, t, 4) != 0) fail("Snapshot is damaged");
  }

  void fail(const char *why) { if (ok) { ok = false; err = why; } }

  // Refuse a snapshot BEFORE anything in the machine was overwritten (wrong cartridge, model...):
  // the running machine carries on untouched. fail() after state was read in resets it instead.
  void reject(const char *why) { fail(why); rejected = true; }
  bool rejected = false;
};

// snapshot.cpp (used by the snapshot page in optionsui.cpp)
const char *snapSystemName();             // subfolder of the running system, nullptr = none
String snapDir();                         // <snapBaseDir()>/<system>
#define SNAP_THUMB_W 160                  // the thumbnail: half the 320x240 picture
#define SNAP_THUMB_H 120
void snapGrabScreen();                    // read the running picture off the panel (page opening over it)
void snapThumbForget();                   // the machine ran on: that picture is stale
bool snapThumbDraw(const char *path, int x, int y);   // false = the file has no thumbnail
void snapList(FileList &out);   // full paths, newest first
bool snapSave(int newest, const char *media, String &saved, const char *&err);
bool snapLoad(const char *path, const char *&err, bool *restart = nullptr, bool force = false);
void snapRestart();                       // reboot into the settings a snapLoad() restored
bool snapDelete(const char *path);

// One per core; each sits behind the same #if gate as the core itself (snapshot.cpp calls them
// under the matching BOARD_HAS_*_CORE). True on success; on false io.err says why.
bool apple2Snapshot(SnapIO &io);
bool c64Snapshot(SnapIO &io);
bool nesSnapshot(SnapIO &io);
bool atariSnapshot(SnapIO &io);
bool msxSnapshot(SnapIO &io);
bool smsSnapshot(SnapIO &io);
bool colecoSnapshot(SnapIO &io);
bool zxSnapshot(SnapIO &io);
bool pcxtSnapshot(SnapIO &io);
