#include "../../emu.h"

void logSetup() {
#if !defined(BOARD_PICOCALC) && !defined(BOARD_DESKTOP)
    // Room for a couple of SD file manager upload frames (sdserial.cpp): the default 256-byte
    // UART buffer overflows in ~2ms at the fast transfer rate. Must precede begin().
    Serial.setRxBufferSize(2 * SDSERIAL_MAX_PAYLOAD);
#endif
    Serial.begin(115200);
#if defined(BOARD_PICOCALC) && defined(BOOT_SERIAL_WAIT_MS)
    // No wait for a USB terminal on a normal boot: the serial log is a debugging aid and must
    // never hold up startup. When chasing a boot fault, build with -DBOOT_SERIAL_WAIT_MS=1500 so
    // USB enumeration finishes (and an already-open terminal attaches) before the first log line.
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < BOOT_SERIAL_WAIT_MS) delay(10);
    if (Serial) delay(200);   // let the host finish opening before the first line goes out
#endif
}

// The on-screen boot console that used to live here -- every printLog() line mirrored onto the
// panel during setup() -- has been replaced by the boot progress screen in src/shared/video.cpp.
// It is in video.cpp because it draws the boot logo, and bootlogo.h's array is `const` at
// namespace scope, so a second translation unit including it would have cost another 49KB of
// flash for a private copy.

void printLog(const char *txt) {
    if (sdSerialActive) return;   // the SD file manager owns the port (sdserial.cpp)
    Serial.println(txt);
}
void printLog(String txt) { printLog(txt.c_str()); }

void printSequence(int seq) {
  for (int i = 0; i < seq; i++) {
    sprintf(buf,"%02X ", actualBlock[i]);
    printLog(buf);
  }
  Serial.println();
}
  
void printProgress(size_t prg, size_t sz) {
    Serial.printf("Progress: %d%%\n", (prg * 100) / content_len);
  }
  
void printCPUStatus() {
  std::string sFlags = ""; 
  for (int f = 0;f<8;f++) {
    sFlags +=(SR & (1 << f)) != 0 ? "1" : "0";
  }
  sprintf(buf, "[PC]%04X: %02X ,[Addr]%04X(%02X): A=%02X X=%02X Y=%02X FL=%02X(%s) OPFlag=%02X, cycleCount=%d, diffCycleCount=%d", lastPC, opcode, argument_addr, read8(argument_addr), A, X, Y, SR, sFlags.c_str(), opflags, cycleCount, diffCpuCycleCount);
  printLog(buf);
}

void PrintHex(uint8_t data[], int length)
{
  for (int i = 0; i < length; i++)
  {
    if (i % 16 == 0)
    {
      Serial.println();
      sprintf(buf, "%08X: ", i);
      printLog(buf);
    }
    sprintf(buf, "%02X ", data[i]);
    printLog(buf);
  }
  Serial.println();
}
