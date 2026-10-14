// picocalc_skin_png.S — docs/picocalc.png linked into emu8-picocalc (decoded by picocalc_skin.cpp).
// CMake passes the picture's path as EMU_SKIN_PNG (a quoted string); only that target builds this.
// (arduino-cli also compiles src/**: the guard keeps the device builds from seeing it.)
#if defined(EMU_SKIN_PNG)
#if defined(_WIN32)
  .section .rdata,"dr"
#else
  .section .rodata
#endif
  .balign 16
  .globl emu8_skin_png
emu8_skin_png:
  .incbin EMU_SKIN_PNG
  .globl emu8_skin_png_end
emu8_skin_png_end:
  .byte 0
#endif
