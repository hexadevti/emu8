// fault_picocalc.cpp - make the two silent FreeRTOS aborts visible on the PicoCalc.
//
// arduino-pico builds the kernel with configCHECK_FOR_STACK_OVERFLOW 2 and
// configUSE_MALLOC_FAILED_HOOK 1, and its default hooks (cores/rp2040/freertos/variantHooks.cpp)
// both call the pico-sdk panic(), which disables interrupts and spins forever on the calling core.
// On this board core 0 also owns the USB CDC task, so a panic there does not just stop the
// firmware -- it stops the serial port from ever enumerating, and the host reports only
// "USB device not recognized". The failure that caused it is then completely invisible.
//
// Both hooks are declared weak in the core, so these definitions replace them. They say what
// happened over Serial and then park the offending task WITHOUT touching interrupts, which keeps
// the USB task alive so the message can actually be read.
#include "../../board.h"

#if defined(BOARD_PICOCALC)

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>

// Printed straight to Serial rather than through printLog(): printLog takes an Arduino String,
// which allocates -- and one of the two callers is here precisely because allocation just failed.
static void __attribute__((noreturn)) reportAndPark(const char *what, const char *who) {
  for (;;) {
    Serial.print("FATAL: ");
    Serial.print(what);
    if (who) { Serial.print(" in task '"); Serial.print(who); Serial.print("'"); }
    Serial.println();
    vTaskDelay(2000 / portTICK_PERIOD_MS);   // repeat: the host may attach after the fact
  }
}

extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
  (void)xTask;
  reportAndPark("FreeRTOS stack overflow", pcTaskName);
}

extern "C" void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void) {
  reportAndPark("FreeRTOS pvPortMalloc failed (out of heap)", pcTaskGetName(NULL));
}

#endif // BOARD_PICOCALC
