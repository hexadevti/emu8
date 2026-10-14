#include "../../emu.h"

// HSPI bus for the SD card. On the JC4827W543 the XPT2046 touch controller shares this same
// bus (same SCK/MISO/MOSI, its own CS), so it is exposed (extern in emu.h) instead of static.
// Not used on the P4 (SD is SD_MMC, touch is I2C), where the HSPI constant may not even exist.
#if !BOARD_SD_MMC && !defined(BOARD_PICOCALC)
SPIClass hspi { HSPI };
#endif
SemaphoreHandle_t gBusLock = NULL;   // touch-vs-SD arbitration on the shared HSPI bus
bool sdCardMounted = false;          // FSSetup() mounted a card (read by the serial file manager)

// Free space on the mounted card, for the boot log lines in DiskIISetup()/HDSetup().
// Arduino-ESP32 (and the desktop shim) answer this with totalBytes()/usedBytes(); arduino-pico's
// fs::FS has neither and reports the same two numbers through info(FSInfo&) instead, so the split
// lives here rather than being duplicated at each call site.
uint64_t sdFreeBytes()
{
  // Under the bus lock: on the PicoCalc this walks the whole FAT (SDFS::info() calls SdFat's
  // freeClusterCount()), and SDFS has no internal locking, so a concurrent scan from another
  // task corrupts the shared volume cache mid-walk. See the busTake() note in FSSetup().
  busTake();
#if defined(BOARD_PICOCALC)
  // SDFS::info() gets usedBytes from SdFat freeClusterCount(), which walks the ENTIRE FAT. On a
  // 32GB FAT32 card that is ~4MB of table, and the PicoCalc slot may have negotiated down to
  // 4MHz -- measured at ~25 seconds, spent before the first ROM is even opened. Nothing checks
  // this number; it exists for one boot log line. Skip the walk and say so rather than paying
  // half a minute of boot for it twice.
  static bool told = false;
  if (!told) { printLog("SD: free-space walk skipped on this board (too slow over SPI)"); told = true; }
  uint64_t free = 0;
#else
  uint64_t free = (uint64_t)FSTYPE.totalBytes() - (uint64_t)FSTYPE.usedBytes();
#endif
  busGive();
  return free;
}

void FSSetup()
{
  if (!gBusLock) gBusLock = xSemaphoreCreateMutex();   // create before any touch/SD bus access
  hdAttached = HdDisk;
  diskAttached = !HdDisk;
  Serial.println("SD Card Setup");
  
  int sdMountRetry = 0;
#if BOARD_SD_MMC
      // ESP32-P4: microSD over the SDMMC (SDIO) peripheral. Mount 1-bit (CLK/CMD/D0 only) for
      // maximum compatibility; the same /sd mountpoint and FSTYPE (SD_MMC) keep the file code
      // unchanged. No shared SPI bus here, so no touch-CS pre-deselect is needed.
      SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN);
      while (!FSTYPE.begin(SD_VFS_ROOT, /*mode1bit=*/true) && sdMountRetry < 10) {
        printLog("Card Mount Failed");
        FSTYPE.end();
        delay(120);
        sdMountRetry++;
      }
      if (sdMountRetry == 10) {
        hdAttached = false;
        diskAttached = false;
        return;
      }
#elif defined(BOARD_PICOCALC)
      // PicoCalc: microSD on spi0, which arduino-pico's rpipico2 variant already defaults to the
      // same pins -- set them anyway so a variant change cannot silently break the card. There is
      // no global VFS and no SD.begin(cs, bus, hz) overload here, so the bus/speed/CS go through
      // SDFSConfig instead. Nothing shares this bus (the panel is on spi1), so no touch pre-deselect.
      SPI.setSCK(SD_SCK_PIN);
      SPI.setTX(SD_MOSI_PIN);
      SPI.setRX(SD_MISO_PIN);
      (void)sdMountRetry;                // the ladder below replaces the shared retry counter


      // Mount, stepping the clock down on each failure rather than retrying the same speed ten
      // times. SD_SPI_HZ (20MHz) is the target, but the PicoCalc slot hangs off a ribbon and not
      // every card will hold that -- a slow card beats no card, and the log says which rate won.
      static const uint32_t kSdRates[] = { SD_SPI_HZ, 12000000, 4000000, 1000000 };
      bool mounted = false;
      for (unsigned i = 0; i < sizeof(kSdRates) / sizeof(kSdRates[0]) && !mounted; i++) {
        SDFS.setConfig(SDFSConfig(SD_CS_PIN, kSdRates[i], SPI));
        mounted = FSTYPE.begin();
        if (!mounted) {
          sprintf(buf, "Card Mount Failed @ %lu Hz", (unsigned long)kSdRates[i]);
          printLog(buf);
          FSTYPE.end();
          delay(50);
        } else if (i) {
          sprintf(buf, "SD: mounted at %lu Hz (fell back from %lu)",
                  (unsigned long)kSdRates[i], (unsigned long)SD_SPI_HZ);
          printLog(buf);
        }
      }
      if (!mounted) {
        // Every rate failed. SDFS throws SdFat's error code away -- SDFSImpl::_fs is private and
        // the global SDFS is a plain fs::FS -- so mount a throwaway SdFat on the same pins purely
        // to read it back. That code says WHICH stage of the init died, which is the whole
        // question here:
        //   CMD0            - nothing answered at all: CS, wiring, or the slot has no power
        //   CMD8 / ACMD41   - the card talks but will not initialise: signal integrity, or a card
        //                     this slot cannot negotiate with
        //   no error code   - the card is fine and the partition/format is what SdFat cannot read
        SdFat *probe = new SdFat();
        if (probe) {
          SdSpiConfig pcfg(SD_CS_PIN, SHARED_SPI, 1000000, &SPI);
          if (probe->begin(pcfg)) {
            printLog("SD: raw SdFat mounted it - the SDFS wrapper is the problem, not the card");
          } else {
            Serial.print("SD: ");
            probe->printSdError(&Serial);
          }
          delete probe;
        }
        hdAttached = false;
        diskAttached = false;
        return;
      }
#else
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
#endif



#if defined(BOARD_PICOCALC)
  // No capacity line here. SDFS has no cardType()/cardSize(), and the global SDFS object is a
  // plain fs::FS, so the only size route is info() -- whose usedBytes comes from SdFat
  // freeClusterCount(), i.e. a walk of the ENTIRE FAT. On the 32GB FAT32 card this was found on,
  // over a link that had negotiated down to 4MHz, that measured ~24 seconds of dead boot time for
  // one cosmetic log line. The mount itself already proves the card is readable.
  printLog("SD: card mounted");
  sdCardMounted = true;
#else
  uint8_t cardType = FSTYPE.cardType();

  if (cardType == CARD_NONE) {
    printLog("No SD card attached");
    return;
  }
  sdCardMounted = true;

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
#endif
}
