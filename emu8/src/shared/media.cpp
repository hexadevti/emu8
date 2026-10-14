#include "../../emu.h"

// media.cpp - the one "is it in / take it out" API every machine shares.
//
// Each core loads media its own way (a cartridge, a floppy, a tape, a snapshot, a program), so each
// supplies a <core>MediaMounted / <core>Unmount pair (proto.h). The settings browser (optionsui.cpp)
// and the desktop Load window only call the two dispatchers below, which is what makes UNMOUNT look
// and behave the same on every platform: highlight the loaded file, press UNMOUNT (or Del).
//
// The MSX and the PC-XT had their own unmount calls before this existed; they are wrapped here.

// MSX: a .dsk and a cartridge can both be in at once, one per slot.
static bool msxMediaMounted(const char *path) { return msxDiskMounted(path) || msxCartLoaded(path); }
static void msxUnmount(const char *path)
{
  if (msxDiskMounted(path)) msxUnmountDisk();   // live, no reset
  if (msxCartLoaded(path))  msxUnloadCart();    // resets the MSX (back to BASIC)
}

// PC-XT: A: (floppy) and C: (hard disk). An image can sit in both; take it out of each.
static bool pcxtIn(const String &slot, const char *path) { return slot.length() && slot == path; }
static bool pcxtMediaMounted(const char *path)
{
  return pcxtIn(selectedPcFileName, path) || pcxtIn(selectedPcHdFileName, path);
}
static void pcxtUnmountPath(const char *path)
{
  if (pcxtIn(selectedPcFileName, path))   pcxtUnmount(0);   // A: ejects live
  if (pcxtIn(selectedPcHdFileName, path)) pcxtUnmount(2);   // C: the running DOS loses its disk
}

bool mediaIsMounted(const char *path)
{
  if (!path || !*path) return false;
  switch (currentPlatform) {
    case PLATFORM_APPLE2: return apple2MediaMounted(path);
    case PLATFORM_C64:    return c64MediaMounted(path);
    case PLATFORM_NES:    return nesMediaMounted(path);
    case PLATFORM_ATARI:  return atariMediaMounted(path);
    case PLATFORM_MSX:    return msxMediaMounted(path);
    case PLATFORM_SMS:    return smsMediaMounted(path);
    case PLATFORM_COLECO: return colecoMediaMounted(path);
    case PLATFORM_ZX:     return zxMediaMounted(path);
    case PLATFORM_PCXT:   return pcxtMediaMounted(path);
    default:              return false;
  }
}

bool mediaUnmount(const char *path)
{
  if (!mediaIsMounted(path)) return false;
  switch (currentPlatform) {
    case PLATFORM_APPLE2: apple2Unmount(path);   break;
    case PLATFORM_C64:    c64Unmount(path);      break;
    case PLATFORM_NES:    nesUnmount(path);      break;
    case PLATFORM_ATARI:  atariUnmount(path);    break;
    case PLATFORM_MSX:    msxUnmount(path);      break;
    case PLATFORM_SMS:    smsUnmount(path);      break;
    case PLATFORM_COLECO: colecoUnmount(path);   break;
    case PLATFORM_ZX:     zxUnmount(path);       break;
    case PLATFORM_PCXT:   pcxtUnmountPath(path); break;
    default:              return false;
  }
  return true;
}
