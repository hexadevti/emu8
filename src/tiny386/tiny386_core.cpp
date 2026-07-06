#if !defined(BOARD_JC4827W543)  // tiny386 is not built for the S3 board (too big; vendored core not wired for the device toolchain)
// tiny386_core.cpp — the side of the glue that includes the vendored tiny386 core (pc.h). Kept in a
// SEPARATE translation unit from tiny386.cpp because pc.h's `PC` machine type collides with the
// Apple II 6502 program-counter global `PC` declared in proto.h (pulled in by emu.h). This file does
// NOT include emu.h: it implements the platform HAL the core needs (get_uticks / bigmalloc /
// load_rom) and exposes the opaque t386_core_* bridge declared in tiny386_core.h.

#include "../../board.h"           // BOARD_* capability macros only (no PC clash)
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <chrono>

extern "C" {
#include "pc.h"
}
#include "tiny386_core.h"

#if !defined(BOARD_DESKTOP)
extern "C" void *ps_malloc(size_t);   // Arduino-ESP32: large PSRAM allocator
#endif

// ----------------------------------------------------------------------------------------------
// Platform HAL required by the vendored core (declared in pc.h). The upstream headless main.c
// supplies these on the host; we exclude it and provide our own.
// ----------------------------------------------------------------------------------------------
extern "C" uint32_t get_uticks()
{
  using namespace std::chrono;
  return (uint32_t)duration_cast<microseconds>(
           steady_clock::now().time_since_epoch()).count();   // free-running us (wraps; core copes)
}

extern "C" void *bigmalloc(size_t size)
{
#if defined(BOARD_DESKTOP)
  return malloc(size);
#else
  return ps_malloc(size);            // guest RAM + VGA RAM live in PSRAM
#endif
}

// BIOS/VGABIOS now live on the SD card under /roms/tiny386 (they used to be embedded arrays in
// tiny386_roms.c). The core passes the configured "bios.bin"/"vgabios.bin" names, matched here by
// substring; t386_read_sd (tiny386.cpp) pulls the image off the card. addr is where the ROM ends
// (backward = placed at addr-len).
extern "C" unsigned char *t386_read_sd(const char *sdPath, int *outLen);

extern "C" int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
  const char *sdPath = nullptr;
  if (file && strstr(file, "vgabios")) sdPath = "/roms/tiny386/vgabios.bin";
  else if (file && strstr(file, "bios")) sdPath = "/roms/tiny386/seabios.bin";
  if (sdPath) {
    int len = 0;
    unsigned char *rom = t386_read_sd(sdPath, &len);
    if (rom && len > 0) {
      memcpy((uint8_t *)phys_mem + (backward ? addr - len : addr), rom, len);
      free(rom);
      return len;
    }
    if (rom) free(rom);
    return 0;
  }
#if defined(BOARD_DESKTOP)
  if (file) {                        // host-file fallback (e.g. a Linux kernel/initrd)
    FILE *fp = fopen(file, "rb");
    if (fp) {
      fseek(fp, 0, SEEK_END); long l = ftell(fp); rewind(fp);
      fread((uint8_t *)phys_mem + (backward ? addr - l : addr), 1, l, fp);
      fclose(fp);
      return (int)l;
    }
  }
#endif
  return 0;
}

// ----------------------------------------------------------------------------------------------
// Bridge
// ----------------------------------------------------------------------------------------------
static uint16_t   *s_fb = nullptr;
static volatile bool s_dirty = true;

static void core_redraw(void * /*opaque*/, int /*x*/, int /*y*/, int /*w*/, int /*h*/)
{
  s_dirty = true;
}

void *t386_core_new(const T386Conf *c)
{
  size_t fbBytes = (size_t)c->fb_w * c->fb_h * 2;
  s_fb = (uint16_t *)bigmalloc(fbBytes);
  if (!s_fb) return nullptr;
  memset(s_fb, 0, fbBytes);

  PCConfig conf;
  memset(&conf, 0, sizeof(conf));
  conf.mem_size     = c->mem_size;
  conf.vga_mem_size = c->vga_size;
  conf.width        = c->fb_w;
  conf.height       = c->fb_h;
  conf.cpu_gen      = c->cpu_gen;
  conf.fpu          = c->fpu;
  conf.bios         = "bios.bin";
  conf.vga_bios     = "vgabios.bin";
  conf.fill_cmos    = 1;

  static char hda[300], fda[300];
  if (c->hda && c->hda[0]) { strncpy(hda, c->hda, sizeof(hda) - 1); conf.disks[0] = hda; }   // C:
  if (c->fda && c->fda[0]) { strncpy(fda, c->fda, sizeof(fda) - 1); conf.fdd[0]   = fda; }   // A:

  PC *pc = pc_new(core_redraw, nullptr, (u8 *)s_fb, &conf);
  if (!pc) return nullptr;
  load_bios_and_reset(pc);
  pc->boot_start_time = get_uticks();
  return pc;
}

uint16_t *t386_core_fb()                    { return s_fb; }
void      t386_core_step(void *pc)          { pc_step((PC *)pc); }
void      t386_core_vga_step(void *pc)      { pc_vga_step((PC *)pc); }
void      t386_core_request_reset(void *pc) { ((PC *)pc)->reset_request = 1; }
void      t386_core_force_redraw(void *pc)  { ((PC *)pc)->full_update = 2; s_dirty = true; }
int       t386_core_step_count()            { return 10240; }   // PC_STEP_COUNT (non-ESP path in pc.c)

bool t386_core_take_dirty()
{
  bool d = s_dirty;
  s_dirty = false;
  return d;
}

void t386_core_resolution(void *pc, int *w, int *h)
{
  vga_get_resolution(((PC *)pc)->vga, w, h);
}

void t386_core_put_keycode(void *pc, int is_down, int keycode)
{
  PS2KbdState *kbd = (PS2KbdState *)((PC *)pc)->kbd;
  if (kbd) ps2_put_keycode(kbd, is_down, keycode);
}

void t386_core_mouse(void *pc, int dx, int dy, int dz, int buttons)
{
  PS2MouseState *m = (PS2MouseState *)((PC *)pc)->mouse;
  // dy is screen-down-positive (ps2_mouse_event negates it); buttons bit0=L bit1=R bit2=M. Dropped
  // until the guest's PS/2 driver enables the mouse (MOUSE_STATUS_ENABLED) -- e.g. Windows 95.
  if (m) ps2_mouse_event(m, dx, dy, dz, buttons);
}

// Current PC-speaker tone: it plays the PIT-channel-2 square wave (freq = PIT_FREQ/count, mode 3),
// gated by port 0x61 (gate & data, exposed as pcspk_get_active_out). The emu8 audio ISR
// (speaker.cpp) turns freq/on into a square wave on the I2S amp.
void t386_core_speaker(void *pc, int *freq, int *on)
{
  PC *p = (PC *)pc;
  int active = p->pcspk ? pcspk_get_active_out(p->pcspk) : 0;
  int f = 0;
  if (active && p->pit && pit_get_mode(p->pit, 2) == 3) {
    int count = pit_get_initial_count(p->pit, 2);
    if (count > 0) f = (int)((long)PIT_FREQ / count);
  }
  if (on)   *on   = (active && f > 20 && f < 20000) ? 1 : 0;
  if (freq) *freq = f;
}

// Hot-mount a disk into the RUNNING machine (no device reboot). A: is the emulink paravirtual floppy
// (live media change -- the guest re-reads). C: re-attaches the primary IDE drive 0; the caller must
// then reset the emulated PC (re-POST) so SeaBIOS re-probes and boots the new disk. Safe because the
// UI pauses the CPU loop while Settings is open.
int t386_core_mount_floppy(void *pc, const char *path)
{
  PC *p = (PC *)pc;
  return p->emulink ? emulink_attach_floppy(p->emulink, 0, path) : -1;   // path NULL = eject
}
int t386_core_mount_hd(void *pc, const char *path)
{
  PC *p = (PC *)pc;
  return (p->ide && path) ? ide_attach(p->ide, 0, path) : -1;
}

#endif // !defined(BOARD_JC4827W543)
