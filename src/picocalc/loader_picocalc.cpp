// loader_picocalc.cpp - hand the Pico back to the UF2 Loader's SD menu (github.com/pelrun/uf2loader).
//
// The loader only opens its menu if Up/F1/F5 shows up as a fresh key press in the first 0.5 s after
// reset, which is hard to hit, and with the Pico on its own USB the power switch never resets it
// anyway. But the loader's stage 3 also takes a command from the watchdog scratch registers, which
// survive a watchdog reboot -- its own menu UI uses exactly this to hand work back. So write "boot to
// SD menu" there and reboot. Without the loader installed nothing reads the scratch registers and
// this is a plain reboot.
//
// Two ways in: Ctrl-Shift-Up (input_picocalc.cpp), and the PC sending PICOCALC_LOADER_CMD over the
// USB serial port (tools/deploy-picocalc.ps1) -- in every mode: picocalcCheckLoaderCmd() reads it
// per frame, and in SD Manager mode, where sdserial owns the port, its parser passes the bytes
// between frames to picocalcLoaderCmdFeed().

#include "../../emu.h"

#if defined(BOARD_PICOCALC)

#include <hardware/watchdog.h>

#define UF2LOADER_MAGIC   0xe98cc638u   // PICOCALC_BL_MAGIC, uf2loader common/bootloader/proginfo.h
#define UF2LOADER_BOOT_SD 1u            // enum bootmode_e { BOOT_DEFAULT, BOOT_SD, ... }
#define PICOCALC_LOADER_CMD "@uf2menu\n"

void picocalcRebootToLoaderMenu(const char *why)
{
  printLog(why);
  delay(50);
  watchdog_hw->scratch[1] = UF2LOADER_BOOT_SD;
  watchdog_hw->scratch[2] = 0;
  watchdog_hw->scratch[0] = UF2LOADER_MAGIC;
  ESP.restart();                        // rp2040.restart(): watchdog_reboot(0, 0, ..), scratch 0-3 kept
}

static uint8_t s_cmdMatched = 0;

// One byte of the serial stream. True if it continued the command (the caller should swallow it);
// on the last byte this does not return.
bool picocalcLoaderCmdFeed(uint8_t c)
{
  static const char cmd[] = PICOCALC_LOADER_CMD;
  if (c != (uint8_t)cmd[s_cmdMatched]) s_cmdMatched = 0;   // broken off: retry as a fresh start
  if (c != (uint8_t)cmd[s_cmdMatched]) return false;
  if (!cmd[++s_cmdMatched]) picocalcRebootToLoaderMenu("USB serial asked for the UF2 Loader menu");
  return true;
}

// Called once per frame from picocalcPumpInput(). Other bytes are dropped so they can't block the
// command, except a 'D' under the C64, which is the trigger of its frame dump (c64DumpFrame).
void picocalcCheckLoaderCmd()
{
  if (currentPlatform == PLATFORM_SDMANAGER) return;       // sdserial reads the port there
  while (Serial.available()) {
    int c = Serial.peek();
    if (c < 0) break;
    if (!picocalcLoaderCmdFeed((uint8_t)c) && currentPlatform == PLATFORM_C64 && c == 'D') break;
    Serial.read();
  }
}

#endif  // BOARD_PICOCALC
