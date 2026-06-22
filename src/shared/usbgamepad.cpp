// usbgamepad.cpp - USB-HID host SNES gamepad input for the JC4827W543 (ESP32-S3 native USB).
//
// The S3's native USB-OTG port (GPIO19/20) is driven as a USB host (serial/flash is on a
// separate external CP210x/UART0, so the native port is dedicated to plugging in devices).
// A SNES-style USB gamepad shows up as a generic HID device; its interrupt-IN reports arrive
// in onReceive(), where we decode the d-pad + buttons into the same globals the ADC joystick
// drove (joyX/joyY/Pb0-3) and hand off to the shared applyPlatformInput(). Apple II also gets
// paddle (timerpdl) updates.
//
// Build target only: BOARD_INPUT_USB (the JC4827W543). The CYD keeps its analog joystick.
//
// Report format (decoded from a real pad over serial; 8-byte report):
//   byte0 = X axis  (0x00 left, ~0x7F center, 0xFF right)
//   byte1 = Y axis  (0x00 up,   ~0x7F center, 0xFF down)
//   byte5 = face buttons in the HIGH nibble (0x10/0x20/0x40/0x80); low nibble 0x0F is constant
//   byte6 = Select (0x10) / Start (0x20)
// (If the A/B feel is swapped for your pad, exchange GP_FACE_A and GP_FACE_B below.)

#include "../../emu.h"

#if BOARD_INPUT_USB

#include "EspUsbHost.h"

#define GP_AXIS_X     0
#define GP_AXIS_Y     1
#define GP_BTN_FACE   5      // byte holding the four face buttons (high nibble)
#define GP_BTN_AUX    6      // byte holding Select / Start
#define GP_FACE_A     0x60   // two face buttons -> primary fire / NES A / C64+Atari fire
#define GP_FACE_B     0x90   // the other two    -> NES B / secondary
#define GP_AUX_SELECT 0x10
#define GP_AUX_START  0x20

// Set to 1 to log every raw report to serial (for re-mapping a different controller).
#define GP_LOG_RAW    0

class SnesUsbHost : public EspUsbHost {
public:
  void onReceive(const usb_transfer_t *transfer) override {
    EspUsbHost *h = (EspUsbHost *)transfer->context;
    endpoint_data_t *ep = &h->endpoint_data_list[transfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_NUM_MASK];
    // Keyboard/mouse are handled by the base class; only decode the gamepad here.
    if (ep->bInterfaceProtocol == HID_ITF_PROTOCOL_KEYBOARD ||
        ep->bInterfaceProtocol == HID_ITF_PROTOCOL_MOUSE) return;
    parseGamepad(transfer->data_buffer, transfer->actual_num_bytes);
  }

  void onGone(const usb_host_client_event_msg_t *eventMsg) override {
    joyX = joyY = 1; Pb0 = Pb1 = Pb2 = Pb3 = false;
    timerpdl0 = timerpdl1 = JOY_MID;
    applyPlatformInput();
    printLog("USB gamepad disconnected");
  }

private:
  bool _prevMenu = false;
  int  _pmX = 1, _pmY = 1; bool _pmFire = false;

  void parseGamepad(const uint8_t *d, int n) {
    if (n < 2) return;

#if GP_LOG_RAW
    static uint8_t prev[20]; static int prevN = -1;
    int mm = n < 20 ? n : 20;
    if (n != prevN || memcmp(d, prev, mm)) {
      char line[96]; int p = snprintf(line, sizeof(line), "GP[%d]:", n);
      for (int i = 0; i < mm; i++) p += snprintf(line + p, sizeof(line) - p, " %02X", d[i]);
      Serial.println(line);
      prevN = n; memcpy(prev, d, mm);
    }
#endif

    // D-pad from the two axis bytes. joyX = vertical (0=up,2=down), joyY = horizontal
    // (0=left,2=right), matching what applyPlatformInput() / the in-menu nav expect.
    uint8_t ax = d[GP_AXIS_X], ay = d[GP_AXIS_Y];
    joyY = (ax < 0x40) ? 0 : (ax > 0xC0) ? 2 : 1;
    joyX = (ay < 0x40) ? 0 : (ay > 0xC0) ? 2 : 1;

    // Buttons.
    uint8_t face = (n > GP_BTN_FACE) ? d[GP_BTN_FACE] : 0;
    uint8_t aux  = (n > GP_BTN_AUX)  ? d[GP_BTN_AUX]  : 0;
    bool sel = (aux & GP_AUX_SELECT) != 0;
    bool sta = (aux & GP_AUX_START)  != 0;
    bool menuCombo = sel && sta;            // Select+Start together = open/close the options menu

    Pb0 = (face & GP_FACE_A) != 0;          // fire / A
    Pb1 = (face & GP_FACE_B) != 0;          // B
    Pb2 = sel && !menuCombo;                // Select (suppressed while the menu combo is held)
    Pb3 = sta && !menuCombo;                // Start

    // Apple II: digital d-pad -> full-deflection paddle positions.
    if (currentPlatform == PLATFORM_APPLE2) {
      timerpdl0 = (joyY == 0) ? JOY_MIN : (joyY == 2) ? JOY_MAX : JOY_MID;
      timerpdl1 = (joyX == 0) ? JOY_MIN : (joyX == 2) ? JOY_MAX : JOY_MID;
    }

    // Menu toggle (edge). The board has no physical buttons; touch is the primary way to open
    // the menu (M4), and this Select+Start shortcut lets the gamepad reach it too.
    if (menuCombo && !_prevMenu) showHideOptionsWindow();
    _prevMenu = menuCombo;

    // In-menu navigation: left/right move the selection, up/down adjust, fire activates.
    if (OptionsWindow) {
      if (joyY != _pmY) { if (joyY == 0) optionsUiNav(-1);    else if (joyY == 2) optionsUiNav(+1); }
      if (joyX != _pmX) { if (joyX == 0) optionsUiAdjust(-1); else if (joyX == 2) optionsUiAdjust(+1); }
      if (Pb0 && !_pmFire) optionsUiActivate();
    }
    _pmX = joyX; _pmY = joyY; _pmFire = Pb0;

    // Push directions + buttons to the active console core.
    applyPlatformInput();
  }
};

static SnesUsbHost usbHost;

static void usbGamepadTask(void *) {
  usbHost.begin();
  printLog("USB host started (SNES gamepad on native USB)");
  while (running) {
    usbHost.task();      // pump USB host events / deliver reports to onReceive()
    delay(1);
  }
  vTaskDelete(NULL);
}

void usbGamepadSetup() {
  // Pin the USB host task to core 1 (with the emulator CPU), AWAY from the core-0 render loop.
  // The render loop's per-frame QSPI flush + XPT2046 touch SPI on core 0 were starving the USB
  // host (its task + ISR also ran on core 0), making the gamepad drop and the board reset-loop.
  xTaskCreatePinnedToCore(usbGamepadTask, "usbGamepad", 4096, NULL, 3, NULL, 1);  // core 1
}

#endif // BOARD_INPUT_USB
