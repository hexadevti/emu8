// pcmem.h - guest memory access for the emu8 PC-XT (8086) port.
//
// Two layouts behind one small API (rd8/wr8/rd16/wr16/ref8), so the BIOS/machine glue is written once:
//
//  * FLAT (ESP32-S3 / desktop): the guest address space is one 1MB buffer (`flat`), exactly what the
//    fabgl core always used. The 8086 core keeps its original direct-pointer macros in this mode.
//
//  * PAGED (PicoCalc): no 1MB buffer exists. The RP2040 has ~260KB of SRAM in total, so the guest
//    gets whatever RAM the heap can spare, in 4KB pages scattered through the heap. Two page tables
//    translate address>>12 into a host pointer:
//      rd[p]  always valid (unbacked pages point at a constant all-0xFF "open bus" page in flash)
//      wr[p]  nullptr when writes must be dropped (ROM / unbacked)
//    Pages 0x100..0x10F alias 0x00..0x0F (the 8086's A20 wrap: FFFF:0010 = 0), and page 0x110 is the
//    CPU register file (REGS_BASE) -- the core addresses registers and memory operands the same way.
//    Every 16-bit access is byte-wise: the Cortex-M0+ faults on unaligned halfword loads/stores.
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifndef PCXT_PAGED_MEM
  #if defined(BOARD_PICOCALC)
    #define PCXT_PAGED_MEM 1
  #else
    #define PCXT_PAGED_MEM 0
  #endif
#endif

namespace pcmem {

#if PCXT_PAGED_MEM

static const int      PAGE_SHIFT = 12;
static const uint32_t PAGE_SIZE  = 1u << PAGE_SHIFT;
static const uint32_t PAGE_MASK  = PAGE_SIZE - 1;
static const int      NPAGES     = 0x111;          // 0x000..0x10F guest (incl. A20 wrap) + 0x110 regs
static const uint32_t REGS_BASE  = 0x110000;

extern uint8_t * rd[NPAGES];
extern uint8_t * wr[NPAGES];
extern uint8_t   sink;                             // ref8() target for unwritable addresses

// Unmap everything: reads return 0xFF, writes are dropped; F000:FFF0 holds the reset JMP.
void clear();
// Back guest page `page` (0x00..0xFF) with host memory; nullptr `w` = read-only. Pages below 0x10
// are mirrored at +0x100 (A20 wrap).
void mapPage(int page, uint8_t * r, uint8_t * w);
// The constant F000:F000..FFFF page (reset JMP + model byte), to seed a RAM copy from.
const uint8_t * topPage();
// The register file lives at REGS_BASE (page 0x110).
void mapRegs(uint8_t * regs);

inline uint8_t rd8(uint32_t a)             { return rd[a >> PAGE_SHIFT][a & PAGE_MASK]; }
inline void    wr8(uint32_t a, uint8_t v)  { uint8_t * p = wr[a >> PAGE_SHIFT]; if (p) p[a & PAGE_MASK] = v; }

inline uint16_t rd16(uint32_t a) {
  if ((a & PAGE_MASK) != PAGE_MASK) {
    const uint8_t * p = rd[a >> PAGE_SHIFT] + (a & PAGE_MASK);
    return p[0] | (p[1] << 8);
  }
  return rd8(a) | (rd8(a + 1) << 8);
}
inline void wr16(uint32_t a, uint16_t v) {
  if ((a & PAGE_MASK) != PAGE_MASK) {
    uint8_t * p = wr[a >> PAGE_SHIFT];
    if (p) { p += a & PAGE_MASK; p[0] = v; p[1] = v >> 8; }
    return;
  }
  wr8(a, v); wr8(a + 1, v >> 8);
}

// Host pointer to a WRITABLE byte (nullptr if the page is ROM/unbacked).
inline uint8_t * wrPtr(uint32_t a)         { uint8_t * p = wr[a >> PAGE_SHIFT]; return p ? p + (a & PAGE_MASK) : nullptr; }
// Readable pointer (never null); valid up to the end of its 4KB page only.
inline const uint8_t * rdPtr(uint32_t a)   { return rd[a >> PAGE_SHIFT] + (a & PAGE_MASK); }
// Bytes left in the page holding `a`.
inline uint32_t pageLeft(uint32_t a)       { return PAGE_SIZE - (a & PAGE_MASK); }

// Lvalue byte: real memory when writable, else a scratch copy (reads OK, writes dropped).
inline uint8_t & ref8(uint32_t a) {
  uint8_t * p = wr[a >> PAGE_SHIFT];
  if (p) return p[a & PAGE_MASK];
  sink = rd8(a);
  return sink;
}

#else  // FLAT

extern uint8_t * flat;

inline uint8_t  rd8(uint32_t a)              { return flat[a]; }
inline void     wr8(uint32_t a, uint8_t v)   { flat[a] = v; }
inline uint16_t rd16(uint32_t a)             { return flat[a] | (flat[a + 1] << 8); }
inline void     wr16(uint32_t a, uint16_t v) { flat[a] = v; flat[a + 1] = v >> 8; }
inline uint8_t * wrPtr(uint32_t a)           { return flat + a; }
inline const uint8_t * rdPtr(uint32_t a)     { return flat + a; }
inline uint32_t pageLeft(uint32_t a)         { return 0x110000 - a; }
inline uint8_t & ref8(uint32_t a)            { return flat[a]; }

#endif

// Copy between host buffers and guest memory (page-boundary safe, drops writes to ROM).
inline void copyIn(uint32_t dst, const uint8_t * src, uint32_t n) { while (n--) wr8(dst++, *src++); }
inline void copyOut(uint8_t * dst, uint32_t src, uint32_t n)      { while (n--) *dst++ = rd8(src++); }

} // namespace pcmem
