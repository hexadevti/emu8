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
#define PICOCALC_SDMGR_CMD  "@sdmgr\n"
#define PICOCALC_RESET_CMD  "@reset\n"
#define PICOCALC_BLANK_CMD  "@blankcfg\n"

void picocalcRebootToLoaderMenu(const char *why)
{
  printLog(why);
  delay(50);
  watchdog_hw->scratch[1] = UF2LOADER_BOOT_SD;
  watchdog_hw->scratch[2] = 0;
  watchdog_hw->scratch[0] = UF2LOADER_MAGIC;
  ESP.restart();                        // rp2040.restart(): watchdog_reboot(0, 0, ..), scratch 0-3 kept
}

static uint8_t s_menuMatched = 0, s_sdmMatched = 0, s_rstMatched = 0, s_blkMatched = 0;

// Advance one command matcher by one byte; true if the byte continued it. A byte that breaks the
// match is retried as a fresh start.
static bool cmdStep(const char *cmd, uint8_t &matched, uint8_t c)
{
  if (c != (uint8_t)cmd[matched]) matched = 0;
  if (c != (uint8_t)cmd[matched]) return false;
  ++matched;
  return true;
}

// One byte of the serial stream. True if it continued a command (the caller should swallow it);
// on the last byte this does not return. Four commands:
//   PICOCALC_LOADER_CMD: reboot into the UF2 Loader menu.
//   PICOCALC_SDMGR_CMD:  reboot into SD Manager mode, so the PC can upload over sdserial
//                        (tools/deploy-serial.py: no USB drive, nothing to eject).
//   PICOCALC_RESET_CMD:  hard reset into the same system (like Ctrl-Shift-F3), so the PC can
//                        watch a whole boot log without a hand on the keyboard.
//   PICOCALC_BLANK_CMD:  erase the settings flash to its never-written state (all 0xFF) and
//                        reboot -- the first boot of a brand-new device, on demand (eprom.cpp).
bool picocalcLoaderCmdFeed(uint8_t c)
{
  static const char menu[] = PICOCALC_LOADER_CMD, sdm[] = PICOCALC_SDMGR_CMD, rst[] = PICOCALC_RESET_CMD,
                    blk[] = PICOCALC_BLANK_CMD;
  bool a = cmdStep(menu, s_menuMatched, c), b = cmdStep(sdm, s_sdmMatched, c), r = cmdStep(rst, s_rstMatched, c),
       k = cmdStep(blk, s_blkMatched, c);
  if (!rst[s_rstMatched]) { printLog("USB serial asked for a hard reset"); delay(50); ESP.restart(); }
  if (!blk[s_blkMatched]) {
    printLog("USB serial asked for blank settings (new-device boot)");
    for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0xFF);
    EEPROM.commit();
    delay(50);
    ESP.restart();
  }
  if (!menu[s_menuMatched]) picocalcRebootToLoaderMenu("USB serial asked for the UF2 Loader menu");
  if (!sdm[s_sdmMatched]) {
    s_sdmMatched = 0;
    if (currentPlatform != PLATFORM_SDMANAGER) { printLog("USB serial asked for the SD Manager"); rebootToSdManager(); }
  }
  return a || b || r || k;
}

// Called once per frame from picocalcPumpInput(). Other bytes are dropped so they can't block the
// command. (A 'D' under the C64 used to be left in the buffer for its frame dump; that reader is
// gone, so one stray 'D' stalled every later command, "@sdmgr" included, until a reset.)
void picocalcCheckLoaderCmd()
{
  if (currentPlatform == PLATFORM_SDMANAGER) return;       // sdserial reads the port there
  while (Serial.available()) {
    int c = Serial.peek();
    if (c < 0) break;
    picocalcLoaderCmdFeed((uint8_t)c);
    Serial.read();
  }
}

#elif defined(BOARD_PICOCALC_P4)

// The P4 counterpart is esp32loader: its bootloader boots the loader instead of the app when
// LP_STORE15 holds its magic across a software reset (esp32loader main/loader_api.h). Without
// esp32loader installed this is a plain restart. Same entry points as above: Ctrl-Shift-Up, and
// PICOCALC_LOADER_CMD on the console UART, so the PC can get to the loader's web upload (F4)
// without a hand on the keyboard.
#include "soc/soc.h"
#include "soc/lp_system_reg.h"
#include "esp_system.h"

#define ESP32LOADER_MAGIC   0x10ADE125u
#define PICOCALC_LOADER_CMD "@uf2menu\n"

void picocalcRebootToLoaderMenu(const char *why)
{
  printLog(why);
  if (currentPlatform != PLATFORM_SDMANAGER) saveConfig();
  delay(50);
  REG_WRITE(LP_SYSTEM_REG_LP_STORE15_REG, ESP32LOADER_MAGIC);
  esp_restart();
}

void picocalcCheckLoaderCmd()
{
  static const char menu[] = PICOCALC_LOADER_CMD;
  static uint8_t matched = 0;
  if (currentPlatform == PLATFORM_SDMANAGER) return;       // sdserial reads the port there
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    if (c != (uint8_t)menu[matched]) matched = 0;
    if (c == (uint8_t)menu[matched] && !menu[++matched]) picocalcRebootToLoaderMenu("Serial asked for esp32loader");
  }
}

#endif  // BOARD_PICOCALC / BOARD_PICOCALC_P4
