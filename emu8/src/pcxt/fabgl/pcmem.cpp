// Not built for the PicoCalc on an RP2040 (Cortex-M0+): its heap leaves ~75KB of guest RAM, too
// little for DOS ("Configuration too large for memory"). The RP2350 runs it paged (fabgl/pcmem.h);
// see BOARD_HAS_PCXT_CORE in board.h. The RP2040 links against src/picocalc/bigram_stubs.cpp.
#if !(defined(BOARD_PICOCALC) && defined(__ARM_ARCH_6M__))
// pcmem.cpp - page tables for the PAGED guest memory layout (see pcmem.h).
#include "pcmem.h"

namespace pcmem {

#if PCXT_PAGED_MEM

uint8_t * rd[NPAGES];
uint8_t * wr[NPAGES];
uint8_t   sink;

namespace {
  // Built at compile time so both live in flash, not in the RP2040's scarce SRAM.
  struct OpenBusPage {
    uint8_t d[PAGE_SIZE];
    constexpr OpenBusPage() : d() { for (auto & b : d) b = 0xFF; }
  };
  // F000:F000..FFFF. The BIOS image ends at ~F000:4080, so the top page only needs what a real ROM
  // has there: the reset vector (JMP F000:0100, what BIOS::init used to poke) and the model byte.
  struct TopPage {
    uint8_t d[PAGE_SIZE];
    constexpr TopPage() : d() {
      for (auto & b : d) b = 0xFF;
      d[0xFF0] = 0xEA; d[0xFF1] = 0x00; d[0xFF2] = 0x01; d[0xFF3] = 0x00; d[0xFF4] = 0xF0;
      d[0xFFE] = 0xFE;   // PC/XT
    }
  };
  constexpr OpenBusPage kOpenBus;
  constexpr TopPage     kTop;
}

void clear() {
  for (int p = 0; p < NPAGES; ++p) {
    rd[p] = const_cast<uint8_t *>(kOpenBus.d);
    wr[p] = nullptr;
  }
  rd[0xFF] = const_cast<uint8_t *>(kTop.d);
}

const uint8_t * topPage() { return kTop.d; }

void mapPage(int page, uint8_t * r, uint8_t * w) {
  rd[page] = r;
  wr[page] = w;
  if (page < 0x10) { rd[page + 0x100] = r; wr[page + 0x100] = w; }
}

void mapRegs(uint8_t * regs) {
  rd[REGS_BASE >> PAGE_SHIFT] = regs;
  wr[REGS_BASE >> PAGE_SHIFT] = regs;
}

#else

uint8_t * flat;

#endif

} // namespace pcmem
#endif // !(BOARD_PICOCALC && RP2040)
