// emu.h must be included FIRST because it is what pulls in board.h and defines the macro.
#include "../../emu.h"
#if BOARD_HAS_ZX_CORE

// zx_tape.cpp - index .TAP / .TZX images into the flat block list the LD-BYTES trap reads
// (zx_machine.cpp). Nothing is copied: a block is an (offset, length) pair into the resident image.
// TZX data blocks 0x10 (standard), 0x11 (turbo) and 0x14 (pure data) carry ordinary flag+data+checksum
// payloads and are indexed; the other TZX blocks only describe timing, pauses or metadata and are
// skipped. A custom loader that bypasses LD-BYTES will not load from either format.

#include "zx.h"
#include <string.h>

namespace zx {

static inline uint32_t le16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static inline uint32_t le24(const uint8_t* p) { return le16(p) | ((uint32_t)p[2] << 16); }
static inline uint32_t le32(const uint8_t* p) { return le24(p) | ((uint32_t)p[3] << 24); }

static void addBlock(uint32_t off, uint32_t len) {
  if (tapeCount < tapeMaxBlocks && len > 0) { tapeBlocks[tapeCount].off = off; tapeBlocks[tapeCount].len = len; tapeCount++; }
}

int tapeIndexTap(const uint8_t* d, int len) {
  tapeCount = 0; tapePos = 0;
  uint32_t p = 0;
  while (p + 2 <= (uint32_t)len) {
    uint32_t n = le16(d + p);
    if (p + 2 + n > (uint32_t)len) break;
    addBlock(p + 2, n);
    p += 2 + n;
  }
  return tapeCount;
}

int tapeIndexTzx(const uint8_t* d, int len) {
  tapeCount = 0; tapePos = 0;
  if (len < 10 || memcmp(d, "ZXTape!\x1A", 8) != 0) return 0;
  uint32_t p = 10, L = (uint32_t)len;
  while (p < L) {
    const uint8_t id = d[p++];
    const uint8_t* b = d + p;
    uint32_t skip, n;
    if (p + 2 > L && id != 0x22 && id != 0x25 && id != 0x27) break;   // truncated image
    switch (id) {
      case 0x10: if (p + 4 > L) return tapeCount; n = le16(b + 2);  addBlock(p + 4, n);  skip = 4 + n;  break;
      case 0x11: if (p + 18 > L) return tapeCount; n = le24(b + 15); addBlock(p + 18, n); skip = 18 + n; break;
      case 0x14: if (p + 10 > L) return tapeCount; n = le24(b + 7);  addBlock(p + 10, n); skip = 10 + n; break;
      case 0x12: skip = 4; break;
      case 0x13: skip = 1 + 2 * (uint32_t)b[0]; break;
      case 0x15: skip = 8 + le24(b + 5); break;
      case 0x18: case 0x19: skip = 4 + le32(b); break;
      case 0x20: case 0x23: case 0x24: skip = 2; break;
      case 0x21: case 0x30: skip = 1 + (uint32_t)b[0]; break;
      case 0x22: case 0x25: case 0x27: skip = 0; break;
      case 0x26: skip = 2 + 2 * le16(b); break;
      case 0x28: case 0x32: skip = 2 + le16(b); break;
      case 0x2A: skip = 4 + le32(b); break;
      case 0x2B: skip = 4 + le32(b); break;
      case 0x31: skip = 2 + (uint32_t)b[1]; break;
      case 0x33: skip = 1 + 3 * (uint32_t)b[0]; break;
      case 0x35: skip = 14 + le32(b + 10); break;
      case 0x5A: skip = 9; break;
      default:   return tapeCount;                    // unknown block: its length is unknowable
    }
    if (p + skip > L) break;
    p += skip;
  }
  return tapeCount;
}

} // namespace zx

#endif  // BOARD_HAS_ZX_CORE
