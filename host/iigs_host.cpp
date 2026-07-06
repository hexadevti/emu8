// iigs_host.cpp - desktop harness to bring up the Apple IIGS ROM boot on the PC (fast iteration).
//
// Reuses the device CPU core (src/iigs/cpu65816.cpp) verbatim and implements the IIGS reset-time
// memory map here so I can iterate the map / softswitches in milliseconds, then port the working
// logic to src/iigs/iigs_mem.cpp for the board. Build:
//   g++ -O2 -I. -o iigs_host host/iigs_host.cpp src/iigs/cpu65816.cpp
// Run:  ./iigs_host "resources/Apple IIGS ROM 01 - 342-0077-B.bin"

#include "../src/iigs/cpu65816.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static uint8_t* MEM;                 // 16MB flat RAM model (banks $00..$FD general RAM)
static uint8_t  ROM[131072];         // ROM 01: [0..0xFFFF]=bank $FF (monitor+vectors), [0x10000..0x1FFFF]=bank $FE
                                     // (empirical: reset vector $FA62 is at file offset 0x0FFFC -> first 64K = $FF)

static long     ioRd[256], ioWr[256];   // $C000-$C0FF read/write counts
static uint8_t  ioWrLast[256];
static long     ioRdOrder[256]; static int ioRdSeen = 0;   // first-seen order for read addrs
static bool     gTrace = false;          // when set, log every memory access (for loop diagnosis)
static long     gInstr = 0;              // instruction counter (drives simulated VBL timing)
static uint32_t gPC = 0;                 // current instruction PC (24-bit), for I/O-access source tracing
static long     gFatalAt = -1;           // instr where the "Fatal system error" string ($FF:8A81) is first touched

static inline uint8_t romFF(uint16_t off) { return ROM[off]; }             // bank $FF = first 64K
static inline uint8_t romFE(uint16_t off) { return ROM[0x10000 + off]; }   // bank $FE = second 64K

// ---- I/O ($C000-$C0FF in banks $00/$01). Stub: log; return 0 for reads. -------------------------
// ============================ Disk II / IWM (5.25", slot 6, $C0E0-$C0EF) ============================
// Ports the GCR 6&2 nibblizer + head-stepping from src/apple2/disk.cpp, plus the IIGS IWM mode
// register ($C0EE/$C0EF with motor off) the firmware probes at $FF:6A5F. DOS-order .dsk.
static uint8_t* g_disk = nullptr; static long g_diskLen = 0;   // raw image (143360 = 35*16*256)
static const uint8_t XLT[64] = {
  0x96,0x97,0x9A,0x9B,0x9D,0x9E,0x9F,0xA6, 0xA7,0xAB,0xAC,0xAD,0xAE,0xAF,0xB2,0xB3,
  0xB4,0xB5,0xB6,0xB7,0xB9,0xBA,0xBB,0xBC, 0xBD,0xBE,0xBF,0xCB,0xCD,0xCE,0xCF,0xD3,
  0xD6,0xD7,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE, 0xDF,0xE5,0xE6,0xE7,0xE9,0xEA,0xEB,0xEC,
  0xED,0xEE,0xEF,0xF2,0xF3,0xF4,0xF5,0xF6, 0xF7,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF };
static const uint8_t DOTRK[16] = {0x0,0x7,0xe,0x6,0xd,0x5,0xc,0x4,0xb,0x3,0xa,0x2,0x9,0x1,0x8,0xf};

static void enc44(uint8_t d, uint8_t* o) { o[0] = (d >> 1) | 0xAA; o[1] = d | 0xAA; }   // 4&4
static void enc62(const uint8_t* in, uint8_t* agg) {                                    // 6&2 (343 bytes)
  uint8_t od[256], lt[0x56] = {0};
  for (int i = 0; i < 256; i++) {
    uint8_t b = in[i]; od[i] = b >> 2; int t = ((b & 1) ? 2 : 0) + ((b & 2) ? 1 : 0);
    if (i < 86) lt[i] |= t; else if (i < 172) lt[i - 86] |= t << 2; else lt[i - 172] |= t << 4;
  }
  uint8_t last = 0;
  for (int i = 0; i < 86; i++)  { agg[i]      = XLT[lt[i] ^ last]; last = lt[i]; }
  for (int i = 0; i < 256; i++) { agg[86 + i] = XLT[od[i] ^ last]; last = od[i]; }
  agg[342] = XLT[last];
}
static int nibblizeTrack(int track, uint8_t* enc) {
  static const uint8_t phys[16] = {0xa,0xb,0xc,0xd,0xe,0xf,0x0,0x1,0x2,0x3,0x4,0x5,0x6,0x7,0x8,0x9};
  const uint8_t vol = 254; const uint8_t* trk = g_disk + (long)track * 4096;
  int p = 0; uint8_t b[2];
  for (int s = 0; s < 16; s++) {
    uint8_t isec = phys[s];
    enc[p++] = 0xff; enc[p++] = 0xff; enc[p++] = 0xff;
    enc[p++] = 0xd5; enc[p++] = 0xaa; enc[p++] = 0x96;                       // address prologue
    enc44(vol, b);  enc[p++] = b[0]; enc[p++] = b[1];
    enc44(track, b);enc[p++] = b[0]; enc[p++] = b[1];
    enc44(isec, b); enc[p++] = b[0]; enc[p++] = b[1];
    enc44(vol ^ (uint8_t)track ^ isec, b); enc[p++] = b[0]; enc[p++] = b[1]; // checksum
    enc[p++] = 0xde; enc[p++] = 0xaa; enc[p++] = 0xeb;                       // address epilogue
    enc[p++] = 0xd5; enc[p++] = 0xaa; enc[p++] = 0xad;                       // data prologue
    uint8_t agg[343]; enc62(trk + (long)DOTRK[isec] * 256, agg);
    for (int i = 0; i < 343; i++) enc[p++] = agg[i];
    enc[p++] = 0xde; enc[p++] = 0xaa; enc[p++] = 0xeb;                       // data epilogue
  }
  return p;   // 5856
}

// head stepping (ported addPhase: match last-4 phase events against asc/desc sequences)
static int g_track = 0; static uint8_t g_ring[4] = {0,0,0,0};
static bool seqeq(const uint8_t* a, const uint8_t* b) { return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2]&&a[3]==b[3]; }
static void addPhase(uint8_t ph) {
  g_ring[0]=g_ring[1]; g_ring[1]=g_ring[2]; g_ring[2]=g_ring[3]; g_ring[3]=ph;
  static const uint8_t eo_asc[4]={0x11,0x00,0x21,0x10}, oe_asc[4]={0x31,0x20,0x01,0x30};
  static const uint8_t eo_dsc[4]={0x31,0x00,0x21,0x30}, oe_dsc[4]={0x11,0x20,0x01,0x10};
  static const uint8_t st1[4]={0x31,0x30,0x21,0x20}, st2[4]={0x11,0x10,0x01,0x00};
  int t = g_track;
  if (t%2==0 && seqeq(g_ring,eo_asc)) t++;
  else if (t%2!=0 && seqeq(g_ring,oe_asc)) t++;
  else if (t%2==0 && seqeq(g_ring,eo_dsc) && t>0) t--;
  else if (t%2!=0 && seqeq(g_ring,oe_dsc) && t>0) t--;
  else if (t>0 && (seqeq(g_ring,st1) || seqeq(g_ring,st2))) t--;
  if (t < 0) t = 0; if (t > 34) t = 34;
  g_track = t;
}

static uint8_t g_enc[5856]; static int g_encLen = 0, g_nib = 0, g_curTrk = -1;
static bool g_motor = false, g_q6h = false, g_q7h = false; static uint8_t g_iwmMode = 0;

static uint8_t diskIO(uint16_t off, bool write, uint8_t val) {
  switch (off) {
    case 0xC0E0: addPhase(0x00); break; case 0xC0E1: addPhase(0x01); break;
    case 0xC0E2: addPhase(0x10); break; case 0xC0E3: addPhase(0x11); break;
    case 0xC0E4: addPhase(0x20); break; case 0xC0E5: addPhase(0x21); break;
    case 0xC0E6: addPhase(0x30); break; case 0xC0E7: addPhase(0x31); break;
    case 0xC0E8: g_motor = false; break; case 0xC0E9: g_motor = true; break;
    case 0xC0EA: case 0xC0EB: break;                       // drive 1/2 select
    case 0xC0EC: g_q6h = false; break;                     // Q6L (read data)
    case 0xC0ED: g_q6h = true;  break;                     // Q6H (load)
    case 0xC0EE: g_q7h = false; break;                     // Q7L (read mode / status)
    case 0xC0EF: g_q7h = true; if (write && !g_motor) g_iwmMode = val & 0x1F; break;   // Q7H (write/mode)
  }
  if (!write) {
    if (off == 0xC0EC && !g_q7h) {                         // stream a GCR nibble in read mode
      if (!g_disk) return 0;                               // no 5.25" disk in the drive
      if (g_curTrk != g_track) { g_encLen = nibblizeTrack(g_track, g_enc); g_curTrk = g_track; g_nib = 0; }
      if (g_nib >= g_encLen) g_nib = 0;
      return g_enc[g_nib++];
    }
    if (off == 0xC0EE) return g_motor ? 0x00 : g_iwmMode;  // motor off: IWM status=mode; on: not write-prot
  }
  return 0;
}

// ---- ADB GLU ($C026 data / $C027 status): the IIGS keyboard/mouse/clock/BRAM microcontroller ----
// Minimal model: command state machine + a 256-byte microcontroller RAM (the BRAM the self-test
// writes/reads via $08/$09). $C027 bit0 = command-reg busy (0=ready), bit5 = response available.
static uint8_t gluMem[256];          // microcontroller / battery RAM
static uint8_t gluCmd = 0; static int gluArgsLeft = 0; static uint8_t gluAddr = 0;
static uint8_t gLastGluCmd = 0;      // most recent command byte (for read-poll diagnosis)
static uint8_t gluResp[32]; static int gluRespLen = 0, gluRespPos = 0;
static int     gGluLog = 0;
static void gluPushResp(uint8_t b) { if (gluRespLen < 32) gluResp[gluRespLen++] = b; }

static void gluWrite(uint8_t v) {
  if (gGluLog < 80) { printf("GLU W %02X %s\n", v, gluArgsLeft ? "(arg)" : "(cmd)"); gGluLog++; }
  if (gluArgsLeft > 0) {                       // argument byte for the in-flight command
    switch (gluCmd) {
      case 0x08: if (gluArgsLeft == 2) gluAddr = v; else gluMem[gluAddr] = v; break;   // write mem: addr,data
      case 0x09: gluAddr = v; gluPushResp(gluMem[gluAddr]); break;                      // read mem: addr -> resp
      default: break;                          // sync/config/etc: consume + ignore
    }
    gluArgsLeft--;
    return;
  }
  gluCmd = v; gLastGluCmd = v;                 // new command
  switch (v) {
    case 0x07: gluArgsLeft = 4; break;         // sync (4 mode bytes)
    case 0x08: gluArgsLeft = 2; break;         // write microcontroller mem (addr, data)
    case 0x09: gluArgsLeft = 1; break;         // read microcontroller mem (addr) -> response
    case 0x04: case 0x05: gluArgsLeft = 1; break;     // set/clear modes
    case 0x06: gluArgsLeft = 3; break;         // set config
    case 0x0A: gluPushResp(0x00); break;       // read modes
    case 0x0B: gluPushResp(0); gluPushResp(0); gluPushResp(0); break;  // read config
    case 0x0D: gluPushResp(0x06); break;       // read version
    case 0x0E: gluPushResp(0x01); gluPushResp(0x00); break;       // read available char sets
    case 0x0F: gluPushResp(0x01); gluPushResp(0x00); gluPushResp(0x00); gluPushResp(0x00); break;  // read avail layouts
    default: break;                            // 0-arg / unknown (logged)
  }
  if (gGluLog < 90) printf("   cmd=%02X -> argsLeft=%d respQueued=%d\n", v, gluArgsLeft, gluRespLen - gluRespPos);
}
static uint8_t gluReadData() {
  if (gluRespPos < gluRespLen) { uint8_t b = gluResp[gluRespPos++]; if (gluRespPos >= gluRespLen) gluRespLen = gluRespPos = 0; return b; }
  return 0;
}
static uint8_t gluStatus() { return (gluRespPos < gluRespLen) ? 0x20 : 0x00; }   // bit5 = response avail

// ============== ProDOS block device / HD (slot 7, $C0F0-$C0F7) ==============
// Reuses the Apple IIe approach: a tiny slot-7 firmware (hdrom, mapped at $C700) that the boot
// scanner / ProDOS calls; it pokes the call params to $C0F2-$C0F7 and reads $C0F0 to execute a
// 512-byte block read/write from the .po/.2mg image (ProDOS-order, header stripped). No GCR.
static const uint8_t hdrom[256] = {
   0xA9,0x20,0xA9,0x00,0xA9,0x03,0xA9,0x3C,0xD0,0x3F,0x38,0xB0,0x01,0x18,0xB0,0x7D,
   0x68,0x85,0x46,0x69,0x03,0xA8,0x68,0x85,0x47,0x69,0x00,0x48,0x98,0x48,0xA0,0x01,
   0xB1,0x46,0x85,0x42,0xC8,0xB1,0x46,0x85,0x45,0xC8,0xB1,0x46,0x85,0x46,0xA0,0x01,
   0xB1,0x45,0x85,0x43,0xC8,0xB1,0x45,0x85,0x44,0xC8,0xB1,0x45,0x48,0xC8,0xB1,0x45,
   0x48,0xC8,0xD0,0x3E,0x00,0x00,0x38,0xB0,0xC5,0x18,0x90,0x41,0xA9,0x00,0x9D,0x83,
   0xC0,0x9D,0x82,0xC0,0xBD,0x80,0xC0,0x7E,0x81,0xC0,0x90,0x08,0x4C,0x00,0xC6,0x00,
   0x00,0x38,0xB0,0xAA,0xA9,0x00,0x85,0x43,0x85,0x44,0x85,0x46,0x85,0x47,0xA9,0x08,
   0x85,0x45,0xA9,0x01,0x85,0x42,0xD0,0x2E,0xB0,0xE2,0x2C,0x61,0xC0,0x30,0xDD,0x4C,
   0x01,0x08,0xB1,0x45,0x85,0x47,0x68,0x85,0x46,0x68,0x85,0x45,0x38,0x08,0x78,0xA5,
   0x00,0xA2,0x60,0x86,0x00,0x20,0x00,0x00,0x85,0x00,0xBA,0xBD,0x00,0x01,0x0A,0x0A,
   0x0A,0x0A,0xAA,0x28,0x90,0xA6,0x08,0xA5,0x42,0x9D,0x82,0xC0,0xA5,0x43,0x9D,0x83,
   0xC0,0xA5,0x44,0x9D,0x84,0xC0,0xA5,0x45,0x9D,0x85,0xC0,0xA5,0x46,0x9D,0x86,0xC0,
   0xA5,0x47,0x9D,0x87,0xC0,0xBD,0x80,0xC0,0x3E,0x81,0xC0,0xB0,0xFB,0x28,0xB0,0x07,
   0x7E,0x81,0xC0,0xA9,0x00,0xF0,0xA1,0x7E,0x81,0xC0,0x60,0x00,0x00,0x00,0x00,0x00,
   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x7F,0xD7,0x0A,
};
static uint8_t* g_hd = nullptr; static long g_hdLen = 0; static int g_hdHdr = 0;
static uint8_t hdCmd = 0, hdUnit = 0, hdStatus = 0;
static uint16_t hdBufAddr = 0, hdBlock = 0;
static uint16_t hdBlockCount() { return g_hd ? (uint16_t)((g_hdLen - g_hdHdr) / 512) : 0; }
static uint8_t hdExec() {
  if (!g_hd) return 0x01;
  long pos = (long)hdBlock * 512 + g_hdHdr;
  if (pos < 0 || pos + 512 > g_hdLen) return 0x01;
  if (hdCmd == 1)      for (int k = 0; k < 512; k++) MEM[(uint16_t)(hdBufAddr + k)] = g_hd[pos + k];   // READ
  else if (hdCmd == 2) for (int k = 0; k < 512; k++) g_hd[pos + k] = MEM[(uint16_t)(hdBufAddr + k)];   // WRITE
  return 0;
}
static uint8_t hdRead(uint16_t off) {
  switch (off) {
    case 0xC0F0: hdStatus = hdExec(); return hdStatus;
    case 0xC0F1: return hdStatus;
    case 0xC0F2: return hdCmd;     case 0xC0F3: return hdUnit;
    case 0xC0F4: return hdBufAddr & 0xFF; case 0xC0F5: return hdBufAddr >> 8;
    case 0xC0F6: return hdBlock & 0xFF;   case 0xC0F7: return hdBlock >> 8;
    case 0xC0F9: return hdBlockCount() & 0xFF; case 0xC0FA: return hdBlockCount() >> 8;
  }
  return 0;
}
static void hdWrite(uint16_t off, uint8_t v) {
  switch (off) {
    case 0xC0F2: hdCmd = v; break;  case 0xC0F3: hdUnit = v; break;
    case 0xC0F4: hdBufAddr = (hdBufAddr & 0xFF00) | v; break;
    case 0xC0F5: hdBufAddr = (hdBufAddr & 0x00FF) | (v << 8); break;
    case 0xC0F6: hdBlock = (hdBlock & 0xFF00) | v; break;
    case 0xC0F7: hdBlock = (hdBlock & 0x00FF) | (v << 8); break;
  }
}

static uint8_t ioRead(uint16_t off) {
  int i = off - 0xC000;
  if (ioRd[i] == 0) ioRdOrder[ioRdSeen++] = off;
  ioRd[i]++;
  if (off == 0xC031 || (off >= 0xC0D0 && off <= 0xC0DF)) { static int n=0; if(n<60){ printf("3.5 R $%04X @PC=%06X\n", off, gPC); n++; } }
  if (off >= 0xC0E0 && off <= 0xC0EF) return diskIO(off, false, 0);   // Disk II / IWM (slot 6)
  if (off >= 0xC0F0 && off <= 0xC0FA) return hdRead(off);             // ProDOS block device (slot 7)
  switch (off) {
    case 0xC019: return ((gInstr / 600) & 1) ? 0x80 : 0x00;   // VBL toggle
    case 0xC026: return gluReadData();
    case 0xC027: return gluStatus();
    default:     return 0x00;
  }
}
static void ioWrite(uint16_t off, uint8_t v) {
  int i = off - 0xC000; ioWr[i]++; ioWrLast[i] = v;
  if (off == 0xC031 || (off >= 0xC0D0 && off <= 0xC0DF)) { static int n=0; if(n<60){ printf("3.5 W $%04X<-%02X @PC=%06X\n", off, v, gPC); n++; } }
  if (off == 0xC029) printf("$C029 (New Video) <- %02X  (SHR %s)\n", v, (v & 0x80) ? "ON" : "off");
  if (off == 0xC035) printf("$C035 (Shadow reg) <- %02X\n", v);
  if (off >= 0xC0E0 && off <= 0xC0EF) { diskIO(off, true, v); return; }
  if (off >= 0xC0F0 && off <= 0xC0FA) { hdWrite(off, v); return; }   // ProDOS block device (slot 7)
  if (off == 0xC026) gluWrite(v);
}

// ---- IIGS reset-time memory map ----------------------------------------------------------------
// The $C000-$CFFF I/O space lives in banks $00/$01 (fast) AND $E0/$E1 (Mega II / slow side).
static inline bool ioBank(uint8_t b) { return b == 0x00 || b == 0x01 || b == 0xE0 || b == 0xE1; }

// ---- language card ($D000-$FFFF read/write split, $C080-$C08F switches), banks $00/$01 ----
static bool lcReadRAM[2] = {false, false}, lcWriteRAM[2] = {false, false}, lcBank2[2] = {false, false};
static int  lcPreWrite[2] = {0, 0};
static uint8_t lcB2[2][0x1000];   // the second $D000-$DFFF bank
static void lcSwitch(int b, uint16_t addr, bool isRead) {
  lcBank2[b] = !(addr & 8);                 // $C088-8F=bank1, $C080-87=bank2
  int mode = addr & 3;
  lcReadRAM[b] = (mode == 0 || mode == 3);
  if (mode == 1 || mode == 3) {             // write-enable needs two consecutive READS of the switch
    if (isRead) { if (++lcPreWrite[b] >= 2) lcWriteRAM[b] = true; } else lcPreWrite[b] = 0;
  } else { lcWriteRAM[b] = false; lcPreWrite[b] = 0; }
}
static uint8_t lcRead(int b, uint16_t off) {
  if (off < 0xE000 && lcBank2[b]) return lcB2[b][off - 0xD000];
  return MEM[((uint32_t)b << 16) | off];
}
static void lcWrite(int b, uint16_t off, uint8_t v) {
  if (off < 0xE000 && lcBank2[b]) lcB2[b][off - 0xD000] = v;
  else MEM[((uint32_t)b << 16) | off] = v;
}

static uint8_t hostRead(uint32_t a) {
  uint8_t bank = a >> 16; uint16_t off = (uint16_t)a; uint8_t v;
  if (bank == 0xFF) v = romFF(off);
  else if (bank == 0xFE) v = romFE(off);
  else if (bank == 0x00 || bank == 0x01) {
    if (off >= 0xC080 && off <= 0xC08F) { lcSwitch(bank, off, true); v = 0; }
    else if (off >= 0xC000 && off <= 0xC0FF) v = ioRead(off);
    else if (off >= 0xD000) v = lcReadRAM[bank] ? lcRead(bank, off) : romFF(off);
    else if (off >= 0xC700 && off <= 0xC7FF && g_hd) v = hdrom[off - 0xC700];  // slot 7 = our HD firmware
    else if (off >= 0xC100) v = romFF(off);          // $C100-$CFFF internal ROM
    else v = MEM[a];
  }
  else if ((bank == 0xE0 || bank == 0xE1) && off >= 0xC000 && off <= 0xC0FF) v = ioRead(off);
  else v = MEM[a];
  if (gFatalAt < 0 && a >= 0xFF8A81 && a <= 0xFF8A93) gFatalAt = gInstr;   // fatal-error string
  if (gTrace) printf("        R %06X = %02X\n", a, v);
  return v;
}
static long g_shrE1 = 0, g_shr01 = 0, g_shr00 = 0;   // SHR-region writes per bank ($2000-$9FFF)
static void hostWrite(uint32_t a, uint8_t v) {
  uint8_t bank = a >> 16; uint16_t off = (uint16_t)a;
  if (gTrace) printf("        W %06X = %02X\n", a, v);
  if (off >= 0x2000 && off <= 0x9FFF) { if (bank==0xE1) g_shrE1++; else if (bank==0x01) g_shr01++; else if (bank==0x00) g_shr00++; }
  if (bank == 0xFE || bank == 0xFF) return;                 // ROM read-only
  if (bank == 0x00 || bank == 0x01) {
    if (off >= 0xC080 && off <= 0xC08F) { lcSwitch(bank, off, false); return; }
    if (off >= 0xC000 && off <= 0xC0FF) { ioWrite(off, v); return; }
    if (off >= 0xD000) { if (lcWriteRAM[bank]) lcWrite(bank, off, v); return; }
    MEM[a] = v; return;
  }
  if ((bank == 0xE0 || bank == 0xE1) && off >= 0xC000 && off <= 0xC0FF) { ioWrite(off, v); return; }
  MEM[a] = v;
}

static const char* MN(uint8_t op);   // tiny opcode-name table (below)

int main(int argc, char** argv) {
  const char* romPath = (argc > 1) ? argv[1] : "resources/Apple IIGS ROM 01 - 342-0077-B.bin";
  FILE* f = fopen(romPath, "rb");
  if (!f) { printf("cannot open ROM: %s\n", romPath); return 1; }
  size_t n = fread(ROM, 1, sizeof(ROM), f); fclose(f);
  printf("ROM loaded: %zu bytes from %s\n", n, romPath);
  if (n != 131072) { printf("WARN: expected 131072 bytes\n"); }

  MEM = (uint8_t*)calloc(16 * 1024 * 1024, 1);

  const char* diskPath = (argc > 2) ? argv[2] : "data/ProDOS_2_4_2.dsk";
  FILE* df = fopen(diskPath, "rb");
  if (df) { g_disk = (uint8_t*)malloc(160 * 1024); g_diskLen = fread(g_disk, 1, 160 * 1024, df); fclose(df);
            printf("disk loaded: %ld bytes from %s\n", g_diskLen, diskPath); }
  else printf("NO disk image at %s\n", diskPath);

  const char* hdPath = (argc > 3) ? argv[3] : nullptr;   // arg 3 = ProDOS block device (.po/.2mg/.hdv) on slot 7
  if (hdPath) {
    FILE* hf = fopen(hdPath, "rb");
    if (hf) {
      fseek(hf, 0, SEEK_END); g_hdLen = ftell(hf); fseek(hf, 0, SEEK_SET);
      g_hd = (uint8_t*)malloc(g_hdLen);
      g_hdLen = fread(g_hd, 1, g_hdLen, hf); fclose(hf);
      g_hdHdr = 0;                                        // .po/.hdv: raw ProDOS blocks, no header
      if (g_hdLen >= 64 && g_hd[0]=='2'&&g_hd[1]=='I'&&g_hd[2]=='M'&&g_hd[3]=='G') {  // .2mg: parse header
        uint32_t off = g_hd[0x18] | (g_hd[0x19]<<8) | (g_hd[0x1A]<<16) | ((uint32_t)g_hd[0x1B]<<24);
        uint32_t fmt = g_hd[0x0C] | (g_hd[0x0D]<<8) | (g_hd[0x0E]<<16) | ((uint32_t)g_hd[0x0F]<<24);
        g_hdHdr = (off >= 64 && off < (uint32_t)g_hdLen) ? (int)off : 64;
        printf("  .2mg: format=%u (0=DOS,1=ProDOS,2=NIB) dataOffset=%u\n", fmt, off);
      }
      printf("HD loaded: %ld bytes (%d hdr, %d blocks) from %s\n", g_hdLen, g_hdHdr, hdBlockCount(), hdPath);
    } else printf("NO HD image at %s\n", hdPath);
  }

  CPU65816 cpu; memset(&cpu, 0, sizeof(cpu));
  cpu.rd = hostRead; cpu.wr = hostWrite;
  cpu.reset();
  printf("reset vector $00:FFFC -> PC=%04X  (P=%02X E=%d)\n", cpu.PC, cpu.P, cpu.E ? 1 : 0);

  // --- run, with a first-N instruction trace + tight-loop detection ---
  const long MAXI = 60L * 1000 * 1000;
  const int  TRACE_N = 48;
  long i = 0;
  uint32_t windowMin = 0xFFFFFFFF, windowMax = 0; long windowStart = 0; long loopAt = -1; uint32_t loopPC = 0;
  // ring of recent CPU state, to reconstruct the path into the fatal-error handler
  static uint32_t rPC[64]; static uint16_t rA[64], rX[64], rY[64], rS[64]; static uint8_t rP[64]; int rp = 0;
  for (; i < MAXI && !cpu.stopped; i++) {
    gInstr = i;
    uint32_t pc24 = ((uint32_t)cpu.PBR << 16) | cpu.PC; gPC = pc24;
    if (gFatalAt < 0 && pc24 == 0xFF8442) { static int n=0; if(n<30){ printf("8442 read-poll: lastGluCmd=$%02X respLen=%d\n", gLastGluCmd, gluRespLen-gluRespPos); n++; } }
    if (gFatalAt < 0 && pc24 == 0xFF8454) { printf(">>> 8442 TIMEOUT after lastGluCmd=$%02X\n", gLastGluCmd); }
    if (gFatalAt < 0 && pc24 == 0xFF8659) { gFatalAt = i; gTrace = true; }   // the $0911 error path entry
    rPC[rp] = pc24; rA[rp] = cpu.A; rX[rp] = cpu.X; rY[rp] = cpu.Y; rS[rp] = cpu.S; rP[rp] = cpu.P; rp = (rp + 1) & 63;
    if (gFatalAt >= 0) {
      printf("\n=== reached FATAL-error string at instr %ld; last 48 instructions (oldest->newest): ===\n", i);
      for (int k = 0; k < 48; k++) {
        int idx = (rp + k) & 63;
        printf("  %02X:%04X  A=%04X X=%04X Y=%04X S=%04X P=%02X\n",
               rPC[idx] >> 16, rPC[idx] & 0xFFFF, rA[idx], rX[idx], rY[idx], rS[idx], rP[idx]);
      }
      printf("  stack top ($01:%04X..): ", (cpu.S + 1) & 0xFFFF);
      for (int s = 1; s <= 10; s++) printf("%02X ", hostRead((uint16_t)(cpu.S + s)));
      printf("\n");
      break;
    }
    if (i < TRACE_N) {
      uint8_t op = hostRead(pc24);
      printf("  %02X:%04X  %02X %-4s  A=%04X X=%04X Y=%04X S=%04X P=%02X E=%d\n",
             cpu.PBR, cpu.PC, op, MN(op), cpu.A, cpu.X, cpu.Y, cpu.S, cpu.P, cpu.E ? 1 : 0);
    }
    // sliding-window loop detect (every instruction): if 200k consecutive PCs stay in a 64-byte span
    if (pc24 < windowMin) windowMin = pc24;
    if (pc24 > windowMax) windowMax = pc24;
    if ((i - windowStart) >= 200000) {
      if (windowMax - windowMin < 64) {
        printf("\n=== tight loop near %02X:%04X - detailed trace (40 steps, R/W logged) ===\n", cpu.PBR, cpu.PC);
        gTrace = true;
        for (int t = 0; t < 40 && !cpu.stopped; t++) {
          uint32_t p = ((uint32_t)cpu.PBR << 16) | cpu.PC;
          printf("  [%02X:%04X op=%02X %-4s A=%04X X=%04X Y=%04X S=%04X P=%02X E=%d]\n",
                 cpu.PBR, cpu.PC, romFF((uint16_t)cpu.PC) /*peek only valid if bank FF*/,
                 "", cpu.A, cpu.X, cpu.Y, cpu.S, cpu.P, cpu.E ? 1 : 0);
          (void)p;
          cpu.step();
        }
        gTrace = false;
        loopAt = i; loopPC = cpu.PC; break;
      }
      windowMin = 0xFFFFFFFF; windowMax = 0; windowStart = i;
    }
    cpu.step();
  }

  printf("\n--- after %ld instructions ---\n", i);
  printf("end: PBR:PC=%02X:%04X  A=%04X X=%04X Y=%04X S=%04X D=%04X DBR=%02X P=%02X E=%d  %s\n",
         cpu.PBR, cpu.PC, cpu.A, cpu.X, cpu.Y, cpu.S, cpu.D, cpu.DBR, cpu.P, cpu.E ? 1 : 0,
         cpu.stopped ? "(STP)" : (loopAt >= 0 ? "(TIGHT LOOP)" : "(ran out)"));
  if (loopAt >= 0) printf("tight loop detected near PC=$%04X (bank %02X)\n", loopPC, cpu.PBR);
  printf("SHR-region writes ($2000-$9FFF): bank $E1=%ld  $01=%ld  $00=%ld   | $C029 last=%02X (SHR %s)  $C035 last=%02X\n",
         g_shrE1, g_shr01, g_shr00, ioWrLast[0x29], (ioWrLast[0x29] & 0x80) ? "ON" : "off", ioWrLast[0x35]);

  // dump the 40-col text screen (Apple II interleaved layout) from both the $00 and $E0 banks
  for (int pass = 0; pass < 2; pass++) {
    uint32_t base = pass ? 0xE00000u : 0x000000u;
    printf("\n--- text screen (bank %s) ---\n", pass ? "E0" : "00");
    for (int r = 0; r < 24; r++) {
      char line[41];
      uint32_t ro = (r % 8) * 0x80 + (r / 8) * 0x28;
      for (int c = 0; c < 40; c++) { uint8_t ch = MEM[base + 0x400 + ro + c] & 0x7F; line[c] = (ch < 0x20 || ch > 0x7E) ? '.' : ch; }
      line[40] = 0;
      printf("|%s|\n", line);
    }
  }

  printf("\n--- I/O reads ($C0xx), in first-seen order ---\n");
  for (int k = 0; k < ioRdSeen; k++) { int off = ioRdOrder[k]; printf("  $%04X r x%-8ld\n", off, ioRd[off - 0xC000]); }
  printf("--- I/O writes ($C0xx) ---\n");
  for (int j = 0; j < 256; j++) if (ioWr[j]) printf("  $%04X w x%-8ld last=%02X\n", 0xC000 + j, ioWr[j], ioWrLast[j]);
  return 0;
}

// minimal opcode mnemonics for the trace (common ones; others print blank)
static const char* MN(uint8_t op) {
  switch (op) {
    case 0x18: return "CLC"; case 0x38: return "SEC"; case 0x58: return "CLI"; case 0x78: return "SEI";
    case 0xD8: return "CLD"; case 0xF8: return "SED"; case 0xB8: return "CLV"; case 0xFB: return "XCE";
    case 0xC2: return "REP"; case 0xE2: return "SEP"; case 0xEA: return "NOP"; case 0xDB: return "STP";
    case 0x4C: return "JMP"; case 0x5C: return "JML"; case 0x6C: return "JMP"; case 0x20: return "JSR";
    case 0x22: return "JSL"; case 0x60: return "RTS"; case 0x6B: return "RTL"; case 0x40: return "RTI";
    case 0xA9: return "LDA"; case 0xA5: return "LDA"; case 0xAD: return "LDA"; case 0xAF: return "LDA";
    case 0xBD: return "LDA"; case 0xB9: return "LDA"; case 0xA2: return "LDX"; case 0xA0: return "LDY";
    case 0x85: return "STA"; case 0x8D: return "STA"; case 0x8F: return "STA"; case 0x9D: return "STA";
    case 0x9C: return "STZ"; case 0x64: return "STZ"; case 0x86: return "STX"; case 0x84: return "STY";
    case 0x29: return "AND"; case 0x09: return "ORA"; case 0x49: return "EOR"; case 0x69: return "ADC";
    case 0xE9: return "SBC"; case 0xC9: return "CMP"; case 0xE0: return "CPX"; case 0xC0: return "CPY";
    case 0x2C: return "BIT"; case 0x89: return "BIT"; case 0x24: return "BIT";
    case 0xF0: return "BEQ"; case 0xD0: return "BNE"; case 0x90: return "BCC"; case 0xB0: return "BCS";
    case 0x10: return "BPL"; case 0x30: return "BMI"; case 0x50: return "BVC"; case 0x70: return "BVS";
    case 0x80: return "BRA"; case 0x82: return "BRL";
    case 0xAA: return "TAX"; case 0xA8: return "TAY"; case 0x8A: return "TXA"; case 0x98: return "TYA";
    case 0x9A: return "TXS"; case 0xBA: return "TSX"; case 0x5B: return "TCD"; case 0x1B: return "TCS";
    case 0x48: return "PHA"; case 0x68: return "PLA"; case 0x08: return "PHP"; case 0x28: return "PLP";
    case 0x8B: return "PHB"; case 0xAB: return "PLB"; case 0x0B: return "PHD"; case 0x2B: return "PLD";
    case 0xE8: return "INX"; case 0xCA: return "DEX"; case 0xC8: return "INY"; case 0x88: return "DEY";
    case 0x1A: return "INC"; case 0x3A: return "DEC"; case 0xEE: return "INC"; case 0xCE: return "DEC";
    default: return "";
  }
}
