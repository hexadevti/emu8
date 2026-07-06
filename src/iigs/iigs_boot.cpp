// iigs_boot.cpp - the Apple IIGS platform core (selectable from the boot splash).
//
// Runs the 65C816 (src/iigs/cpu65816) against the embedded ROM 01 + a bankPtr memory map (banks
// $00/$01 in internal SRAM, $E0/$E1 + expansion in PSRAM, ROM $FE/$FF from the embedded flash
// array), with the ADB GLU + language card the firmware needs. iigsSetup() allocates + resets;
// iigsLoop() runs the CPU (from loop()); iigsRenderText() draws the 40-col text page to the LCD
// (called from renderLoop). Boots the IIGS firmware to its "Apple IIgs ROM Version 01" banner.
// (Disk + keyboard + SHR graphics are follow-ups; the device map mirrors the validated
// host/iigs_host.cpp.)

#include "../../emu.h"       // tft (DisplayGFX), displayFlush(), colors/datum, currentPlatform
#include "esp_heap_caps.h"
#include <string.h>
#include "cpu65816.h"
#include "iigs_rom01.h"      // extern const unsigned char* iigsRom01 (loaded from SD)

// ----- banked RAM -----
static uint8_t* bankPtr[256];                 // 64KB base per bank, or nullptr (ROM/IO/unmapped)
static uint8_t  lcB2[2][0x1000];              // language-card second $D000-$DFFF bank ($00,$01)
static uint8_t  gluMem[256];                  // ADB microcontroller / BRAM

// Apple IIGS ROM 01 (342-0077-B, 128K). Loaded from /roms/iigs/rom01.bin into a PSRAM buffer at boot
// (iigsLoadRom) - it used to be the embedded iigsRom01[] flash array. romFE/romFF index it directly.
const unsigned char* iigsRom01 = nullptr;
static bool g_romMissing = false;             // set if /roms/iigs/rom01.bin is absent -> halt + error

static inline uint8_t romFF(uint16_t off) { return iigsRom01[off]; }            // bank $FF = first 64K
static inline uint8_t romFE(uint16_t off) { return iigsRom01[0x10000 + off]; }  // bank $FE = second 64K

// Load ROM 01 from the SD card (once; cached). Must run before the 65C816 reset, which reads the
// reset vector from bank $FF. Returns false if /roms/iigs/rom01.bin is missing or the wrong size.
bool iigsLoadRom() {
  if (iigsRom01) return true;
  File f = FSTYPE.open("/roms/iigs/rom01.bin", FILE_READ);
  if (!f) { printLog("IIGS: /roms/iigs/rom01.bin missing"); return false; }
  if ((int)f.size() != 131072) { f.close(); printLog("IIGS: rom01.bin wrong size (want 128K)"); return false; }
  uint8_t* b = (uint8_t*)ps_malloc(131072);
  if (!b) { f.close(); printLog("IIGS: ROM alloc failed"); return false; }
  int rd = 0;
  while (rd < 131072) { int n = f.read(b + rd, (131072 - rd > 8192) ? 8192 : (131072 - rd)); if (n <= 0) break; rd += n; }
  f.close();
  if (rd != 131072) { free(b); printLog("IIGS: ROM read short"); return false; }
  iigsRom01 = b;
  printLog("IIGS: ROM 01 loaded from /roms/iigs/rom01.bin (128K)");
  return true;
}

// ----- ADB GLU ($C026/$C027) -----
static uint8_t gluCmd = 0; static int gluArgsLeft = 0; static uint8_t gluAddr = 0;
static uint8_t gluResp[32]; static int gluRespLen = 0, gluRespPos = 0;
static void gluPush(uint8_t b) { if (gluRespLen < 32) gluResp[gluRespLen++] = b; }
static void gluWrite(uint8_t v) {
  if (gluArgsLeft > 0) {
    if (gluCmd == 0x08) { if (gluArgsLeft == 2) gluAddr = v; else gluMem[gluAddr] = v; }
    else if (gluCmd == 0x09) { gluAddr = v; gluPush(gluMem[gluAddr]); }
    gluArgsLeft--; return;
  }
  gluCmd = v;
  switch (v) {
    case 0x07: gluArgsLeft = 4; break; case 0x08: gluArgsLeft = 2; break; case 0x09: gluArgsLeft = 1; break;
    case 0x04: case 0x05: gluArgsLeft = 1; break; case 0x06: gluArgsLeft = 3; break;
    case 0x0A: gluPush(0); break; case 0x0B: gluPush(0); gluPush(0); gluPush(0); break;
    case 0x0D: gluPush(0x06); break; case 0x0E: gluPush(1); gluPush(0); break;
    case 0x0F: gluPush(1); gluPush(0); gluPush(0); gluPush(0); break;
    default: break;
  }
}
static uint8_t gluRead() { if (gluRespPos < gluRespLen) { uint8_t b = gluResp[gluRespPos++]; if (gluRespPos >= gluRespLen) gluRespLen = gluRespPos = 0; return b; } return 0; }
static uint8_t gluStat() { return (gluRespPos < gluRespLen) ? 0x20 : 0x00; }

// ===================== Disk II / IWM (5.25", slot 6, $C0E0-$C0EF) =====================
// Ported from host/iigs_host.cpp: GCR 6&2 nibblizer + head stepping + the IWM mode register.
// The .dsk is loaded from the SD card into PSRAM by iigsLoadDisk() (from the options "mount").
static uint8_t* g_disk = nullptr;                 // 143360-byte image (35*16*256), in PSRAM
static bool     g_diskLoaded = false;
static volatile bool g_diskResetReq = false;      // set on mount -> iigsLoop resets the CPU to re-boot
static const uint8_t XLT[64] = {
  0x96,0x97,0x9A,0x9B,0x9D,0x9E,0x9F,0xA6, 0xA7,0xAB,0xAC,0xAD,0xAE,0xAF,0xB2,0xB3,
  0xB4,0xB5,0xB6,0xB7,0xB9,0xBA,0xBB,0xBC, 0xBD,0xBE,0xBF,0xCB,0xCD,0xCE,0xCF,0xD3,
  0xD6,0xD7,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE, 0xDF,0xE5,0xE6,0xE7,0xE9,0xEA,0xEB,0xEC,
  0xED,0xEE,0xEF,0xF2,0xF3,0xF4,0xF5,0xF6, 0xF7,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF };
static const uint8_t DOTRK[16] = {0x0,0x7,0xe,0x6,0xd,0x5,0xc,0x4,0xb,0x3,0xa,0x2,0x9,0x1,0x8,0xf};
static void enc44(uint8_t d, uint8_t* o) { o[0] = (d >> 1) | 0xAA; o[1] = d | 0xAA; }
static void enc62(const uint8_t* in, uint8_t* agg) {
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
    enc[p++] = 0xd5; enc[p++] = 0xaa; enc[p++] = 0x96;
    enc44(vol, b);   enc[p++] = b[0]; enc[p++] = b[1];
    enc44(track, b); enc[p++] = b[0]; enc[p++] = b[1];
    enc44(isec, b);  enc[p++] = b[0]; enc[p++] = b[1];
    enc44(vol ^ (uint8_t)track ^ isec, b); enc[p++] = b[0]; enc[p++] = b[1];
    enc[p++] = 0xde; enc[p++] = 0xaa; enc[p++] = 0xeb;
    enc[p++] = 0xd5; enc[p++] = 0xaa; enc[p++] = 0xad;
    uint8_t agg[343]; enc62(trk + (long)DOTRK[isec] * 256, agg);
    for (int i = 0; i < 343; i++) enc[p++] = agg[i];
    enc[p++] = 0xde; enc[p++] = 0xaa; enc[p++] = 0xeb;
  }
  return p;
}
static int g_track = 0; static uint8_t g_ring[4] = {0,0,0,0};
static bool seqeq(const uint8_t* a, const uint8_t* b) { return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2]&&a[3]==b[3]; }
static void iigsAddPhase(uint8_t ph) {
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
    case 0xC0E0: iigsAddPhase(0x00); break; case 0xC0E1: iigsAddPhase(0x01); break;
    case 0xC0E2: iigsAddPhase(0x10); break; case 0xC0E3: iigsAddPhase(0x11); break;
    case 0xC0E4: iigsAddPhase(0x20); break; case 0xC0E5: iigsAddPhase(0x21); break;
    case 0xC0E6: iigsAddPhase(0x30); break; case 0xC0E7: iigsAddPhase(0x31); break;
    case 0xC0E8: g_motor = false; break; case 0xC0E9: g_motor = true; break;
    case 0xC0EA: case 0xC0EB: break;
    case 0xC0EC: g_q6h = false; break;
    case 0xC0ED: g_q6h = true;  break;
    case 0xC0EE: g_q7h = false; break;
    case 0xC0EF: g_q7h = true; if (write && !g_motor) g_iwmMode = val & 0x1F; break;
  }
  if (!write) {
    if (off == 0xC0EC && !g_q7h) {                  // stream a GCR nibble in read mode
      if (!g_diskLoaded) return 0;
      if (g_curTrk != g_track) { g_encLen = nibblizeTrack(g_track, g_enc); g_curTrk = g_track; g_nib = 0; }
      if (g_nib >= g_encLen) g_nib = 0;
      return g_enc[g_nib++];
    }
    if (off == 0xC0EE) return g_motor ? 0x00 : g_iwmMode;   // motor off: IWM status=mode
  }
  return 0;
}

// Load a .dsk from the SD card into PSRAM and request a CPU reset so the firmware boots it.
void iigsLoadDisk(const char* path) {
  if (!path || !path[0]) return;
  if (!g_disk) g_disk = (uint8_t*)ps_malloc(160 * 1024);
  if (!g_disk) { Serial.println("IIGS disk: PSRAM alloc fail"); return; }
  busTake();
  File f = FSTYPE.open(path, FILE_READ);
  size_t n = 0;
  if (f) { n = f.read(g_disk, 160 * 1024); f.close(); }
  busGive();
  g_diskLoaded = (n >= 143360);
  g_curTrk = -1; g_track = 0; g_nib = 0;
  Serial.printf("IIGS disk: %u bytes from %s (%s)\n", (unsigned)n, path, g_diskLoaded ? "ok" : "FAIL");
  if (g_diskLoaded) g_diskResetReq = true;          // reboot the firmware to boot from the disk
}

// ====== ProDOS block device / HD (slot 7): hdrom@$C700 + $C0F0-$C0F7 ======
// Same approach as the Apple IIe HD (validated on the host): the slot-7 firmware `hdrom` (from rom.h)
// is mapped at $C700; the boot scanner / ProDOS calls it; it pokes params to $C0F2-$C0F7 and reads
// $C0F0 to do a 512-byte block read/write straight from the .po/.2mg/.hdv image (header stripped).
static uint8_t* g_hd = nullptr; static long g_hdLen = 0; static int g_hdHdr = 0;
static uint8_t hdCmd = 0, hdUnit = 0, hdStatus = 0;
static uint16_t hdBufAddr = 0, hdBlock = 0;
static inline uint16_t hdBlockCount() { return g_hd ? (uint16_t)((g_hdLen - g_hdHdr) / 512) : 0; }
static uint8_t hdExec() {
  if (!g_hd || !bankPtr[0]) return 0x01;
  long pos = (long)hdBlock * 512 + g_hdHdr;
  if (pos < 0 || pos + 512 > g_hdLen) return 0x01;
  if (hdCmd == 1)      for (int k = 0; k < 512; k++) bankPtr[0][(uint16_t)(hdBufAddr + k)] = g_hd[pos + k];  // READ
  else if (hdCmd == 2) for (int k = 0; k < 512; k++) g_hd[pos + k] = bankPtr[0][(uint16_t)(hdBufAddr + k)];  // WRITE
  return 0;
}
static uint8_t hdRead(uint16_t off) {
  switch (off) {
    case 0xC0F0: hdStatus = hdExec(); return hdStatus;
    case 0xC0F1: return hdStatus;
    case 0xC0F2: return hdCmd;            case 0xC0F3: return hdUnit;
    case 0xC0F4: return hdBufAddr & 0xFF; case 0xC0F5: return hdBufAddr >> 8;
    case 0xC0F6: return hdBlock & 0xFF;   case 0xC0F7: return hdBlock >> 8;
    case 0xC0F9: return hdBlockCount() & 0xFF; case 0xC0FA: return hdBlockCount() >> 8;
  }
  return 0;
}
static void hdWrite(uint16_t off, uint8_t v) {
  switch (off) {
    case 0xC0F2: hdCmd = v; break;        case 0xC0F3: hdUnit = v; break;
    case 0xC0F4: hdBufAddr = (hdBufAddr & 0xFF00) | v; break;
    case 0xC0F5: hdBufAddr = (hdBufAddr & 0x00FF) | (v << 8); break;
    case 0xC0F6: hdBlock = (hdBlock & 0xFF00) | v; break;
    case 0xC0F7: hdBlock = (hdBlock & 0x00FF) | (v << 8); break;
  }
}
// Load a ProDOS block image (.po/.2mg/.hdv) from SD into PSRAM + reboot to boot it from slot 7.
void iigsLoadHD(const char* path) {
  if (!path || !path[0]) return;
  busTake();
  apple2EnsureHdRom();                                // slot-7 HD firmware ($C700) from /roms/apple2 (shared w/ Apple II)
  File f = FSTYPE.open(path, FILE_READ);
  long sz = f ? (long)f.size() : 0;
  long n = 0;
  if (sz > 0) {
    if (g_hd) { free(g_hd); g_hd = nullptr; }
    g_hd = (uint8_t*)ps_malloc(sz);
    if (g_hd) { while (n < sz) { int r = f.read(g_hd + n, sz - n); if (r <= 0) break; n += r; } }
  }
  if (f) f.close();
  busGive();
  g_hdLen = (g_hd ? n : 0);
  g_hdHdr = 0;                                        // .po/.hdv: raw ProDOS blocks, no header
  if (g_hdLen >= 64 && g_hd[0]=='2'&&g_hd[1]=='I'&&g_hd[2]=='M'&&g_hd[3]=='G') {   // .2mg: parse the header
    uint32_t off = g_hd[0x18] | (g_hd[0x19]<<8) | (g_hd[0x1A]<<16) | ((uint32_t)g_hd[0x1B]<<24);
    g_hdHdr = (off >= 64 && off < (uint32_t)g_hdLen) ? (int)off : 64;   // dataOffset, NOT len%512 (trailing chunks)
  }
  Serial.printf("IIGS HD: %ld bytes (%d hdr, %u blocks) from %s\n", g_hdLen, g_hdHdr, hdBlockCount(), path);
  if (g_hd && g_hdLen > 0) g_diskResetReq = true;     // reboot -> scan boots slot 7
}

static long gInstr = 0;

// Apple II video soft switches: toggled by ANY access (read or write). The render dispatch
// (iigsRenderText) reads these to pick 40-col text vs hi-res vs double-hi-res, page 1 vs 2.
static bool vidText = true, vidMixed = false, vidPage2 = false, vidHires = false;
static bool vidDHires = false, vid80Col = false;   // DHGR = hires + dhires ($C05E) + 80col ($C00D)
static uint8_t newVideoReg = 0;                    // $C029 New Video reg; bit7 = Super Hi-Res on
// Apple IIe auxiliary-memory soft switches: route bank-$00 RAM accesses to bank $01 (aux). Needed so
// DHGR software (and 80-col) can stage half its data in aux. Defaults off -> bank $00 unchanged.
static bool sw80Store = false, swRamRd = false, swRamWrt = false;
// True when an access to bank-$00 offset `off` should be redirected to aux (bank $01).
static inline bool auxBankFor(uint16_t off, bool isWrite) {
  if (off < 0x0200 || off >= 0xC000) return false;                 // ZP/stack stay main; $C000+ elsewhere
  if (sw80Store) {                                                 // 80STORE: PAGE2 owns the display pages
    if (off >= 0x0400 && off <= 0x07FF) return vidPage2;           // text page 1
    if (vidHires && off >= 0x2000 && off <= 0x3FFF) return vidPage2; // hi-res page 1
  }
  return isWrite ? swRamWrt : swRamRd;                             // else RAMRD/RAMWRT own $0200-$BFFF
}
// IIGS shadowing: copy bank $00/$01 writes to $E0/$E1 (the Mega II/VGC side the video hardware reads).
// $C035 = inhibit register (bit=1 -> that region does NOT shadow). Combined with the aux redirect,
// SHR software writing $01:2000-9FFF reaches $E1:2000 (where renderSHR reads) -> graphics appear.
static uint8_t shadowReg = 0x00;
static inline void shadowWrite(uint8_t bank, uint16_t off, uint8_t v) {
  if (bank == 0x00) {
    bool sh = (off >= 0x0400 && off <= 0x07FF) ? !(shadowReg & 0x01)     // text page 1
            : (off >= 0x2000 && off <= 0x3FFF) ? !(shadowReg & 0x02)     // hi-res page 1
            : (off >= 0x4000 && off <= 0x5FFF) ? !(shadowReg & 0x04)     // hi-res page 2
            : false;
    if (sh && bankPtr[0xE0]) bankPtr[0xE0][off] = v;
  } else if (bank == 0x01) {
    bool sh = (off >= 0x0400 && off <= 0x07FF) ? !(shadowReg & 0x01)     // text page 1 (aux)
            : (off >= 0x2000 && off <= 0x9FFF) ? !(shadowReg & 0x08)     // super hi-res (+ aux hi-res)
            : false;
    if (sh && bankPtr[0xE1]) bankPtr[0xE1][off] = v;
  }
}
// IIGS clock/border ($C033 data, $C034 control): minimal stub so the runtime clock polls don't hang.
static uint8_t clkData = 0, clkCtl = 0;
static void vidSwitch(uint16_t off) {
  switch (off) {
    case 0xC050: vidText  = false; break;   // GRAPHICS on
    case 0xC051: vidText  = true;  break;   // TEXT on
    case 0xC052: vidMixed = false; break;   // full-screen graphics
    case 0xC053: vidMixed = true;  break;   // mixed (4 text rows at the bottom)
    case 0xC054: vidPage2 = false; break;   // display page 1
    case 0xC055: vidPage2 = true;  break;   // display page 2
    case 0xC056: vidHires = false; break;   // lo-res
    case 0xC057: vidHires = true;  break;   // hi-res
    case 0xC05E: vidDHires = true; break;   // double hi-res on (AN3 off)
    case 0xC05F: vidDHires = false; break;  // double hi-res off
  }
}

static uint8_t ioRead(uint16_t off) {
  if (off >= 0xC0E0 && off <= 0xC0EF) return diskIO(off, false, 0);   // Disk II / IWM (slot 6)
  if (off >= 0xC0F0 && off <= 0xC0FA) return hdRead(off);             // ProDOS block device (slot 7)
  if ((off >= 0xC050 && off <= 0xC057) || off == 0xC05E || off == 0xC05F) { vidSwitch(off); return 0x00; }
  switch (off) {
    case 0xC000: return (uint8_t)keymem;                      // keyboard data (bit7 = key available)
    case 0xC010: keyboardStrobe(); return 0x00;               // any-key-down / clear strobe
    case 0xC019: return ((gInstr / 600) & 1) ? 0x80 : 0x00;   // fake VBL
    case 0xC01A: return vidText  ? 0x80 : 0x00;               // RDTEXT
    case 0xC01B: return vidMixed ? 0x80 : 0x00;               // RDMIXED
    case 0xC01C: return vidPage2 ? 0x80 : 0x00;               // RDPAGE2
    case 0xC01D: return vidHires ? 0x80 : 0x00;               // RDHIRES
    case 0xC029: return newVideoReg;                          // New Video reg (bit7 = Super Hi-Res)
    case 0xC030: speakerToggle(); return 0x00;               // 1-bit speaker click
    case 0xC033: return clkData;                              // clock data
    case 0xC034: return clkCtl & 0x7F;                        // clock control: bit7=0 -> never busy
    case 0xC035: return shadowReg;                            // shadow register
    case 0xC026: return gluRead();
    case 0xC027: return gluStat();
    default:     return 0x00;
  }
}
static void ioWrite(uint16_t off, uint8_t v) {
  if (off >= 0xC0E0 && off <= 0xC0EF) { diskIO(off, true, v); return; }   // Disk II / IWM (slot 6)
  if (off >= 0xC0F0 && off <= 0xC0FA) { hdWrite(off, v); return; }        // ProDOS block device (slot 7)
  if ((off >= 0xC050 && off <= 0xC057) || off == 0xC05E || off == 0xC05F) { vidSwitch(off); return; }
  switch (off) {                                              // aux-memory soft switches (write-only)
    case 0xC000: sw80Store = false; return;
    case 0xC001: sw80Store = true;  return;
    case 0xC002: swRamRd  = false; return;                    // read main 48K
    case 0xC003: swRamRd  = true;  return;                    // read aux 48K
    case 0xC004: swRamWrt = false; return;                    // write main 48K
    case 0xC005: swRamWrt = true;  return;                    // write aux 48K
  }
  if (off == 0xC00C) { vid80Col = false; return; }            // 80-column off
  if (off == 0xC00D) { vid80Col = true;  return; }            // 80-column on (needed for DHGR)
  if (off == 0xC029) { newVideoReg = v;  return; }            // New Video reg (bit7 = Super Hi-Res)
  if (off == 0xC030) { speakerToggle(); return; }             // 1-bit speaker click
  if (off == 0xC033) { clkData = v; return; }                 // clock data
  if (off == 0xC034) { clkCtl  = v; return; }                 // clock control (+ border color)
  if (off == 0xC035) { shadowReg = v; return; }               // shadow register
  if (off == 0xC010) { keyboardStrobe(); return; }            // clear keyboard strobe
  if (off == 0xC026) gluWrite(v);
}

// ----- language card (banks $00/$01) -----
static bool lcRd[2] = {false, false}, lcWr[2] = {false, false}, lcB2sel[2] = {false, false};
static int  lcPre[2] = {0, 0};
static void lcSwitch(int b, uint16_t a, bool isRd) {
  lcB2sel[b] = !(a & 8); int m = a & 3; lcRd[b] = (m == 0 || m == 3);
  if (m == 1 || m == 3) { if (isRd) { if (++lcPre[b] >= 2) lcWr[b] = true; } else lcPre[b] = 0; }
  else { lcWr[b] = false; lcPre[b] = 0; }
}
static uint8_t lcReadRAM(int b, uint16_t off) { if (off < 0xE000 && lcB2sel[b]) return lcB2[b][off - 0xD000]; return bankPtr[b][off]; }
static void    lcWriteRAM(int b, uint16_t off, uint8_t v) { if (off < 0xE000 && lcB2sel[b]) lcB2[b][off - 0xD000] = v; else bankPtr[b][off] = v; }

// ----- 24-bit bus -----
static uint8_t iigsRd(uint32_t a) {
  uint8_t bank = a >> 16; uint16_t off = (uint16_t)a;
  if (bank == 0xFF) return romFF(off);
  if (bank == 0xFE) return romFE(off);
  if (bank == 0x00 || bank == 0x01) {
    if (off >= 0xC080 && off <= 0xC08F) { lcSwitch(bank, off, true); return 0; }
    if (off >= 0xC000 && off <= 0xC0FF) return ioRead(off);
    if (off >= 0xD000) return lcRd[bank] ? lcReadRAM(bank, off) : romFF(off);
    if (off >= 0xC700 && off <= 0xC7FF && g_hd && hdrom) return hdrom[off - 0xC700];   // slot 7 = our HD firmware (from /roms/apple2)
    if (off >= 0xC100) return romFF(off);
    { uint8_t rb = (bank == 0x00 && auxBankFor(off, false)) ? 0x01 : bank;   // aux-memory redirect
      return bankPtr[rb] ? bankPtr[rb][off] : 0; }
  }
  if ((bank == 0xE0 || bank == 0xE1) && off >= 0xC000 && off <= 0xC0FF) return ioRead(off);
  return bankPtr[bank] ? bankPtr[bank][off] : 0;
}
static void iigsWr(uint32_t a, uint8_t v) {
  uint8_t bank = a >> 16; uint16_t off = (uint16_t)a;
  if (bank == 0xFE || bank == 0xFF) return;
  if (bank == 0x00 || bank == 0x01) {
    if (off >= 0xC080 && off <= 0xC08F) { lcSwitch(bank, off, false); return; }
    if (off >= 0xC000 && off <= 0xC0FF) { ioWrite(off, v); return; }
    if (off >= 0xD000) { if (lcWr[bank]) lcWriteRAM(bank, off, v); return; }
    { uint8_t wb = (bank == 0x00 && auxBankFor(off, true)) ? 0x01 : bank;     // aux-memory redirect
      if (bankPtr[wb]) bankPtr[wb][off] = v;
      if (off >= 0x0400 && off <= 0x9FFF) shadowWrite(wb, off, v);            // mirror to $E0/$E1 for video
      return; }
  }
  if ((bank == 0xE0 || bank == 0xE1) && off >= 0xC000 && off <= 0xC0FF) { ioWrite(off, v); return; }
  if (bankPtr[bank]) bankPtr[bank][off] = v;
}

// ----- platform state -----
static CPU65816 g_cpu;
static bool     g_ready = false;

// Apple II hi-res page ($00:2000 page 1 / $00:4000 page 2) -> RGB565, pushed centered+scaled.
// The interleaved line layout matches real Apple II hi-res. Color = the classic NTSC artifact rule.
#define HGR_VIOLET 0xD81F
#define HGR_GREEN  0x3FE6
#define HGR_BLUE   0x051F
#define HGR_ORANGE 0xFB60
static uint16_t* hgrBuf = nullptr;                 // up to 280x192 RGB565 scratch, lazily PSRAM-alloc'd
// availH = logical height the image may use: 240 = full panel (centered); less = compacted above the
// on-screen keyboard (downsampled). Honors the VIDEO COLOR/MONO toggle.
static void renderHiresPage(int availH, bool dhgr) {
  if (!hgrBuf) { hgrBuf = (uint16_t*)ps_malloc(280 * 192 * 2); if (!hgrBuf) return; }
  uint32_t pageOff = vidPage2 ? 0x4000 : 0x2000;
  const uint8_t* mainp = bankPtr[0] + pageOff;       // main RAM (bank $00)
  const uint8_t* auxp  = (dhgr && bankPtr[1]) ? bankPtr[1] + pageOff : nullptr;   // aux RAM (bank $01)
  int destRows = (availH >= 240) ? 192 : availH;     // compact: squeeze 192 source rows into availH
  if (destRows < 1) destRows = 1; else if (destRows > 192) destRows = 192;
  for (int dy = 0; dy < destRows; dy++) {
    int y = (destRows == 192) ? dy : (dy * 192 / destRows);
    uint32_t off = (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80 + (y >> 6) * 0x28;
    uint16_t* out = hgrBuf + dy * 280;
    if (dhgr && auxp) {
      // 80 interleaved bytes -> 560 bits: aux[col] 7 bits (LSB-first) then main[col] 7 bits.
      uint8_t bits[560]; int bi = 0;
      for (int col = 0; col < 40; col++) {
        uint8_t a = auxp[off + col];  for (int k = 0; k < 7; k++) bits[bi++] = (a >> k) & 1;
        uint8_t m = mainp[off + col]; for (int k = 0; k < 7; k++) bits[bi++] = (m >> k) & 1;
      }
      if (videoColor) {                                // 140 color px (4 bits each), each doubled -> 280
        for (int i = 0, x = 0; i < 560; i += 4, x += 2) {
          int idx = (bits[i] ? 8 : 0) | (bits[i + 1] ? 4 : 0) | (bits[i + 2] ? 2 : 0) | (bits[i + 3] ? 1 : 0);
          out[x] = out[x + 1] = colors16[idx];
        }
      } else {                                         // 560 mono bits downsampled 2:1 -> 280
        for (int i = 0, x = 0; i < 560; i += 2, x++) out[x] = bits[i] ? 0xFFFF : 0x0000;
      }
    } else if (videoColor) {                           // single hi-res, NTSC artifact color
      const uint8_t* line = mainp + off;
      uint8_t bits[281], pal[280];
      for (int col = 0; col < 40; col++) {
        uint8_t b = line[col], p = (b >> 7) & 1;
        for (int k = 0; k < 7; k++) { int x = col * 7 + k; bits[x] = (b >> k) & 1; pal[x] = p; }
      }
      bits[280] = 0;
      for (int x = 0; x < 280; x++) {
        if (!bits[x]) out[x] = 0x0000;
        else if ((x > 0 && bits[x - 1]) || bits[x + 1]) out[x] = 0xFFFF;   // two adjacent on -> white
        else out[x] = (x & 1) ? (pal[x] ? HGR_ORANGE : HGR_GREEN)
                              : (pal[x] ? HGR_BLUE   : HGR_VIOLET);
      }
    } else {                                           // single hi-res, mono
      const uint8_t* line = mainp + off;
      uint16_t* o = out;
      for (int col = 0; col < 40; col++) { uint8_t b = line[col]; for (int k = 0; k < 7; k++) *o++ = ((b >> k) & 1) ? 0xFFFF : 0x0000; }
    }
  }
  int x0   = (320 - 280) / 2;                        // 20: center horizontally
  int yTop = (destRows == 192) ? (240 - 192) / 2 : 0;// center full-screen; top-align when compacted
  tft.pushImage(x0, yTop, 280, destRows, hgrBuf);
}

// ----- IIGS Super Hi-Res (VGC): 320x200 (16 col/line) or 640x200, all in bank $E1 -----
//   $E1:2000  framebuffer (200 lines x 160 bytes)
//   $E1:9D00  SCBs (1/line: bit7=640mode, bit5=fill[320], bits3-0=palette index)
//   $E1:9E00  16 palettes x 16 colors x 2 bytes ($0RGB 12-bit)
static uint16_t* shrBuf = nullptr;                   // 320x200 RGB565 scratch (PSRAM)
static inline uint16_t shr565(uint16_t w) {          // $0RGB 12-bit -> RGB565
  uint8_t r = (w >> 8) & 0xF, g = (w >> 4) & 0xF, b = w & 0xF;
  uint8_t r8 = (r << 4) | r, g8 = (g << 4) | g, b8 = (b << 4) | b;
  return (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
}
static void renderSHR(int availH) {
  if (!shrBuf) { shrBuf = (uint16_t*)ps_malloc(320 * 200 * 2); if (!shrBuf) return; }
  const uint8_t* e1 = bankPtr[0xE1];
  if (!e1) return;
  const uint8_t* fb   = e1 + 0x2000;
  const uint8_t* scbs = e1 + 0x9D00;
  const uint8_t* pals = e1 + 0x9E00;
  int destRows = (availH >= 240) ? 200 : (availH > 200 ? 200 : availH);
  if (destRows < 1) destRows = 1;
  for (int dy = 0; dy < destRows; dy++) {
    int ln = (destRows == 200) ? dy : (dy * 200 / destRows);
    uint8_t scb = scbs[ln];
    bool mode640 = scb & 0x80;
    bool fill    = (scb & 0x20) && !mode640;
    const uint8_t* pal = pals + (scb & 0x0F) * 32;
    uint16_t pal565[16];
    for (int c = 0; c < 16; c++) pal565[c] = shr565((uint16_t)(pal[c * 2] | (pal[c * 2 + 1] << 8)));
    const uint8_t* src = fb + ln * 160;
    uint16_t* out = shrBuf + dy * 320;
    if (!mode640) {                                  // 320: 2 px/byte (hi nibble = left)
      uint16_t last = pal565[0];
      for (int col = 0; col < 160; col++) {
        uint8_t b = src[col];
        uint16_t cH = (fill && (b >> 4) == 0) ? last : pal565[b >> 4];   last = cH;
        uint16_t cL = (fill && (b & 0xF) == 0) ? last : pal565[b & 0xF]; last = cL;
        out[col * 2] = cH; out[col * 2 + 1] = cL;
      }
    } else {                                         // 640: 4 px/byte (2 bits), per-position palette group; 2:1 downsample
      for (int col = 0; col < 160; col++) {
        uint8_t b = src[col];
        out[col * 2]     = pal565[((b >> 6) & 3) + 8];   // x%4==0 -> colors 8-11
        out[col * 2 + 1] = pal565[((b >> 2) & 3) + 0];   // x%4==2 -> colors 0-3
      }
    }
  }
  int yTop = (destRows == 200) ? 20 : 0;             // 320px fills the panel width; center vertically
  tft.pushImage(0, yTop, 320, destRows, shrBuf);
}

// Apple II lo-res ($C050 GR + $C056 LORES): 40x48 blocks of 16 colors from the text page ($0400/$0800),
// each text byte = 2 vertically-stacked blocks (low nibble = top, high nibble = bottom). 7x4 px/block.
static void renderLoRes(int availH) {
  if (!hgrBuf) { hgrBuf = (uint16_t*)ps_malloc(280 * 192 * 2); if (!hgrBuf) return; }
  const uint8_t* tp = bankPtr[0] + (vidPage2 ? 0x0800 : 0x0400);
  int destRows = (availH >= 240) ? 192 : availH;
  if (destRows < 1) destRows = 1; else if (destRows > 192) destRows = 192;
  for (int dy = 0; dy < destRows; dy++) {
    int y  = (destRows == 192) ? dy : (dy * 192 / destRows);   // source scanline 0..191
    int by = y / 4;                                            // block row 0..47
    uint32_t ro = ((by / 2) % 8) * 0x80 + ((by / 2) / 8) * 0x28;
    uint16_t* out = hgrBuf + dy * 280;
    for (int col = 0; col < 40; col++) {
      uint8_t b = tp[ro + col];
      uint16_t color = colors16[(by & 1) ? (b >> 4) : (b & 0x0F)];
      for (int xx = 0; xx < 7; xx++) out[col * 7 + xx] = color;
    }
  }
  int yTop = (destRows == 192) ? 24 : 0;
  tft.pushImage(20, yTop, 280, destRows, hgrBuf);
}

// Render the IIGS 40x24 text page ($00:0400) to the LCD (built-in 6x8 font; renderLoop flushes).
// Called from renderLoop (core 0) while iigsLoop (core 1) runs the CPU - shared SRAM, read-only here.
void iigsRenderText() {
  tft.setUiMode(true);
  bool osk = oskActive();
  int kbdTop = osk ? oskRasterHeight() : 240;      // logical y where the on-screen keyboard starts
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (!g_ready || !bankPtr[0]) {                   // setup failed (e.g. out of memory): say so
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(g_romMissing ? "IIGS: put rom01.bin in /roms/iigs" : "IIGS init failed", 160, 60, 2);
    return;
  }
  // IIGS Super Hi-Res ($C029 bit7): the VGC overrides the Apple II video entirely.
  if (newVideoReg & 0x80) {
    if (osk) tft.fillRect(0, 0, 320, kbdTop, TFT_BLACK);
    else     tft.fillScreen(TFT_BLACK);
    renderSHR(kbdTop);
    return;
  }
  // Apple II graphics: hi-res / double-hi-res, or lo-res. When the keyboard is up, clear ONLY above it
  // and compact the image there (a full fillScreen wiped the OSK -> it "closed").
  if (!vidText) {
    if (osk) tft.fillRect(0, 0, 320, kbdTop, TFT_BLACK);
    else     tft.fillScreen(TFT_BLACK);
    if (vidHires) renderHiresPage(kbdTop, vidDHires && vid80Col);   // hi-res / DHGR (dhires + 80col)
    else          renderLoRes(kbdTop);                              // 40x48 lo-res blocks
    if (vidMixed && !osk) {                                         // MIXED: the bottom 4 rows are text
      const uint8_t* tp = bankPtr[0] + 0x400;
      tft.fillRect(0, 184, 320, 32, TFT_BLACK);                     // graphics maps logical y 24..216; rows 20-23
      tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextDatum(TL_DATUM);
      for (int r = 20; r < 24; r++) {
        char line[41]; uint32_t ro = (r % 8) * 0x80 + (r / 8) * 0x28;
        for (int c = 0; c < 40; c++) { uint8_t ch = tp[ro + c] & 0x7F; line[c] = (ch < 0x20 || ch > 0x7E) ? ' ' : ch; }
        line[40] = 0;
        tft.drawString(line, 80, 24 + r * 8, 1);
      }
    }
    return;
  }
  // Text page: 80-column (main + aux interleaved) or 40-column. Clear ONLY above the keyboard (the OSK
  // repaints only when dirty, so a full fillScreen every frame wiped it -> flicker).
  if (osk) tft.fillRect(0, 0, 320, kbdTop, TFT_BLACK);
  else     tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  const uint8_t* mainT = bankPtr[0] + 0x400;                        // main text page
  const uint8_t* auxT  = bankPtr[1] ? bankPtr[1] + 0x400 : mainT;   // aux holds the even 80-col columns
  // Built-in font is 6x8 PHYSICAL px: 40 cols = 240px (center at logical x=80); 80 cols = 480px (full width).
  bool col80 = vid80Col;
  int   X0   = col80 ? 0 : (320 - 160) / 2;
  float rowH = (kbdTop - 8.0f) / 23.0f;            // 10 full screen; ~4.5 compacted above the keyboard
  for (int r = 0; r < 24; r++) {
    uint32_t ro = (r % 8) * 0x80 + (r / 8) * 0x28;
    char line[81];
    if (col80) {
      for (int c = 0; c < 80; c++) { const uint8_t* s = (c & 1) ? mainT : auxT; uint8_t ch = s[ro + c / 2] & 0x7F; line[c] = (ch < 0x20 || ch > 0x7E) ? ' ' : ch; }
      line[80] = 0;
    } else {
      for (int c = 0; c < 40; c++) { uint8_t ch = mainT[ro + c] & 0x7F; line[c] = (ch < 0x20 || ch > 0x7E) ? ' ' : ch; }
      line[40] = 0;
    }
    tft.drawString(line, X0, (int)(r * rowH + 0.5f), 1);
  }
}

void iigsSetup() {
  printLog("IIGS Setup (ROM 01)...");
  if (!iigsLoadRom()) { g_romMissing = true; g_ready = false; return; }  // ROM must be on SD before reset
  // M0.5 locality: the HOTTEST bank $00 (zero page, stack, low RAM, language card) goes in INTERNAL
  // SRAM (fast); $01 (aux) + $E0/$E1 (Mega II/video) + the $02.. expansion stay in PSRAM. Internal
  // SRAM is fragmented by the platform statics, so $00 falls back to PSRAM if its 64K alloc fails.
  const int EXP = 0x20;
  const int NPS = 3 /*$01,$E0,$E1*/ + EXP;
  uint8_t* ps = (uint8_t*)ps_malloc((size_t)NPS * 0x10000);
  if (!ps) { Serial.println("IIGS PSRAM ALLOC FAIL"); g_ready = false; return; }
  memset(ps, 0, (size_t)NPS * 0x10000);
  for (int b = 0; b < 256; b++) bankPtr[b] = nullptr;
  bankPtr[0x01] = ps;
  bankPtr[0xE0] = ps + 1 * 0x10000;
  bankPtr[0xE1] = ps + 2 * 0x10000;
  for (int b = 0; b < EXP; b++) bankPtr[0x02 + b] = ps + (size_t)(3 + b) * 0x10000;
  uint8_t* s0 = (uint8_t*)heap_caps_malloc(0x10000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  bool b0Internal = (s0 != nullptr);
  if (!s0) s0 = (uint8_t*)ps_malloc(0x10000);
  if (!s0) { Serial.println("IIGS bank $00 ALLOC FAIL"); g_ready = false; return; }
  memset(s0, 0, 0x10000);
  bankPtr[0x00] = s0;
  memset(&g_cpu, 0, sizeof(g_cpu));
  g_cpu.rd = iigsRd; g_cpu.wr = iigsWr; g_cpu.reset();
  g_ready = true;
  Serial.printf("IIGS reset PC=%04X  bank $00=%s  spiram free=%u  internal free=%u\n", g_cpu.PC,
                b0Internal ? "SRAM" : "PSRAM",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void iigsLoop() {
  if (g_romMissing) { delay(500); return; }                              // no ROM on SD -> halt (render shows it)
  if (!g_ready) { iigsSetup(); if (!g_ready) { delay(500); return; } }   // don't step a null-bus CPU
  uint64_t lastCyc = g_cpu.cycles; uint32_t lastUs = micros();
  for (;;) {
    if (g_diskResetReq) {                          // a disk was just mounted -> reboot to boot it
      g_diskResetReq = false; g_cpu.reset(); gInstr = 0;
      lastCyc = g_cpu.cycles; lastUs = micros();
    }
    // Speed regulator: SPEED=1MHz throttles in TINY batches (16 instr) with a sub-ms BUSY-WAIT (no
    // vTaskDelay). The CPU pauses for only ~40us between batches, so the speaker_state "freeze"
    // artifact lands at ~20kHz - ABOVE the speaker's 5kHz low-pass, so it's filtered out (a coarse
    // vTaskDelay froze it for whole ms -> ~140Hz gaps below the low-pass -> the "horrible" sound).
    // The core counts a flat 2 cycles/instr; the real 6502 averages ~3, so scale the budget x1.5 for
    // a true ~1.02MHz instruction rate. FAST just runs the interpreter flat out.
    int batch = Fast1MhzSpeed ? 200000 : 16;
    for (int k = 0; k < batch && !g_cpu.stopped; k++) { g_cpu.step(); gInstr++; }
    if (!Fast1MhzSpeed) {
      uint64_t cyc = g_cpu.cycles;
      uint32_t budgetUs  = (uint32_t)((cyc - lastCyc) * 3 / 2);
      uint32_t elapsedUs = micros() - lastUs;
      if (budgetUs > elapsedUs) {
        uint32_t waitUs = budgetUs - elapsedUs;
        if (waitUs > 500) waitUs = 500;                 // safety cap; a normal 16-instr batch waits ~40us
        delayMicroseconds(waitUs);
      }
      lastCyc = cyc; lastUs = micros();
    }
    if (paused) { while (paused) delay(50); lastCyc = g_cpu.cycles; lastUs = micros(); }
  }
}
