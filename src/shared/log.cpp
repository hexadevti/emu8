#include "../../emu.h"

void logSetup() {
#if !defined(BOARD_PICOCALC) && !defined(BOARD_DESKTOP)
    // Room for a couple of SD file manager upload frames (sdserial.cpp): the default 256-byte
    // UART buffer overflows in ~2ms at the fast transfer rate. Must precede begin().
    Serial.setRxBufferSize(2 * SDSERIAL_MAX_PAYLOAD);
#endif
    Serial.begin(115200);
#if defined(BOARD_PICOCALC)
    // arduino-pico services USB CDC from a FreeRTOS task that only starts once the scheduler is
    // running, so the host is still MID-ENUMERATION when setup() begins. Anything that faults in
    // those first few milliseconds takes core 0 down with it -- including the USB task -- and the
    // port never appears at all ("USB device not recognized"), which hides the very crash we need
    // to read. Idling here lets enumeration finish first, and waiting for the host to actually
    // open the port means the boot log is not lost to a terminal that attaches too late.
    // Serial goes truthy when the host OPENS the port, not when it enumerates, so with no
    // terminal listening this loop never exits early and costs the full budget on EVERY boot.
    // It was 5000ms during bring-up and that was most of a visibly slow start. Enumeration is
    // the part that actually needs covering and it takes a few hundred ms, so 1500 covers it
    // with room to spare and still catches a terminal that is already open. Waiting longer than
    // this to let someone ATTACH one never worked anyway -- opening a terminal by hand takes
    // more than five seconds. Raise it with -DBOOT_SERIAL_WAIT_MS=... when chasing a boot fault.
  #ifndef BOOT_SERIAL_WAIT_MS
  #define BOOT_SERIAL_WAIT_MS 1500
  #endif
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

void printLog(String txt) {
    if (sdSerialActive) return;   // the SD file manager owns the port (sdserial.cpp)
    Serial.println(txt.c_str());
}

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
