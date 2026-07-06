// tiny386_core.h — thin C bridge over the vendored tiny386 core (pc.h). Lets the emu8 glue
// (tiny386.cpp) drive the machine WITHOUT including pc.h, whose `PC` machine type would clash with
// the Apple II 6502 program-counter global `PC` (proto.h). The core lives in its own translation
// unit (tiny386_core.cpp) that includes pc.h but NOT emu.h; the glue includes emu.h but talks to
// the machine only through these opaque (void*) calls.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t mem_size;     // guest RAM bytes
  uint32_t vga_size;     // VGA RAM bytes
  int      cpu_gen;      // 3=386 4=486 5/6=586/686
  int      fpu;          // enable x87
  int      fb_w, fb_h;   // RGB565 framebuffer the VGA renders into
  const char *hda;       // C: primary IDE disk image path, or NULL
  const char *fda;       // A: floppy image path (emulink), or NULL
} T386Conf;

void     *t386_core_new(const T386Conf *c);  // build the PC (+fb via bigmalloc), load BIOS, reset; PC* or NULL
uint16_t *t386_core_fb(void);                // the RGB565 framebuffer (fb_w*fb_h)
void      t386_core_step(void *pc);          // run one pc_step (an i386 chunk + device housekeeping)
void      t386_core_vga_step(void *pc);      // render into the fb when the VGA signals a refresh
void      t386_core_request_reset(void *pc); // soft-reboot the PC at the next step
void      t386_core_force_redraw(void *pc);  // force a full repaint on the next vga step
int       t386_core_step_count(void);        // i386 instructions per pc_step (for the boot benchmark)
bool      t386_core_take_dirty(void);        // true (and clears) if the fb changed since the last call
void      t386_core_resolution(void *pc, int *w, int *h);  // active VGA mode pixel dims (0 until 1st refresh)
void      t386_core_put_keycode(void *pc, int is_down, int keycode);  // feed a PS/2 set-1 keycode
void      t386_core_mouse(void *pc, int dx, int dy, int dz, int buttons);  // feed a PS/2 mouse event
void      t386_core_speaker(void *pc, int *freq, int *on);  // current PC-speaker tone (PIT ch2 + port 0x61)
int       t386_core_mount_floppy(void *pc, const char *path);  // A: live media change (NULL = eject)
int       t386_core_mount_hd(void *pc, const char *path);      // C: re-attach (caller resets the PC to re-probe)

#ifdef __cplusplus
}
#endif
