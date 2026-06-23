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
// Set to 1 to log every HID report (onReceive + onKeyboard) to serial, e.g. to capture a keyboard's
// media-key report format when adding a new shortcut.
#define USB_RX_DEBUG  0

// NOTE on hot-swap: after a device disconnect the ESP32-S3 USB host leaves the root port wedged,
// and the next plugged device fails to enumerate ("HUB: Failed to issue second reset / Root port
// reset failed"). A full host re-install (usb_host_uninstall + begin) does NOT clear it (the port
// reset still fails) and repeating it can crash the board, so it isn't attempted. The native USB
// port has no GPIO VBUS control to power-cycle the device. Workaround: tap RST after swapping
// devices — they enumerate cleanly on a cold boot.
class SnesUsbHost : public EspUsbHost {
public:
  void onReceive(const usb_transfer_t *transfer) override {
    EspUsbHost *h = (EspUsbHost *)transfer->context;
    endpoint_data_t *ep = &h->endpoint_data_list[transfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_NUM_MASK];
#if USB_RX_DEBUG
    { int n = transfer->actual_num_bytes; char ln[160];
      int p = snprintf(ln, sizeof(ln), "RX ep=%02x cls=%02x sub=%02x proto=%02x n=%d:",
                       transfer->bEndpointAddress, ep->bInterfaceClass, ep->bInterfaceSubClass, ep->bInterfaceProtocol, n);
      for (int i = 0; i < n && i < 20; i++) p += snprintf(ln + p, sizeof(ln) - p, " %02X", transfer->data_buffer[i]);
      Serial.println(ln); }
#endif
    // A USB keyboard's media keys (Consumer Control) ride the keyboard endpoint here as a short
    // report-ID'd report, e.g. "02 E9 00" = Vol+, "02 EA 00" = Vol- (report ID 0x02, then the 16-bit
    // usage). They arrive with proto=KEYBOARD, so catch Volume Up/Down BEFORE the keyboard/mouse
    // early-return below (the boot keyboard's own 8-byte reports are too long to match, so normal
    // typing is untouched and still flows through onKeyboard).
    if (handleConsumerVolume(transfer->data_buffer, transfer->actual_num_bytes)) return;
    // Keyboard/mouse are handled by the base class (onKeyboard below); only decode the gamepad here.
    if (ep->bInterfaceProtocol == HID_ITF_PROTOCOL_KEYBOARD ||
        ep->bInterfaceProtocol == HID_ITF_PROTOCOL_MOUSE) return;
    parseGamepad(transfer->data_buffer, transfer->actual_num_bytes);
  }

  // A USB keyboard shares this host: the base class decodes its boot report and calls us here.
  // Forward the raw report (modifier + 6-key rollover) to the per-platform mapper (usbkeyboard.cpp).
  void onKeyboard(hid_keyboard_report_t report, hid_keyboard_report_t last_report) override {
#if USB_RX_DEBUG
    Serial.printf("KB mod=%02x keys= %02X %02X %02X %02X %02X %02X\n", report.modifier,
                  report.keycode[0], report.keycode[1], report.keycode[2],
                  report.keycode[3], report.keycode[4], report.keycode[5]);
#endif
    usbKeyboardReport(report.modifier, report.keycode, last_report.keycode);
  }

  void onGone(const usb_host_client_event_msg_t *eventMsg) override {
    joyX = joyY = 1; Pb0 = Pb1 = Pb2 = Pb3 = false;
    timerpdl0 = timerpdl1 = JOY_MID;
    applyPlatformInput();
    _prevConsumer = 0;    // forget any half-pressed media key
    usbKeyboardReset();   // release any keys a disconnected keyboard was holding
    printLog("USB device disconnected (tap RST after plugging a new device)");
  }

private:
  bool _prevMenu = false;
  int  _pmX = 1, _pmY = 1; bool _pmFire = false;
  uint8_t _prevConsumer = 0;   // last Consumer-Control volume usage seen (press/release edge tracking)

  // Master volume from a USB keyboard's media keys. The Consumer-Control report is short and carries
  // a 16-bit usage; Volume Up = 0xE9, Volume Down = 0xEA (Consumer page). We step the app volume once
  // per press (edge-detected) and swallow the report so the gamepad decoder never sees it. Returns
  // true if the short report was a media-volume press/release (consumed), false otherwise.
  bool handleConsumerVolume(const uint8_t *d, int n) {
    if (!d || n < 1 || n > 4) return false;        // gamepad reports are 8 bytes; media reports are short
    uint8_t cur = 0;
    for (int i = 0; i < n; i++) if (d[i] == 0xE9 || d[i] == 0xEA) { cur = d[i]; break; }
    if (!cur && !_prevConsumer) return false;      // short report, no volume usage, none pending: not ours
    if (cur && cur != _prevConsumer) {             // press edge -> one step (0x10 of the 0x00..0xF0 range)
      if (cur == 0xE9) { if (volume < 0xf0) volume += 0x10; }
      else             { if (volume > 0) { volume -= 0x10; if (volume > 0xf0) volume = 0; } }
    }
    _prevConsumer = cur;                            // track so holding a media key steps only once
    return true;
  }

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
  printLog("USB host started (SNES gamepad / keyboard on native USB)");
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
