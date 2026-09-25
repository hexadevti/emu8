// sdserial.cpp - SD card file manager over the USB serial port.
//
// A small framed request/response server that lets a host (tools/sdmanager: the web app or the
// Python CLI) browse, upload, download, rename and delete files on the internal microSD card
// WITHOUT pulling the card. It runs as a low-priority FreeRTOS task next to whichever emulator core
// is active, sleeping on the port until a host speaks to it, so a plain serial monitor sees the
// usual boot log and nothing else changes.
//
// Wire format (both directions; little-endian):
//
//   A5 5A | cmd u8 | seq u8 | len u16 | payload[len] | crc16 u16
//
// crc16 is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over cmd..payload. A reply echoes the
// request seq, sets cmd|0x80, and its payload starts with a status byte. Anything between frames
// (boot log lines, a Serial.printf from some core) is not a frame and the host skips it; each
// reply leaves in ONE Serial.write() so the UART/CDC lock keeps other prints out of its middle.
// While a session is live printLog() is muted so the port stays quiet.
//
// Every request is idempotent or keyed by an explicit offset, so the host recovers from a lost
// or corrupted frame by resending the same request. The full command table is in
// tools/sdmanager/PROTOCOL.md.
//
// Uploads land in "<path>.part" and are renamed over <path> only on a committed WRCLOSE, so a cut
// cable never leaves a truncated ROM behind under its real name.
//
// Host test: host/sdserial_host.cpp builds this file against the desktop FS shim with
// SDSERIAL_HOST defined, speaking the protocol over stdin/stdout.

#if defined(SDSERIAL_HOST)
  #include <Arduino.h>
  #include "FS.h"
  #include "SD.h"
  #define FSTYPE SD
  #include <string>
  // Supplied by host/sdserial_host.cpp: a byte pipe standing in for the serial port.
  int      sdsHostAvailable();
  int      sdsHostRead();
  void     sdsHostWrite(const uint8_t *b, size_t n);
  uint32_t sdsHostMillis();
  void     sdsHostIdle();
  static inline void busTake() {}
  static inline void busGive() {}
  volatile bool sdSerialActive = false;
  bool sdSerialMounted = true;
#else
  #include "../../emu.h"
#endif
#include <strings.h>   // strcasecmp

#if defined(BOARD_DESKTOP) && !defined(SDSERIAL_HOST)
// Desktop SDL build: its "Serial" is stdout and the SD card is already a host folder, so there is
// nothing to serve. Keep the symbols so the shared code links unchanged.
volatile bool sdSerialActive = false;
void sdSerialSetup() {}
#else

#if !defined(SDSERIAL_HOST)
volatile bool sdSerialActive = false;
#endif

// ---------------------------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------------------------
// Largest payload either side sends (proto.h: 1K on the CYD/PicoCalc heap, 4K on the PSRAM boards,
// where it cuts the per-chunk round trips). The host asks for it in HELLO.
#if defined(SDSERIAL_HOST)
  #define SDS_MAX_PAYLOAD 4096
#else
  #define SDS_MAX_PAYLOAD SDSERIAL_MAX_PAYLOAD
#endif
#define SDS_SESSION_TIMEOUT_MS 10000   // no valid frame for this long -> session over, logs resume
#define SDS_BAUD_REVERT_MS     4000    // after a baud switch, no frame this soon -> back to default
#define SDS_DEFAULT_BAUD       115200
#define SDS_MAX_PATH           512

enum : uint8_t {
  CMD_HELLO   = 0x01, CMD_FSINFO  = 0x02,
  CMD_LIST    = 0x10, CMD_STAT    = 0x11,
  CMD_RDOPEN  = 0x20, CMD_READ    = 0x21, CMD_RDCLOSE = 0x22,
  CMD_WROPEN  = 0x30, CMD_WRITE   = 0x31, CMD_WRCLOSE = 0x32,
  CMD_MKDIR   = 0x40, CMD_REMOVE  = 0x41, CMD_RENAME  = 0x42,
  CMD_BAUD    = 0x50, CMD_REBOOT  = 0x60, CMD_BYE     = 0x7F,
};
enum : uint8_t {
  ST_OK = 0, ST_FAIL = 1, ST_NOT_FOUND = 2, ST_IO = 3, ST_BAD_ARG = 4,
  ST_NO_SD = 5, ST_NOT_OPEN = 6, ST_UNKNOWN_CMD = 7, ST_UNSUPPORTED = 8,
};
#define SDS_PROTO_VERSION 1
#define HELLO_SD_MOUNTED  0x01
#define HELLO_USB_LINK    0x02   // native USB CDC: the baud rate is meaningless, skip CMD_BAUD
#define HELLO_CAN_BAUD    0x04

// ---------------------------------------------------------------------------------------------
// Port + clock glue
// ---------------------------------------------------------------------------------------------
#if defined(SDSERIAL_HOST)
static inline int      portAvailable() { return sdsHostAvailable(); }
static inline int      portRead()      { return sdsHostRead(); }
static inline void     portWrite(const uint8_t *b, size_t n) { sdsHostWrite(b, n); }
static inline uint32_t nowMs()         { return sdsHostMillis(); }
static inline void     idle(bool)      { sdsHostIdle(); }
#define SDS_USB_LINK 1
#else
static inline int      portAvailable() { return Serial.available(); }
static inline int      portRead()      { return Serial.read(); }
static inline void     portWrite(const uint8_t *b, size_t n) { Serial.write(b, n); }
static inline uint32_t nowMs()         { return millis(); }
// Poll fast while a host is talking (the UART RX buffer is only a few frames deep), lazily when not.
static inline void     idle(bool busy) { vTaskDelay(busy ? 1 : pdMS_TO_TICKS(20)); }
  #if defined(BOARD_PICOCALC) || ARDUINO_USB_CDC_ON_BOOT
    #define SDS_USB_LINK 1
  #else
    #define SDS_USB_LINK 0
  #endif
#endif

// ---------------------------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------------------------
// Frame buffers come from the heap on first use: static DRAM is the scarce resource on the CYD
// (no PSRAM), while the heap has room once the emulator has set up.
static uint8_t *rxBuf = nullptr;   // cmd seq len payload crc (magic not stored)
static uint8_t *txBuf = nullptr;   // magic cmd seq len payload crc
static uint32_t lastFrameMs = 0;
static uint32_t baudSwitchedMs = 0;  // nonzero while running at a non-default rate
#if !SDS_USB_LINK
static uint32_t currentBaud = SDS_DEFAULT_BAUD;
#endif

static File        rdFile;           // open download
static File        wrFile;           // open upload (writing <wrPath>.part)
static std::string wrPath;

static File        listDir;          // directory being paged through by CMD_LIST
static std::string listPath;
static uint16_t    listNext = 0;     // index of the next entry listDir will yield
struct PendingEntry { bool valid; uint8_t flags; uint32_t size; std::string name; };
static PendingEntry listPending = { false, 0, 0, "" };  // read from listDir but not yet sent

static uint16_t crc16(const uint8_t *p, size_t n, uint16_t crc = 0xFFFF) {
  while (n--) {
    crc ^= (uint16_t)(*p++) << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}
static inline void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline uint16_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// Reply builder: payload goes straight into txBuf so a reply is a single write.
static uint8_t *rp = nullptr;   // txBuf + 6: reply payload; rp[0] is the status byte
static void sendReply(uint8_t cmd, uint8_t seq, uint16_t len) {
  txBuf[0] = 0xA5; txBuf[1] = 0x5A;
  txBuf[2] = cmd | 0x80; txBuf[3] = seq;
  put16(txBuf + 4, len);
  put16(txBuf + 6 + len, crc16(txBuf + 2, 4 + len));
  portWrite(txBuf, 6 + len + 2);
}
static void sendStatus(uint8_t cmd, uint8_t seq, uint8_t st) { rp[0] = st; sendReply(cmd, seq, 1); }

#if !defined(SDSERIAL_HOST)
static bool sdMounted() { return sdCardMounted; }   // set by FSSetup() (sd.cpp)
#else
static bool sdMounted() { return sdSerialMounted; }
#endif

// Path argument: payload bytes as a string, forced to start with '/'. Rejects ".." so a host
// cannot step outside the card's root on the desktop shim (on the device there is nothing above).
static bool takePath(const uint8_t *p, size_t n, std::string &out) {
  if (n == 0 || n > SDS_MAX_PATH) return false;
  out.assign((const char *)p, n);
  if (out.find('\0') != std::string::npos) return false;
  if (out[0] != '/') out.insert(out.begin(), '/');
  if (out.find("/../") != std::string::npos || (out.size() >= 3 && out.compare(out.size() - 3, 3, "/..") == 0))
    return false;
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  return true;
}

static const char *baseName(const char *n) {
  const char *s = strrchr(n, '/');
  return s ? s + 1 : n;
}

static void closeList() { if (listDir) listDir.close(); listPath.clear(); listNext = 0; listPending.valid = false; }

// Abort an unfinished upload: the .part file is garbage.
static void abortWrite() {
  if (!wrFile && wrPath.empty()) return;
  busTake();
  if (wrFile) wrFile.close();
  FSTYPE.remove((wrPath + ".part").c_str());
  busGive();
  wrPath.clear();
}

static void revertBaud() {
#if !SDS_USB_LINK
  if (currentBaud != SDS_DEFAULT_BAUD) {
    Serial.flush();
    Serial.updateBaudRate(SDS_DEFAULT_BAUD);
    currentBaud = SDS_DEFAULT_BAUD;
  }
#endif
  baudSwitchedMs = 0;
}

static void endSession() {
  busTake();
  if (rdFile) rdFile.close();
  busGive();
  abortWrite();
  busTake(); closeList(); busGive();
  revertBaud();
  sdSerialActive = false;
}

// ---------------------------------------------------------------------------------------------
// Command handlers. Each gets the request payload and sends exactly one reply.
// ---------------------------------------------------------------------------------------------
static void doHello(uint8_t seq) {
  rp[0] = ST_OK;
  rp[1] = SDS_PROTO_VERSION;
  rp[2] = (sdMounted() ? HELLO_SD_MOUNTED : 0) | (SDS_USB_LINK ? HELLO_USB_LINK : HELLO_CAN_BAUD);
  put16(rp + 3, SDS_MAX_PAYLOAD);
#if defined(BOARD_NAME)
  const char *name = BOARD_NAME;
#elif defined(SDSERIAL_HOST)
  const char *name = "emu8 host test";
#else
  const char *name = "emu8";
#endif
  size_t n = strlen(name);
  memcpy(rp + 5, name, n);
  sendReply(CMD_HELLO, seq, 5 + n);
}

static void doFsInfo(uint8_t seq) {
  uint64_t total = 0, used = 0;   // 0/0 = unknown (the PicoCalc cannot afford the FAT walk)
#if !defined(BOARD_PICOCALC)
  busTake();
  total = FSTYPE.totalBytes();
  used  = FSTYPE.usedBytes();
  busGive();
#endif
  rp[0] = ST_OK;
  put32(rp + 1, (uint32_t)total); put32(rp + 5, (uint32_t)(total >> 32));
  put32(rp + 9, (uint32_t)used);  put32(rp + 13, (uint32_t)(used >> 32));
  sendReply(CMD_FSINFO, seq, 17);
}

// LIST [start u16][path] -> [st][count u8][more u8] { [flags u8][size u32][nameLen u8][name] }*
// The directory stays open between pages; a request for any other (path, start) reopens it and
// skips forward, so a retry or a random jump still works, only slower.
static void doList(uint8_t seq, const uint8_t *p, uint16_t n) {
  std::string path;
  if (n < 3 || !takePath(p + 2, n - 2, path)) return sendStatus(CMD_LIST, seq, ST_BAD_ARG);
  uint16_t start = get16(p);
  busTake();
  if (!listDir || path != listPath || start != listNext) {
    closeList();
    listDir = FSTYPE.open(path.c_str(), "r");
    if (!listDir || !listDir.isDirectory()) {
      if (listDir) listDir.close();
      busGive();
      return sendStatus(CMD_LIST, seq, ST_NOT_FOUND);
    }
    listPath = path;
    while (listNext < start) {
      File f = listDir.openNextFile();
      if (!f) break;
      f.close();
      listNext++;
    }
  }
  size_t pos = 3;
  uint8_t count = 0;
  bool more = true;
  while (count < 255) {
    if (!listPending.valid) {
      File f = listDir.openNextFile();
      if (!f) { more = false; break; }
      listPending.valid = true;
      listPending.flags = f.isDirectory() ? 1 : 0;
      listPending.size  = f.isDirectory() ? 0 : (uint32_t)f.size();
      listPending.name  = baseName(f.name());
      f.close();
      if (listPending.name.size() > 255) listPending.name.resize(255);
    }
    size_t need = 6 + listPending.name.size();
    if (pos + need > SDS_MAX_PAYLOAD) break;       // does not fit: it leads the next page
    rp[pos] = listPending.flags;
    put32(rp + pos + 1, listPending.size);
    rp[pos + 5] = (uint8_t)listPending.name.size();
    memcpy(rp + pos + 6, listPending.name.data(), listPending.name.size());
    pos += need;
    count++;
    listNext++;
    listPending.valid = false;
  }
  if (!more) closeList();
  busGive();
  rp[0] = ST_OK; rp[1] = count; rp[2] = more ? 1 : 0;
  sendReply(CMD_LIST, seq, pos);
}

static void doStat(uint8_t seq, const uint8_t *p, uint16_t n) {
  std::string path;
  if (!takePath(p, n, path)) return sendStatus(CMD_STAT, seq, ST_BAD_ARG);
  busTake();
  File f = FSTYPE.open(path.c_str(), "r");
  if (!f) { busGive(); return sendStatus(CMD_STAT, seq, ST_NOT_FOUND); }
  rp[0] = ST_OK;
  rp[1] = f.isDirectory() ? 1 : 0;
  put32(rp + 2, f.isDirectory() ? 0 : (uint32_t)f.size());
  f.close();
  busGive();
  sendReply(CMD_STAT, seq, 6);
}

static void doRdOpen(uint8_t seq, const uint8_t *p, uint16_t n) {
  std::string path;
  if (!takePath(p, n, path)) return sendStatus(CMD_RDOPEN, seq, ST_BAD_ARG);
  busTake();
  if (rdFile) rdFile.close();
  rdFile = FSTYPE.open(path.c_str(), "r");
  if (!rdFile || rdFile.isDirectory()) {
    if (rdFile) rdFile.close();
    busGive();
    return sendStatus(CMD_RDOPEN, seq, ST_NOT_FOUND);
  }
  uint32_t size = rdFile.size();
  busGive();
  rp[0] = ST_OK; put32(rp + 1, size);
  sendReply(CMD_RDOPEN, seq, 5);
}

// READ [offset u32][len u16] -> [st][data]. Short data = end of file.
static void doRead(uint8_t seq, const uint8_t *p, uint16_t n) {
  if (n != 6) return sendStatus(CMD_READ, seq, ST_BAD_ARG);
  if (!rdFile) return sendStatus(CMD_READ, seq, ST_NOT_OPEN);
  uint32_t off = get32(p);
  uint16_t len = get16(p + 4);
  if (len > SDS_MAX_PAYLOAD - 1) len = SDS_MAX_PAYLOAD - 1;
  busTake();
  bool ok = rdFile.position() == off || rdFile.seek(off);
  size_t got = ok ? rdFile.read(rp + 1, len) : 0;
  busGive();
  if (!ok) return sendStatus(CMD_READ, seq, ST_IO);
  rp[0] = ST_OK;
  sendReply(CMD_READ, seq, 1 + got);
}

static void doRdClose(uint8_t seq) {
  busTake();
  if (rdFile) rdFile.close();
  busGive();
  sendStatus(CMD_RDCLOSE, seq, ST_OK);
}

static void doWrOpen(uint8_t seq, const uint8_t *p, uint16_t n) {
  std::string path;
  if (!takePath(p, n, path) || path == "/") return sendStatus(CMD_WROPEN, seq, ST_BAD_ARG);
  abortWrite();
  busTake();
  wrFile = FSTYPE.open((path + ".part").c_str(), "w");   // truncate: FILE_WRITE is O_APPEND on arduino-pico
  busGive();
  if (!wrFile) return sendStatus(CMD_WROPEN, seq, ST_IO);
  wrPath = path;
  sendStatus(CMD_WROPEN, seq, ST_OK);
}

// WRITE [offset u32][data] -> [st][position u32]. A resend of a chunk whose reply was lost comes
// back with an offset behind the file position; seeking there and rewriting it is harmless.
static void doWrite(uint8_t seq, const uint8_t *p, uint16_t n) {
  if (n < 4) return sendStatus(CMD_WRITE, seq, ST_BAD_ARG);
  if (!wrFile) return sendStatus(CMD_WRITE, seq, ST_NOT_OPEN);
  uint32_t off = get32(p);
  size_t len = n - 4;
  busTake();
  uint32_t cur = wrFile.position();
  bool ok = off <= cur && (off == cur || wrFile.seek(off));
  size_t put = ok ? wrFile.write(p + 4, len) : 0;
  uint32_t pos = wrFile.position();
  busGive();
  if (!ok || put != len) return sendStatus(CMD_WRITE, seq, off > cur ? ST_BAD_ARG : ST_IO);
  rp[0] = ST_OK; put32(rp + 1, pos);
  sendReply(CMD_WRITE, seq, 5);
}

// WRCLOSE [commit u8]: commit renames <path>.part over <path>; 0 throws the upload away.
static void doWrClose(uint8_t seq, const uint8_t *p, uint16_t n) {
  if (n != 1) return sendStatus(CMD_WRCLOSE, seq, ST_BAD_ARG);
  if (!wrFile) {
    // Resent WRCLOSE after its reply was lost: the first one already did the work.
    return sendStatus(CMD_WRCLOSE, seq, wrPath.empty() ? ST_OK : ST_NOT_OPEN);
  }
  if (!p[0]) { abortWrite(); return sendStatus(CMD_WRCLOSE, seq, ST_OK); }
  std::string part = wrPath + ".part";
  busTake();
  wrFile.close();
  if (FSTYPE.exists(wrPath.c_str())) FSTYPE.remove(wrPath.c_str());
  bool ok = FSTYPE.rename(part.c_str(), wrPath.c_str());
  busGive();
  wrPath.clear();
  sendStatus(CMD_WRCLOSE, seq, ok ? ST_OK : ST_IO);
}

static void doMkdir(uint8_t seq, const uint8_t *p, uint16_t n) {
  std::string path;
  if (!takePath(p, n, path) || path == "/") return sendStatus(CMD_MKDIR, seq, ST_BAD_ARG);
  busTake();
  File f = FSTYPE.open(path.c_str(), "r");
  bool isDir = f && f.isDirectory();
  if (f) f.close();
  bool ok = isDir || FSTYPE.mkdir(path.c_str());   // already there counts as success
  busGive();
  sendStatus(CMD_MKDIR, seq, ok ? ST_OK : ST_IO);
}

// REMOVE [path]: a file, or an EMPTY directory. The host walks trees itself.
static void doRemove(uint8_t seq, const uint8_t *p, uint16_t n) {
  std::string path;
  if (!takePath(p, n, path) || path == "/") return sendStatus(CMD_REMOVE, seq, ST_BAD_ARG);
  busTake();
  closeList();                                      // FAT will not rmdir a directory held open
  File f = FSTYPE.open(path.c_str(), "r");
  if (!f) { busGive(); return sendStatus(CMD_REMOVE, seq, ST_NOT_FOUND); }
  bool isDir = f.isDirectory();
  f.close();
  bool ok = isDir ? FSTYPE.rmdir(path.c_str()) : FSTYPE.remove(path.c_str());
  busGive();
  sendStatus(CMD_REMOVE, seq, ok ? ST_OK : ST_IO);
}

// RENAME [from]\0[to]
static void doRename(uint8_t seq, const uint8_t *p, uint16_t n) {
  const uint8_t *z = (const uint8_t *)memchr(p, 0, n);
  std::string from, to;
  if (!z || !takePath(p, z - p, from) || !takePath(z + 1, n - (z - p) - 1, to) || from == "/" || to == "/")
    return sendStatus(CMD_RENAME, seq, ST_BAD_ARG);
  busTake();
  closeList();
  // FAT names are case-insensitive: "a.dsk" -> "A.DSK" sees its own target as existing.
  bool caseOnly = strcasecmp(from.c_str(), to.c_str()) == 0;
  bool hasFrom = FSTYPE.exists(from.c_str());
  bool hasTo = !caseOnly && FSTYPE.exists(to.c_str());
  // Source gone and target present: a resend whose first reply was lost, already done.
  bool ok = hasFrom ? !hasTo && FSTYPE.rename(from.c_str(), to.c_str()) : hasTo;
  busGive();
  sendStatus(CMD_RENAME, seq, ok ? ST_OK : ST_FAIL);
}

static void doBaud(uint8_t seq, const uint8_t *p, uint16_t n) {
#if SDS_USB_LINK
  (void)p; (void)n;
  sendStatus(CMD_BAUD, seq, ST_UNSUPPORTED);
#else
  if (n != 4) return sendStatus(CMD_BAUD, seq, ST_BAD_ARG);
  uint32_t baud = get32(p);
  if (baud < 9600 || baud > 2000000) return sendStatus(CMD_BAUD, seq, ST_BAD_ARG);
  sendStatus(CMD_BAUD, seq, ST_OK);                 // acknowledged at the OLD rate
  Serial.flush();
  delay(20);                                        // let the host see it before the line changes
  Serial.updateBaudRate(baud);
  currentBaud = baud;
  baudSwitchedMs = baud == SDS_DEFAULT_BAUD ? 0 : nowMs();
  lastFrameMs = nowMs();
#endif
}

static void dispatch(uint8_t cmd, uint8_t seq, const uint8_t *p, uint16_t n) {
  if (cmd != CMD_HELLO && cmd != CMD_BYE && cmd != CMD_REBOOT && cmd != CMD_BAUD && !sdMounted())
    return sendStatus(cmd, seq, ST_NO_SD);
  switch (cmd) {
    case CMD_HELLO:   doHello(seq); break;
    case CMD_FSINFO:  doFsInfo(seq); break;
    case CMD_LIST:    doList(seq, p, n); break;
    case CMD_STAT:    doStat(seq, p, n); break;
    case CMD_RDOPEN:  doRdOpen(seq, p, n); break;
    case CMD_READ:    doRead(seq, p, n); break;
    case CMD_RDCLOSE: doRdClose(seq); break;
    case CMD_WROPEN:  doWrOpen(seq, p, n); break;
    case CMD_WRITE:   doWrite(seq, p, n); break;
    case CMD_WRCLOSE: doWrClose(seq, p, n); break;
    case CMD_MKDIR:   doMkdir(seq, p, n); break;
    case CMD_REMOVE:  doRemove(seq, p, n); break;
    case CMD_RENAME:  doRename(seq, p, n); break;
    case CMD_BAUD:    doBaud(seq, p, n); break;
    case CMD_REBOOT:
      sendStatus(cmd, seq, ST_OK);
      endSession();
#if !defined(SDSERIAL_HOST)
      Serial.flush();
      delay(100);
      ESP.restart();
#endif
      break;
    case CMD_BYE:
      sendStatus(cmd, seq, ST_OK);
#if !defined(SDSERIAL_HOST)
      Serial.flush();
#endif
      endSession();
      break;
    default:          sendStatus(cmd, seq, ST_UNKNOWN_CMD); break;
  }
}

// ---------------------------------------------------------------------------------------------
// Frame parser: magic -> header -> payload+crc. A bad CRC or oversized length just drops back to
// hunting for the magic; the host resends on timeout.
// ---------------------------------------------------------------------------------------------
static uint8_t  rxState = 0;     // 0: want A5, 1: want 5A, 2: header+body
static uint32_t lastByteMs = 0;

static void feed(uint8_t b) {
  static size_t have = 0, need = 0;
  uint8_t &state = rxState;
  switch (state) {
    case 0: if (b == 0xA5) state = 1; return;
    case 1: state = (b == 0x5A) ? 2 : (b == 0xA5 ? 1 : 0); have = 0; need = 4; return;
  }
  rxBuf[have++] = b;
  if (have == 4) {
    uint16_t len = get16(rxBuf + 2);
    if (len > SDS_MAX_PAYLOAD) { state = 0; return; }
    need = 4 + len + 2;
  }
  if (have < need) return;
  state = 0;
  uint16_t len = get16(rxBuf + 2);
  if (crc16(rxBuf, 4 + len) != get16(rxBuf + 4 + len)) return;
  lastFrameMs = nowMs();
  baudSwitchedMs = 0;           // a good frame at the new rate: the switch took
  sdSerialActive = true;
  dispatch(rxBuf[0], rxBuf[1], rxBuf + 4, len);
}

void sdSerialPoll() {
  if (!rxBuf) {
    // Lazily, on the first byte from a host -- not on the first poll, which is right after boot.
    // Until something talks to us these 2K belong to the heap the emulator is still using.
    if (portAvailable() <= 0) return;
    rxBuf = (uint8_t *)malloc(6 + SDS_MAX_PAYLOAD + 2);
    txBuf = (uint8_t *)malloc(2 + 4 + SDS_MAX_PAYLOAD + 2);
    if (!rxBuf || !txBuf) { free(rxBuf); free(txBuf); rxBuf = txBuf = nullptr; return; }
    rp = txBuf + 6;
  }
  int budget = 2 * (SDS_MAX_PAYLOAD + 16);   // bound one pass so a flood cannot pin the task
  uint32_t now = nowMs();
  // A frame that stalls part-way had a corrupted length (or lost bytes): drop it, so the host's
  // resend, which comes after a much longer timeout, starts from a clean parser.
  if (rxState && now - lastByteMs > 200) rxState = 0;
  while (budget-- > 0 && portAvailable() > 0) {
    int c = portRead();
    if (c < 0) break;
    lastByteMs = now;
    feed((uint8_t)c);
  }
  now = nowMs();
  if (baudSwitchedMs && now - baudSwitchedMs > SDS_BAUD_REVERT_MS) {
    // The host never reached us at the new rate: go back so it can reconnect at the default.
    revertBaud();
  }
  if (sdSerialActive && now - lastFrameMs > SDS_SESSION_TIMEOUT_MS) endSession();
}

#if !defined(SDSERIAL_HOST)
static void sdSerialTask(void *) {
  for (;;) {
    sdSerialPoll();
    idle(sdSerialActive || portAvailable() > 0);
  }
}

// Started only in SD Manager mode (emu8.ino), where no emulator is set up and the whole heap is
// free. It used to run beside every emulator, and on the PicoCalc's RP2040 its task was the
// allocation that tipped a IIe boot into "FATAL: pvPortMalloc failed".
void sdSerialSetup() {
  xTaskCreatePinnedToCore(sdSerialTask, "sdserial", 4096, NULL, 1, NULL, 0);
}
#endif

#endif  // !BOARD_DESKTOP || SDSERIAL_HOST
