// snapshot.cpp - save states on the SD card (Ctrl-Shift-F5; see snapshot.h for the core side).
//
// Files: <snapBaseDir()>/<system>/NNNN-<name>.snp, NNNN counting up from 0001 per system and <name>
// the program / cartridge / disk that was running, so the list sorts newest first by name alone and
// still says what each one is. The page that lists them is a mode of the settings window
// (optionsui.cpp); this file does the files.
//
// Layout: an 80-byte header, the settings, then the core's own stream (its xxxSnapshot(SnapIO&)),
// then "END!".
//   0  "EMU8SNAP"   magic
//   8  u16          SNAP_VERSION
//  10  u8           platform (Platform enum)
//  11  u8           model (Apple II: 1 = IIe; 0 elsewhere)
//  12  u32          bytes after the header (the core's stream + the end tag)
//  16  char[64]     what was running (path, informational)
//  80  "CONF" u16 n  the settings record, EEPROM bytes 0..n-1 (version 2 on; n = SNAP_CONF_LEN)
//1878  "THMB" u16 w u16 h + w*h RGB565  the picture at half size (version 3 on; w = h = 0: none)
// The length is checked against the file size before anything is read into the machine, so a
// file cut short by a pulled card never gets half-way in.
//
// The settings say which disk / HD / cartridge was mounted, the Apple model, the options: RAM taken
// with one disk does not run with another in the drive. So a load whose settings differ from the
// running ones writes them back, reboots so the system comes up with that media, and the load then
// finishes by itself (snapBootPoll). The ROM picks and base folders past SNAP_CONF_LEN stay.

#include "../../emu.h"
#include "snapshot.h"
#include "filebrowser.h"
#include <algorithm>

#define SNAP_VERSION   3              // 2: + the settings record, 3: + the thumbnail
#define SNAP_HDR_LEN   80
#define SNAP_MEDIA_LEN 64
#define SNAP_CONF_LEN  1792           // RomSelEEPROMaddress: every board's EEPROM up to the ROM picks
#define SNAP_CONF_AT   (SNAP_HDR_LEN + 6)   // file offset of the settings bytes (after "CONF" u16)
#define SNAP_THUMB_AT  (SNAP_CONF_AT + SNAP_CONF_LEN)   // "THMB" u16 w u16 h, then the pixels
static const char kSnapMagic[8] = { 'E', 'M', 'U', '8', 'S', 'N', 'A', 'P' };

// Subfolder for the running system, nullptr when there is nothing to snapshot.
const char *snapSystemName()
{
  switch (currentPlatform) {
    case PLATFORM_APPLE2: return AppleIIe ? "apple2e" : "apple2plus";   // not interchangeable
    case PLATFORM_C64:    return "c64";
    case PLATFORM_NES:    return "nes";
    case PLATFORM_ATARI:  return "atari2600";
#if BOARD_HAS_MSX_CORE
    case PLATFORM_MSX:    return "msx";
#endif
#if BOARD_HAS_SMS_CORE
    case PLATFORM_SMS:    return "sms";
#endif
#if BOARD_HAS_COLECO_CORE
    case PLATFORM_COLECO: return "coleco";
#endif
#if BOARD_HAS_ZX_CORE
    case PLATFORM_ZX:     return "zx48";
#endif
#if BOARD_HAS_PCXT_CORE
    case PLATFORM_PCXT:   return "pcxt";
#endif
    default:              return nullptr;
  }
}

String snapDir()
{
  const char *sys = snapSystemName();
  String d = snapBaseDir();
  if (d != "/") d += "/";
  d += sys ? sys : "none";
  return d;
}

static bool snapCore(SnapIO &io)
{
  switch (currentPlatform) {
    case PLATFORM_APPLE2: return apple2Snapshot(io);
    case PLATFORM_C64:    return c64Snapshot(io);
    case PLATFORM_NES:    return nesSnapshot(io);
    case PLATFORM_ATARI:  return atariSnapshot(io);
#if BOARD_HAS_MSX_CORE
    case PLATFORM_MSX:    return msxSnapshot(io);
#endif
#if BOARD_HAS_SMS_CORE
    case PLATFORM_SMS:    return smsSnapshot(io);
#endif
#if BOARD_HAS_COLECO_CORE
    case PLATFORM_COLECO: return colecoSnapshot(io);
#endif
#if BOARD_HAS_ZX_CORE
    case PLATFORM_ZX:     return zxSnapshot(io);
#endif
#if BOARD_HAS_PCXT_CORE
    case PLATFORM_PCXT:   return pcxtSnapshot(io);
#endif
    default: io.fail("No snapshots for this system"); return false;
  }
}

static uint8_t snapModel() { return currentPlatform == PLATFORM_APPLE2 && AppleIIe ? 1 : 0; }

// Timed, not busTake(): this runs in the render task (the keyboard pump), and a card that never
// lets go must not freeze the panel and the keyboard with it (see fbScan).
static bool snapLock()   { return !gBusLock || xSemaphoreTake(gBusLock, pdMS_TO_TICKS(5000)) == pdTRUE; }
static void snapUnlock() { if (gBusLock) xSemaphoreGive(gBusLock); }

static bool snapAccept(const std::string &name)
{
  size_t n = name.size();
  return n > 4 && strcasecmp(name.c_str() + n - 4, ".snp") == 0;
}

// Every snapshot of the running system, newest (highest number) first, as full paths.
void snapList(FileList &out)
{
  out.release();
  if (!snapSystemName()) return;
  FileBrowser b = { "SNAP", &out, snapAccept, nullptr, FB_MAX_ENTRIES, snapDir() };
  fbScan(b);
  for (size_t i = 0; i < out.size(); ) {                     // files only: drop ".." and "name/"
    const char *e = out.name(i);
    size_t l = strlen(e);
    if (!strcmp(e, "..") || (l && e[l - 1] == '/')) out.erase(i); else i++;
  }
  out.sortIdx([](const char *a, const char *b) {               // bare names: the number leads
    int na = atoi(a), nb = atoi(b);
    return na != nb ? na > nb : strcmp(a, b) > 0;
  });
}

// "NNNN-NAME.snp": NAME is the running program's file name without folder, extension and the
// "(1982) (Activision) [!]" tags, punctuation dropped ("Pitfall!" -> "Pitfall"), cut to keep the
// whole path inside the 63 characters the rest of the firmware handles.
static String snapFileName(int num, const char *media)
{
  char name[32] = "";
  if (media && *media && strcmp(media, "/") != 0) {
    const char *b = strrchr(media, '/');
    b = b ? b + 1 : media;
    size_t j = 0;
    for (const char *p = b; *p && j < 24; p++) {
      if (*p == '.' && !strchr(p + 1, '.')) break;                       // stop at the extension
      if ((*p == '(' || *p == '[') && j) break;                          // ...and at the tags
      char c = *p;
      if (c == '_') c = ' ';
      if (!isalnum((unsigned char)c) && c != '-' && c != ' ') continue;
      if (c == ' ' && (!j || name[j - 1] == ' ')) continue;              // no leading / double spaces
      name[j++] = c;
    }
    while (j && (name[j - 1] == ' ' || name[j - 1] == '-')) j--;
    name[j] = 0;
  }
  char f[48];
  if (name[0]) snprintf(f, sizeof(f), "%04d-%s.snp", num, name);
  else         snprintf(f, sizeof(f), "%04d.snp", num);
  return String(f);
}

static void snapHeader(SnapIO &io, char *media, uint8_t &platform, uint8_t &model, uint32_t &len,
                       uint16_t &ver)
{
  char magic[8];
  memcpy(magic, kSnapMagic, 8);
  ver = SNAP_VERSION;
  io.bytes(magic, 8);
  io.val(ver);
  io.val(platform);
  io.val(model);
  io.val(len);
  io.bytes(media, SNAP_MEDIA_LEN);
  // Version 1 had no settings record: with another disk in, its RAM hangs the machine. Refused.
  if (io.ok && io.loading && (memcmp(magic, kSnapMagic, 8) != 0 || ver < 2 || ver > SNAP_VERSION))
    io.fail(memcmp(magic, kSnapMagic, 8) != 0 ? "Not a snapshot file" : "Old snapshot format - save a new one");
}

// Does settings byte `at` pick what the machine runs -- its model, its mounted media? Only those
// make a load need the snapshot's settings + a reboot; the rest (colours, volume, speed...) is left
// as it is now when the media match. `model` is the snapshot's (Apple: II+ 0 / IIe 1).
static bool snapMediaByte(int at, uint8_t model)
{
  auto slot = [at](int base) { return at >= base && at < base + 128; };
  if (at == PlatformEEPROMaddress) return true;
  switch (currentPlatform) {
    case PLATFORM_APPLE2:
      if (at == IIpIIeEEPROMaddress) return true;
      return model ? (at == IIeHdDiskEEPROMaddress || slot(IIeDiskFileNameEEPROMaddress) || slot(IIeHdFileNameEEPROMaddress))
                   : (at == HdDiskEEPROMaddress || slot(DiskFileNameEEPROMaddress) || slot(HdFileNameEEPROMaddress));
    case PLATFORM_C64:    return slot(C64FileNameEEPROMaddress);
    case PLATFORM_NES:    return slot(NesFileNameEEPROMaddress);
    case PLATFORM_ATARI:  return slot(AtariFileNameEEPROMaddress);
    case PLATFORM_MSX:    return at == MsxDiskRomEEPROMaddress || slot(MsxFileNameEEPROMaddress);
    case PLATFORM_SMS:    return slot(SmsFileNameEEPROMaddress);
    case PLATFORM_PCXT:   return slot(PcxtFileNameEEPROMaddress) || slot(PcxtHdFileNameEEPROMaddress);
    case PLATFORM_COLECO: return slot(ColecoFileNameEEPROMaddress);
    case PLATFORM_ZX:     return slot(ZxFileNameEEPROMaddress);
    default:              return false;
  }
}

// The settings record, 64 bytes at a time (no heap for all of it). Saving writes the EEPROM;
// loading only reads it past and says whether its model / media differ from the running ones
// (snapMediaByte) -- applying it is snapConfApply(), once the whole
// file is known to be good.
static bool snapConf(SnapIO &io, uint8_t model = 0)
{
  io.tag("CONF");
  uint16_t n = SNAP_CONF_LEN;
  io.val(n);
  if (io.ok && io.loading && n != SNAP_CONF_LEN) io.fail("Snapshot from another version");
  bool differs = false;
  int nameLen = 0;                                           // the slot's length byte (snapshot's)
  uint8_t b[64];
  for (int at = 0; io.ok && at < SNAP_CONF_LEN; at += sizeof(b)) {
    if (!io.loading) for (size_t i = 0; i < sizeof(b); i++) b[i] = EEPROM.read(at + i);
    io.bytes(b, sizeof(b));
    if (!io.loading) continue;
    for (size_t i = 0; i < sizeof(b) && !differs; i++) {
      const int a = at + i;
      if (!snapMediaByte(a, model)) continue;
      // Name slots (128 on): a length byte, then that many chars; what lies past them is leftovers.
      if (a >= 128 && a % 128 == 0) nameLen = b[i] > 127 ? 0 : b[i];
      else if (a >= 128 && a % 128 > nameLen) continue;
      differs = b[i] != EEPROM.read(a);
    }
  }
  return differs;
}

// ---------- the thumbnail ----------
// snapGrabScreen() reads the machine's picture off the panel when the page opens over it, at half
// size, into <snapBaseDir()>/thumb.tmp (the RP2040 has no 38 KB of RAM to keep it in); snapSave()
// copies it into the file. Opened any other way (from the settings) there is no picture to take.
static bool snapThumbOk = false;

static String snapThumbFile()
{
  String d = snapBaseDir();
  if (d != "/") d += "/";
  return d + "thumb.tmp";
}

void snapGrabScreen()
{
  snapThumbOk = false;
  if (!snapSystemName()) return;
  const uint32_t t0 = millis();
  uint16_t *rows = (uint16_t *)malloc((2 * 320 + SNAP_THUMB_W) * sizeof(uint16_t));
  if (!rows) { printLog("SNAP: no memory for the thumbnail"); return; }
  uint16_t *out = rows + 2 * 320;
  if (!snapLock()) { free(rows); return; }
  const String base = snapBaseDir(), path = snapThumbFile();
  if (base != "/" && !FSTYPE.exists(base.c_str())) FSTYPE.mkdir(base.c_str());
  if (FSTYPE.exists(path.c_str())) FSTYPE.remove(path.c_str());
  File f = FSTYPE.open(path.c_str(), FILE_WRITE);
  bool ok = (bool)f;
  for (int ty = 0; ok && ty < SNAP_THUMB_H; ty++) {
    ok = displayReadVideo(0, ty * 2, 320, 2, rows);
    for (int tx = 0; ok && tx < SNAP_THUMB_W; tx++) {           // 2x2 average per channel
      const uint16_t a = rows[tx * 2], b = rows[tx * 2 + 1], c = rows[320 + tx * 2], d = rows[320 + tx * 2 + 1];
      const uint32_t r = ((a >> 11) + (b >> 11) + (c >> 11) + (d >> 11) + 2) >> 2;
      const uint32_t g = (((a >> 5) & 63) + ((b >> 5) & 63) + ((c >> 5) & 63) + ((d >> 5) & 63) + 2) >> 2;
      const uint32_t bl = ((a & 31) + (b & 31) + (c & 31) + (d & 31) + 2) >> 2;
      out[tx] = (uint16_t)((r << 11) | (g << 5) | bl);
    }
    if (ok) ok = f.write((const uint8_t *)out, SNAP_THUMB_W * 2) == SNAP_THUMB_W * 2;
    if (ty == SNAP_THUMB_H / 2 && ok) {                          // what the panel answered (read-back check)
      sprintf(buf, "SNAP: thumbnail mid-row %04X %04X %04X %04X", out[0], out[40], out[80], out[120]);
      printLog(buf);
    }
  }
  if (f) f.close();
  if (!ok) FSTYPE.remove(path.c_str());
  snapUnlock();
  free(rows);
  snapThumbOk = ok;
  sprintf(buf, "SNAP: thumbnail %s (%lu ms)", ok ? "taken" : "FAILED", (unsigned long)(millis() - t0));
  printLog(buf);
}

void snapThumbForget() { snapThumbOk = false; }

// "THMB" u16 w u16 h + pixels. Saving copies thumb.tmp (or writes w = h = 0); loading steps over it.
static void snapThumb(SnapIO &io)
{
  io.tag("THMB");
  uint16_t w = 0, h = 0;
  File t;
  if (!io.loading && snapThumbOk) {
    t = FSTYPE.open(snapThumbFile().c_str(), FILE_READ);
    if (t && (uint32_t)t.size() == (uint32_t)SNAP_THUMB_W * SNAP_THUMB_H * 2) { w = SNAP_THUMB_W; h = SNAP_THUMB_H; }
  }
  io.val(w);
  io.val(h);
  if (!io.loading) {
    uint8_t row[SNAP_THUMB_W * 2];
    for (int y = 0; io.ok && y < h; y++) {
      if (t.read(row, sizeof(row)) != sizeof(row)) { memset(row, 0, sizeof(row)); }
      io.bytes(row, sizeof(row));
    }
    if (t) t.close();
    return;
  }
  const uint32_t n = (uint32_t)w * h * 2;
  if (io.ok && n) {
    if (!io.f->seek(io.f->position() + n)) io.fail("Snapshot file is short");
    io.count += n;
  }
}

// Draw `path`'s thumbnail at (x, y), SNAP_THUMB_W x SNAP_THUMB_H. False = it has none.
bool snapThumbDraw(const char *path, int x, int y)
{
  if (!path || !*path) return false;
  if (!snapLock()) { printLog("SNAP: thumb draw: SD busy"); return false; }
  File f = FSTYPE.open(path, FILE_READ);
  const char *why = nullptr;
  uint8_t hdr[10];
  uint16_t w = 0, h = 0;
  char tag[4];
  if (!f) why = "open";
  else if (f.read(hdr, 10) != 10 || memcmp(hdr, kSnapMagic, 8) != 0) why = "header";
  else if ((hdr[8] | (hdr[9] << 8)) < 3) why = nullptr, w = 1;  // older file: no picture, not an error
  else if (!f.seek(SNAP_THUMB_AT) || f.read((uint8_t *)tag, 4) != 4 || memcmp(tag, "THMB", 4) != 0) why = "tag";
  else if (f.read((uint8_t *)&w, 2) != 2 || f.read((uint8_t *)&h, 2) != 2) why = "size";
  bool ok = !why && w == SNAP_THUMB_W && h == SNAP_THUMB_H;
  if (ok) {
#if BOARD_DISPLAY_GFX
    int band = SNAP_THUMB_H;        // the UI scales: whole bands keep the rows from leaving gaps
    uint16_t *px = (uint16_t *)malloc((size_t)w * band * 2);
    if (!px) band = 1;
#else
    const int band = 1;             // straight to the panel: one row at a time, off the stack
    uint16_t *px = nullptr;
#endif
    uint16_t row[SNAP_THUMB_W];
    uint16_t *dst = px ? px : row;
    tft.setSwapBytes(true);
    for (int r = 0; ok && r < h; r += band) {
      const int n = r + band > h ? h - r : band;
      ok = f.read((uint8_t *)dst, (size_t)w * n * 2) == (size_t)w * n * 2;
      if (ok) tft.pushImage(x, y + r, w, n, dst);
      else why = "pixels";
    }
    tft.setSwapBytes(false);
    free(px);
  }
  if (f) f.close();
  snapUnlock();
  if (why) {
    sprintf(buf, "SNAP: thumb draw failed (%s) %s", why, path);
    printLog(buf);
  }
  return ok;
}

static void snapConfApply(File &f)
{
  uint8_t b[64];
  f.seek(SNAP_CONF_AT);
  for (int at = 0; at < SNAP_CONF_LEN; at += sizeof(b)) {
    if (f.read(b, sizeof(b)) != sizeof(b)) return;
    for (size_t i = 0; i < sizeof(b); i++) EEPROM.write(at + i, b[i]);
  }
  EEPROM.commit();
}

// The load a settings restore rebooted for: <snapBaseDir()>/pending holds its path.
static String snapPendingFile()
{
  String d = snapBaseDir();
  if (d != "/") d += "/";
  return d + "pending";
}

// Save the running machine as the next number in its folder. `newest` is the list's highest number
// (snapList order), `media` what is running. On success `saved` is the new file's full path.
bool snapSave(int newest, const char *media, String &saved, const char *&err)
{
  err = nullptr;
  if (!snapSystemName()) { err = "No snapshots for this system"; return false; }
  const String dir = snapDir();
  const String path = dir + "/" + snapFileName(newest + 1, media);
  if (path.length() >= ROMSEL_SLOT_LEN) { err = "Folder path too long"; return false; }
  saveConfig();                                      // the record below is what runs right now
  if (!snapLock()) { err = "SD card busy - try again"; return false; }

  const String base = snapBaseDir();
  if (base != "/" && !FSTYPE.exists(base.c_str())) FSTYPE.mkdir(base.c_str());
  if (!FSTYPE.exists(dir.c_str())) FSTYPE.mkdir(dir.c_str());
  File f = FSTYPE.open(path.c_str(), FILE_WRITE);
  if (!f) { snapUnlock(); err = "Cannot create the file"; return false; }

  SnapIO io;
  io.f = &f; io.loading = false;
  char m[SNAP_MEDIA_LEN] = {0};
  if (media) strncpy(m, media, SNAP_MEDIA_LEN - 1);
  uint8_t platform = (uint8_t)currentPlatform, model = snapModel();
  uint32_t len = 0;                                  // patched below
  uint16_t ver;
  snapHeader(io, m, platform, model, len, ver);
  const uint32_t start = io.count;
  snapConf(io);
  snapThumb(io);
  snapCore(io);
  io.tag("END!");
  if (io.ok) {
    len = io.count - start;
    if (!f.seek(12) || f.write((const uint8_t *)&len, 4) != 4) io.fail("SD card write failed");
  }
  f.close();
  if (!io.ok) FSTYPE.remove(path.c_str());          // never leave half a snapshot behind
  snapUnlock();

  if (!io.ok) { err = io.err ? io.err : "Save failed"; return false; }
  saved = path;
  sprintf(buf, "SNAP: saved %s (%lu bytes)", path.c_str(), (unsigned long)(len + SNAP_HDR_LEN));
  printLog(buf);
  return true;
}

// The machine was partly overwritten by a snapshot that then turned out bad: start it over clean.
static void snapResetMachine()
{
  switch (currentPlatform) {
    case PLATFORM_APPLE2: cpuReset(); break;
#if BOARD_HAS_SMS_CORE
    case PLATFORM_SMS:    smsHardReset(); break;
#endif
#if BOARD_HAS_COLECO_CORE
    case PLATFORM_COLECO: colecoHardReset(); break;
#endif
#if BOARD_HAS_ZX_CORE
    case PLATFORM_ZX:     zxHardReset(); break;
#endif
#if BOARD_HAS_PCXT_CORE
    case PLATFORM_PCXT:   pcxtHardReset(); break;
#endif
    default:              saveConfig(); delay(50); ESP.restart(); break;   // no live reset: reboot into it
  }
}

// Load `path` into the running machine. On false nothing was changed, unless the file failed
// part-way through (then the machine has been reset and `err` says so).
//
// When the snapshot's settings differ from the running ones (another disk mounted, another model...)
// and `force` is off, nothing is loaded: its settings are written instead, `restart` turns true and
// the caller reboots (snapRestart); the load then finishes after the boot (snapBootPoll). `force`
// is that second half -- the settings are already the snapshot's.
bool snapLoad(const char *path, const char *&err, bool *restart, bool force)
{
  err = nullptr;
  if (restart) *restart = false;
  if (!force) saveConfig();             // the EEPROM then says what is mounted right now
  if (!snapLock()) { err = "SD card busy - try again"; return false; }
  File f = FSTYPE.open(path, FILE_READ);
  if (!f) { snapUnlock(); err = "Cannot open the file"; return false; }

  SnapIO io;
  io.f = &f; io.loading = true;
  char m[SNAP_MEDIA_LEN];
  uint8_t platform = 0, model = 0;
  uint32_t len = 0;
  uint16_t ver = 0;
  snapHeader(io, m, platform, model, len, ver);
  if (io.ok && platform != (uint8_t)currentPlatform) io.fail("Snapshot is for another system");
  if (io.ok && (uint32_t)f.size() != SNAP_HDR_LEN + len) io.fail("Snapshot file is damaged");
  const bool differs = io.ok && snapConf(io, model);
  if (io.ok && differs && !force) {
    // Its settings first (they may switch the Apple model too), then a reboot into them.
    snapConfApply(f);
    f.close();
    File p = FSTYPE.open(snapPendingFile().c_str(), FILE_WRITE);
    const bool pendingOk = (bool)p;
    if (p) { p.write((const uint8_t *)path, strlen(path)); p.close(); }
    snapUnlock();
    sprintf(buf, "SNAP: %s needs its own settings: restored, restarting%s", path, pendingOk ? "" : " (pending NOT written)");
    printLog(buf);
    if (restart) *restart = true;
    return false;
  }
  if (io.ok && ver >= 3) snapThumb(io);
  if (io.ok && model != snapModel()) io.fail(model ? "Snapshot is for the Apple IIe" : "Snapshot is for the Apple II+");
  bool touched = false;
  if (io.ok) {
    const uint32_t before = io.count;
    snapCore(io);
    touched = !io.rejected && io.count > before;   // state was read in (a reject() comes first)
    io.tag("END!");
  }
  f.close();
  snapUnlock();

  if (!io.ok) {
    err = io.err ? io.err : "Load failed";
    sprintf(buf, "SNAP: load %s failed: %s%s", path, err, touched ? " (machine reset)" : "");
    printLog(buf);
    if (touched) { snapResetMachine(); err = "Snapshot damaged - machine reset"; }
    return false;
  }
  sprintf(buf, "SNAP: loaded %s", path);
  printLog(buf);
  return true;
}

// Reboot into the settings snapLoad() just wrote. Straight to the restart: the usual saveConfig()
// on the way out would write the running machine's settings back over them.
void snapRestart()
{
  printLog("SNAP: restarting into the snapshot's settings");
  delay(50);
  ESP.restart();
}

// After a settings restore + reboot (snapLoad's restart), the load finishes by itself and the
// boot stays hidden: the system menu is skipped (snapBootPending, from the splash), and while the
// core starts and mounts its media the render loop shows "Restoring snapshot" instead of the
// machine (snapBootPoll returns true). Then the core is paused, the snapshot loaded, and resumed.
static uint8_t snapBootState = 0;        // 0 not looked, 1 waiting for the boot, 2 done, 3 showing an error
static uint32_t snapBootT0 = 0;
static char snapBootPath[ROMSEL_SLOT_LEN];

// Once, at boot: is there a load to finish? (Reads and removes the pending file.)
bool snapBootPending()
{
  if (snapBootState == 0) {
    snapBootState = 2;
    if (!snapSystemName() || !snapLock()) return false;
    const String pf = snapPendingFile();
    size_t n = 0;
    if (FSTYPE.exists(pf.c_str())) {
      File p = FSTYPE.open(pf.c_str(), FILE_READ);
      if (p) { n = p.read((uint8_t *)snapBootPath, sizeof(snapBootPath) - 1); p.close(); }
      FSTYPE.remove(pf.c_str());       // once only, whatever happens next: never a boot loop
    }
    snapUnlock();
    if (!n) return false;
    snapBootPath[n] = 0;
    const String dir = snapDir() + "/";
    if (strncmp(snapBootPath, dir.c_str(), dir.length()) != 0) {
      sprintf(buf, "SNAP: pending %s is not for this system, dropped", snapBootPath);
      printLog(buf);
      return false;
    }
    sprintf(buf, "SNAP: %s pending after the restart: boot hidden", snapBootPath);
    printLog(buf);
    snapBootState = 1;
    snapBootT0 = millis();
  }
  return snapBootState == 1 || snapBootState == 3;
}

static void snapBootDraw(const char *title, const char *line, uint16_t colour)
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(colour, TFT_BLACK);
  tft.drawString(title, 160, 104, 2);
  tft.setTextColor(tft.color565(150, 160, 175), TFT_BLACK);
  tft.drawString(line, 160, 132, 1);
}

// Render loop, every frame the running system would be drawn: true = the restore owns the screen.
bool snapBootPoll()
{
  if (!snapBootPending()) return false;
  static bool drawn = false;
  const char *sl = strrchr(snapBootPath, '/');
  const char *name = sl ? sl + 1 : snapBootPath;
  if (snapBootState == 3) {                                  // the error stays up a moment
    if (millis() - snapBootT0 < 4000) return true;
    snapBootState = 2;
    clearScreen();
    return false;
  }
  if (!drawn) { snapBootDraw("Restoring snapshot...", name, TFT_WHITE); drawn = true; }
  // Let the core boot and mount its media (an HD, a cartridge) first.
  if (millis() - snapBootT0 < 2500) return true;
  paused = true;
  delay(100);                                                // the core settles into its pause
  const char *err = nullptr;
  const bool ok = snapLoad(snapBootPath, err, nullptr, true);
  paused = false;
  if (!ok) {
    snapBootDraw("Snapshot not restored", err ? err : "Load failed", tft.color565(220, 40, 40));
    snapBootState = 3;
    snapBootT0 = millis();
    return true;
  }
  snapBootState = 2;
  clearScreen();                                             // the machine repaints its screen
  return false;
}

bool snapBootBusy() { return snapBootState == 1 || snapBootState == 3; }

bool snapDelete(const char *path)
{
  if (!snapLock()) return false;
  bool ok = FSTYPE.remove(path);
  snapUnlock();
  sprintf(buf, "SNAP: delete %s %s", path, ok ? "ok" : "FAILED");
  printLog(buf);
  return ok;
}
