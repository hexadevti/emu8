// picocalc-p4-bringup.ino -- hardware bring-up for a Waveshare ESP32-P4-WIFI6 in the PicoCalc's
// Pico socket. Standalone (not part of emu8): it proves every PicoCalc peripheral is reachable
// through the P4's pin map before the emulator is ported on top of it.
//
// What it checks, in order:
//   1. LCD   ST7365P 320x320 on SPI (same init sequence as emu8's display_picocalc.cpp)
//   2. KBD   STM32 keyboard MCU at I2C 0x1F: firmware version, battery, live key codes
//   3. SD    microSD on its own SPI bus: mount + root listing
//   4. AUDIO a tone on the left speaker, then the right one
// Everything is also printed on Serial (115200), i.e. the module's own USB-C port.
//
// Keys once running:  1..5 = LCD SPI clock 20/40/53/60/80 MHz   A = replay the speaker test
//                     S = re-mount the SD card                    any key = shown on screen
//
// Pin map: Waveshare's ESP32-P4-WIFI6 pinout read in Pico orientation (pin 1 at the USB end;
// GNDs at 3/8/13/18 match a Pico exactly). Keyboard I2C 50/49 is also what the MicroPython
// PicoCalc port (Pedrocasf/PicoCalcESP32-P4-WIFI6-Micropython) verified on hardware.
//
// Build (isolated P4 toolchain, see emu8/src/shared/p4/README.md):
//   arduino-cli compile --fqbn esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=16M,ChipVariant=prev3 tools/picocalc-p4-bringup
//   arduino-cli upload  -p COMx --fqbn <same> tools/picocalc-p4-bringup

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include "hal/usb_serial_jtag_ll.h"
#include "esp_ldo_regulator.h"
#include "uifont.h"   // copy of emu8/src/shared/fonts/uifont.h (X11 misc-fixed, public domain)

//                       Pico pin   P4 GPIO
#define LCD_SCK    3   // GP10
#define LCD_MOSI   2   // GP11
#define LCD_MISO   8   // GP12  (also the module's I2C SCL net -- pull-up only, harmless)
#define LCD_CS     7   // GP13  (also the module's I2C SDA net)
#define LCD_DC    24   // GP14  USB-Serial-JTAG D- on the P4: the pad is released below
#define LCD_RST   25   // GP15  USB-Serial-JTAG D+
#define KBD_SDA   50   // GP6
#define KBD_SCL   49   // GP7
#define SD_MISO   48   // GP16
#define SD_CS     47   // GP17
#define SD_SCK    46   // GP18
#define SD_MOSI   33   // GP19
#define AUD_L     23   // GP26
#define AUD_R     22   // GP27

#define KBD_ADDR  0x1F
#define W 320
#define H 320

static SPIClass lcdSPI(FSPI);
static SPIClass sdSPI(HSPI);
static uint16_t *fb;                      // 320x320 RGB565, PSRAM
static uint32_t lcdHz = 40000000;
static bool kbdOk = false, sdOk = false;
static int  kbdVer = -1, batPct = -1;
static char sdInfo[8][40];
static int  sdLines = 0;
static char lastKey[40] = "(none yet)";
static uint32_t keyCount = 0, pushUs = 0;

// ------------------------------------------------------------------ LCD
static inline void lcdCmd(uint8_t c) {
  digitalWrite(LCD_DC, LOW);  lcdSPI.write(c);
  digitalWrite(LCD_DC, HIGH);
}
static inline void lcdArgs(const uint8_t *d, size_t n) { if (n) lcdSPI.writeBytes(d, n); }

static const uint8_t kInit[] = {           // emu8 display_picocalc.cpp kInit, RGB565
  0xE0, 15, 0x00,0x03,0x09,0x08,0x16,0x0A,0x3F,0x78,0x4C,0x09,0x0A,0x08,0x16,0x1A,0x0F,
  0xE1, 15, 0x00,0x16,0x19,0x03,0x0F,0x05,0x32,0x45,0x46,0x04,0x0E,0x0D,0x35,0x37,0x0F,
  0xC0,  2, 0x17,0x15,
  0xC1,  1, 0x41,
  0xC5,  3, 0x00,0x12,0x80,
  0x36,  1, 0x48,                          // MADCTL: MX | BGR
  0x3A,  1, 0x55,                          // RGB565
  0xB0,  1, 0x00,
  0xB1,  1, 0xA0,
  0x21,  0,                                // INVON
  0xB4,  1, 0x02,
  0xB6,  3, 0x02,0x02,0x3B,
  0xB7,  1, 0xC6,
  0xE9,  1, 0x00,
  0xF7,  4, 0xA9,0x51,0x2C,0x82,
};

// GPIO24/25 come out of reset owned by the USB-Serial-JTAG PHY, with its D+ pull-up on. Hand
// them to the GPIO matrix before the LCD driver uses them as DC/RST.
static void releaseUsbJtagPads() {
  USB_SERIAL_JTAG.conf0.pad_pull_override = 1;
  USB_SERIAL_JTAG.conf0.dp_pullup = 0;
  USB_SERIAL_JTAG.conf0.dm_pullup = 0;
  usb_serial_jtag_ll_phy_enable_pad(false);
}

static void lcdBegin() {
  releaseUsbJtagPads();
  pinMode(LCD_CS, OUTPUT);  digitalWrite(LCD_CS, HIGH);
  pinMode(LCD_DC, OUTPUT);  digitalWrite(LCD_DC, HIGH);
  pinMode(LCD_RST, OUTPUT); digitalWrite(LCD_RST, HIGH);
  lcdSPI.begin(LCD_SCK, LCD_MISO, LCD_MOSI, -1);
  lcdSPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));   // init at a safe clock
  delay(10); digitalWrite(LCD_RST, LOW); delay(10); digitalWrite(LCD_RST, HIGH); delay(200);
  digitalWrite(LCD_CS, LOW);                // held low for good, as in emu8
  for (size_t i = 0; i < sizeof(kInit); ) {
    uint8_t cmd = kInit[i++], n = kInit[i++];
    lcdCmd(cmd); lcdArgs(&kInit[i], n); i += n;
  }
  lcdCmd(0x11); delay(120);
  lcdCmd(0x29); delay(120);
  lcdSPI.endTransaction();
}

static void lcdPush() {
  uint32_t t0 = micros();
  lcdSPI.beginTransaction(SPISettings(lcdHz, MSBFIRST, SPI_MODE0));
  const uint8_t ca[4] = {0, 0, (W - 1) >> 8, (W - 1) & 0xFF};
  const uint8_t ra[4] = {0, 0, (H - 1) >> 8, (H - 1) & 0xFF};
  lcdCmd(0x2A); lcdArgs(ca, 4);
  lcdCmd(0x2B); lcdArgs(ra, 4);
  lcdCmd(0x2C);
  lcdSPI.writePixels(fb, W * H * 2);        // swaps to big-endian RGB565 on the wire
  lcdSPI.endTransaction();
  pushUs = micros() - t0;
}

// ------------------------------------------------------------------ framebuffer drawing
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static void fill(int x, int y, int w, int h, uint16_t c) {
  for (int j = y; j < y + h && j < H; j++)
    for (int i = x; i < x + w && i < W; i++)
      if (i >= 0 && j >= 0) fb[j * W + i] = c;
}
static void text(int x, int y, const char *s, uint16_t c) {   // 7x13
  for (; *s; s++, x += 7) {
    uint8_t ch = (uint8_t)*s;
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *g = &UIFONT_7X13[(ch - 0x20) * 13];
    for (int r = 0; r < 13; r++)
      for (int b = 0; b < 8; b++)
        if (g[r] & (0x80 >> b)) {
          int px = x + b, py = y + r;
          if (px >= 0 && px < W && py >= 0 && py < H) fb[py * W + px] = c;
        }
  }
}

static void drawScreen() {
  const uint16_t BG = rgb(0, 0, 40), WHITE = 0xFFFF, GREEN = rgb(0, 220, 0), RED = rgb(230, 0, 0),
                 YELLOW = rgb(255, 220, 0), GREY = rgb(160, 160, 160);
  fill(0, 0, W, H, BG);
  // colour bars + corner markers: verifies colour order, inversion and mirroring at a glance
  const uint16_t bars[5] = {rgb(255, 0, 0), rgb(0, 255, 0), rgb(0, 0, 255), WHITE, 0};
  const char *names[5] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK"};
  for (int i = 0; i < 5; i++) {
    fill(i * 64, 0, 64, 40, bars[i]);
    text(i * 64 + 4, 42, names[i], WHITE);
  }
  fill(0, H - 8, 8, 8, YELLOW);             // bottom-left
  fill(W - 8, H - 8, 8, 8, rgb(255, 0, 255)); // bottom-right
  char b[64];
  int y = 62;
  text(4, y, "emu8 PicoCalc ESP32-P4 bring-up", YELLOW); y += 16;
  snprintf(b, sizeof b, "LCD SPI %lu MHz  frame %lu.%lu ms", (unsigned long)(lcdHz / 1000000),
           (unsigned long)(pushUs / 1000), (unsigned long)(pushUs / 100 % 10));
  text(4, y, b, WHITE); y += 16;
  snprintf(b, sizeof b, "KBD  %s  fw 0x%02X  battery %d%%", kbdOk ? "OK" : "FAIL",
           kbdVer < 0 ? 0 : kbdVer, batPct & 0x7F);
  text(4, y, b, kbdOk ? GREEN : RED); y += 16;
  snprintf(b, sizeof b, "key #%lu: %s", (unsigned long)keyCount, lastKey);
  text(4, y, b, WHITE); y += 16;
  text(4, y, sdOk ? "SD   OK" : "SD   FAIL (S = retry)", sdOk ? GREEN : RED); y += 16;
  for (int i = 0; i < sdLines; i++, y += 14) text(12, y, sdInfo[i], GREY);
  y = H - 44;
  text(4, y, "1-5: SPI 20/40/53/60/80MHz", GREY); y += 14;
  text(4, y, "A: speaker test   S: SD re-mount", GREY);
  lcdPush();
}

// ------------------------------------------------------------------ keyboard (STM32 @ 0x1F)
static int kbdRead(uint8_t reg, uint8_t *out, int n) {
  Wire.beginTransmission(KBD_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return -1;
  delayMicroseconds(2000);                  // the STM32 needs a turnaround before the read
  if (Wire.requestFrom((uint8_t)KBD_ADDR, (uint8_t)n) != n) return -1;
  for (int i = 0; i < n; i++) out[i] = Wire.read();
  return n;
}
static void kbdWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(KBD_ADDR);
  Wire.write(reg | 0x80); Wire.write(v);
  Wire.endTransmission();
}
static bool kbdTry(int sda, int scl, uint32_t hz) {
  Wire.end();
  Wire.begin(sda, scl, hz);
  for (int i = 0; i < 5; i++) {
    Wire.beginTransmission(KBD_ADDR);
    uint8_t e = Wire.endTransmission();
    Serial.printf("KBD: sda %d scl %d %lu Hz -> err %u\n", sda, scl, (unsigned long)hz, e);
    if (e == 0) return true;
    delay(100);
  }
  return false;
}
static void kbdBegin() {
  // Idle levels with the internal pulls off: both should read 1 if the mainboard has pull-ups.
  pinMode(KBD_SDA, INPUT); pinMode(KBD_SCL, INPUT);
  delay(2);
  Serial.printf("KBD: idle SDA(%d)=%d SCL(%d)=%d (no pulls)\n", KBD_SDA, digitalRead(KBD_SDA),
                KBD_SCL, digitalRead(KBD_SCL));
  pinMode(KBD_SDA, INPUT_PULLDOWN); pinMode(KBD_SCL, INPUT_PULLDOWN);
  delay(2);
  Serial.printf("KBD: with pulldown SDA=%d SCL=%d\n", digitalRead(KBD_SDA), digitalRead(KBD_SCL));
  pinMode(KBD_SDA, INPUT); pinMode(KBD_SCL, INPUT);

  kbdOk = kbdTry(KBD_SDA, KBD_SCL, 10000) || kbdTry(KBD_SDA, KBD_SCL, 100000);
  if (!kbdOk && kbdTry(KBD_SCL, KBD_SDA, 10000)) {
    Serial.println("KBD: *** answers with SDA/SCL SWAPPED ***");
    kbdOk = true;
  }
  if (!kbdOk) {
    Wire.end();
    Wire.begin(KBD_SDA, KBD_SCL, 10000);
    Serial.print("KBD: bus scan:");
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
    }
    Serial.println();
    return;
  }
  uint8_t r[2];
  if (kbdRead(0x01, r, 2) == 2) kbdVer = r[1];
  if (kbdRead(0x0B, r, 2) == 2) batPct = r[1];
  kbdWrite(0x0A, 0xFF);                     // LCD backlight (emu8's KBD_REG_BK2)
  kbdWrite(0x05, 0xFF);                     // keyboard backlight
  Serial.printf("KBD: fw 0x%02X, battery %d%%\n", kbdVer, batPct & 0x7F);
}
static int kbdPoll() {                      // returns key code on press, -1 otherwise
  uint8_t r[2];
  if (kbdRead(0x09, r, 2) != 2 || r[1] == 0) return -1;
  snprintf(lastKey, sizeof lastKey, "state %u code 0x%02X '%c'", r[0], r[1],
           (r[1] >= 0x20 && r[1] < 0x7F) ? r[1] : '.');
  keyCount++;
  Serial.printf("KEY: %s\n", lastKey);
  return r[0] == 1 ? r[1] : -1;
}

// ------------------------------------------------------------------ SD
// GPIO39..48 (the IO domain VDDPST_5) are powered by the P4's internal LDO channel 4 on this
// module (schematic: VDDPST_5 = ESP_LDO_VO4). PicoCalc's SD lines land on 46/47/48, so the
// LDO must be on at 3.3 V before they work.
static void ioLdoOn() {
  static esp_ldo_channel_handle_t h = nullptr;
  if (h) return;
  esp_ldo_channel_config_t c = {};
  c.chan_id = 4;
  c.voltage_mv = 3300;
  esp_err_t e = esp_ldo_acquire_channel(&c, &h);
  Serial.printf("LDO4 3.3V: %s\n", esp_err_to_name(e));
}

static void sdBegin() {
  ioLdoOn();
  SD.end();
  sdSPI.end();
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, sdSPI, 20000000);
  sdLines = 0;
  if (!sdOk) { Serial.println("SD: mount FAILED"); return; }
  snprintf(sdInfo[sdLines++], 40, "%s card, %llu MB", SD.cardType() == CARD_SDHC ? "SDHC" : "SD",
           SD.cardSize() / (1024ULL * 1024ULL));
  File root = SD.open("/");
  for (File f = root.openNextFile(); f && sdLines < 8; f = root.openNextFile())
    snprintf(sdInfo[sdLines++], 40, "%s%s", f.isDirectory() ? "/" : "", f.name());
  Serial.println("SD: mounted");
  for (int i = 0; i < sdLines; i++) Serial.printf("  %s\n", sdInfo[i]);
}

// ------------------------------------------------------------------ audio
static void tone1(int pin, int hz, int ms) {
  ledcAttach(pin, hz, 8);
  ledcWrite(pin, 128);
  delay(ms);
  ledcWrite(pin, 0);
  ledcDetach(pin);
  pinMode(pin, OUTPUT); digitalWrite(pin, LOW);
}
static void audioTest() {
  Serial.println("AUDIO: left 440Hz, then right 660Hz");
  tone1(AUD_L, 440, 400);
  delay(150);
  tone1(AUD_R, 660, 400);
}

// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== emu8 PicoCalc ESP32-P4 bring-up ===");
  Serial.printf("chip rev %d, PSRAM %u KB free, flash %u MB\n", ESP.getChipRevision(),
                (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getFlashChipSize() >> 20));
  fb = (uint16_t *)ps_malloc(W * H * 2);
  if (!fb) { Serial.println("FATAL: no PSRAM for the framebuffer"); for (;;) delay(1000); }

  kbdBegin();                               // first: it switches the LCD backlight on
  lcdBegin();
  Serial.println("LCD: init sent");
  const uint16_t solid[4] = {rgb(255, 0, 0), rgb(0, 255, 0), rgb(0, 0, 255), 0xFFFF};
  for (int i = 0; i < 4; i++) { fill(0, 0, W, H, solid[i]); lcdPush(); delay(500); }
  Serial.printf("LCD: full frame at %lu MHz took %lu us\n", (unsigned long)(lcdHz / 1000000),
                (unsigned long)pushUs);
  sdBegin();
  drawScreen();
  audioTest();
}

void loop() {
  static uint32_t lastBat = 0;
  int k = kbdOk ? kbdPoll() : -1;
  bool redraw = (k >= 0);
  static const uint32_t speeds[5] = {20000000, 40000000, 53333333, 60000000, 80000000};
  if (k >= '1' && k <= '5') {
    lcdHz = speeds[k - '1'];
    Serial.printf("LCD: SPI -> %lu Hz\n", (unsigned long)lcdHz);
  } else if (k == 'a' || k == 'A') {
    audioTest();
  } else if (k == 's' || k == 'S') {
    sdBegin();
  }
  if (kbdOk && millis() - lastBat > 10000) {
    uint8_t r[2];
    if (kbdRead(0x0B, r, 2) == 2 && r[1] != batPct) { batPct = r[1]; redraw = true; }
    lastBat = millis();
  }
  if (redraw) {
    drawScreen();
    Serial.printf("LCD: frame %lu us at %lu MHz\n", (unsigned long)pushUs, (unsigned long)(lcdHz / 1000000));
  }
  delay(20);
}
