#include "../../emu.h"

// HSPI bus for the SD card. On the JC4827W543 the XPT2046 touch controller shares this same
// bus (same SCK/MISO/MOSI, its own CS), so it is exposed (extern in emu.h) instead of static.
SPIClass hspi { HSPI };
SemaphoreHandle_t gBusLock = NULL;   // touch-vs-SD arbitration on the shared HSPI bus

void FSSetup()
{
  if (!gBusLock) gBusLock = xSemaphoreCreateMutex();   // create before any touch/SD bus access
  hdAttached = HdDisk;
  diskAttached = !HdDisk;
  Serial.println("SD Card Setup");
  
  int sdMountRetry = 0;
      hspi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
#if !BOARD_TOUCH_VIA_TFT
      // Deselect the XPT2046 touch (it shares the SD MISO line) BEFORE mounting. On a cold boot
      // its CS floats and can drive MISO, corrupting the SD init — which is why the card mounted
      // only after a reset (the pin retained HIGH). Pull it high up front.
      pinMode(TOUCH_CS_PIN, OUTPUT);
      digitalWrite(TOUCH_CS_PIN, HIGH);
#endif
      // Run the SD bus at SD_SPI_HZ (20MHz) instead of the begin() default of 4MHz: directory
      // reads (esp. nested opendir) were the bottleneck after the per-entry fopen was removed.
      while (!FSTYPE.begin(SD_CS_PIN, hspi, SD_SPI_HZ) && sdMountRetry < 10) {
        printLog("Card Mount Failed");
        FSTYPE.end();      // reset SD/SPI state before retrying (helps cold-boot recovery)
        delay(120);
        sdMountRetry++;
      }

      if (sdMountRetry == 10) {
        hdAttached = false;
        diskAttached = false;
        return;
      }
      


  uint8_t cardType = FSTYPE.cardType();

  if (cardType == CARD_NONE) {
    printLog("No SD card attached");
    return;
  }

  printLog("SD Card Type: ");
  if (cardType == CARD_MMC) {
    printLog("MMC");
  } else if (cardType == CARD_SD) {
    printLog("SDSC");
  } else if (cardType == CARD_SDHC) {
    printLog("SDHC");
  } else {
    printLog("UNKNOWN");
  }
  
  uint64_t cardSize = FSTYPE.cardSize() / (1024 * 1024);
  sprintf(buf,"SD Card Size: %lluMB\n", cardSize);
  printLog(buf);


}