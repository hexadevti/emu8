// apple2esp32cyd - multi-platform retro emulator for the ESP32 Cheap Yellow Display
// (ESP32 + ILI9341 TFT via TFT_eSPI), SD-card storage.
//
// This header declares the shared state as `extern`; the single definitions live in
// globals.cpp. That lets the source files compile as separate translation units
// (src/<platform>/...), instead of the old single-.ino concatenation.

#pragma once

#include <Arduino.h>   // .cpp units (unlike .ino) don't get this implicitly
#include "FS.h"
#include <SPI.h>
#include <TFT_eSPI.h>
#include "SD.h"
#include <EEPROM.h>
#include "rom.h"
#include <string>
#include <bitset>
#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <thread>
#include <mutex>
#include <vector>
#include <deque>
#include <map>
#include <condition_variable>

#include "BLEDevice.h"

// BLE (currently unused; kept for reference)
static BLEUUID serviceUUID((uint16_t) 0x1812);
static BLEUUID    charUUID((uint16_t) 0x2a4d);
static bool doConnect = false;
static bool connected = false;
static bool doScan = false;
static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* myDevice;

extern size_t content_len;
extern File file;
extern bool opened;
extern String filelist;
extern int freeSpace;

// SD storage
#define FSTYPE SD
// VFS mount path the Arduino SD class uses (the default mountpoint passed to SD.begin() in
// FSSetup()). Raw opendir()/readdir() need the full VFS path, so file scans prefix their
// SD-relative directory with this. If FSSetup() ever passes a different mountpoint, update it.
#define SD_VFS_ROOT "/sd"
#define SD_SPI_HZ   20000000   // SD SPI clock (Hz). 20MHz >> the 4MHz default -> ~5x faster reads.
extern std::vector<std::string> hdFiles;
extern std::vector<std::string> diskFiles;
extern std::vector<std::string> c64Files;
extern std::vector<std::string> nesFiles;

// Board Pins
#define SD_SCK_PIN 18
#define SD_MISO_PIN 19
#define SD_MOSI_PIN 23
#define SD_CS_PIN 5
#define KEYBOARD_DATA_PIN 21
#define KEYBOARD_IRQ_PIN 22
#define ANALOG_X_PIN 4
#define ANALOG_Y_PIN 35
#define LED_PIN 17
#define DIGITAL_BUTTON12_PIN 34 // joystick buttons 0-3
#define SPEAKER_PIN 26

// Video Config
extern TFT_eSPI tft;
extern int margin_x;
extern int margin_y;
static const uint16_t screenWidth  = 240;
static const uint16_t screenHeight = 320;
extern std::mutex page_lock;
extern const uint16_t colors[8];     // defined in globals.cpp (after tft, for init order)
extern const uint16_t colors16[16];

extern uint16_t tx, ty; // touch coordinates

// keyboard scan buffer
extern unsigned short keyboard_data[3];
extern unsigned char keyboard_buf_indx, keyboard_mbyte;
extern boolean shift_enabled;
extern boolean ctrl_enabled;
extern char keymem;

// Joystick Config
#define JOY_MAX 1024
#define JOY_MID 512
#define JOY_MIN 0
#define EEPROM_SIZE 1024
extern int fnSelected;
extern int joystickCycles0;
extern int joystickCycles1;
extern int joystickCycles2;
extern int joystickCycles3;
extern int analogX;
extern int analogY;
extern int digital_button1;
extern bool pPb0;
extern bool pPb1;
extern bool pPb2;
extern bool pPb3;
extern int joyX;
extern int joyY;
extern int pJoyX;
extern int pJoyY;

// Feature flags / runtime state
extern bool running;
extern bool paused;
extern bool sound;
extern bool dacSound;
extern bool upscale;
extern bool smoothUpscale;
extern bool AppleIIe;
extern bool OptionsWindow;
extern bool DebugWindow;
extern bool ClearWindow;
extern bool initializedHdDisk;
extern bool HdDisk;
extern bool Fast1MhzSpeed;
extern bool joystick;
extern bool mouse;
extern bool diskAttached;
extern bool hdAttached;
extern bool serialVideoAttached;
extern bool serialKeyboardAttached;
extern bool videoColor;
extern bool wifiConnected;
extern uint8_t volume;

// Target system for the multi-platform emulator. Apple II is implemented; C64 and
// NES are placeholders selectable from the boot splash (see src/shared/video.cpp
// splashService and the dispatch in apple2esp32cyd.ino). Persisted in EEPROM.
enum Platform : uint8_t { PLATFORM_APPLE2 = 0, PLATFORM_C64 = 1, PLATFORM_NES = 2 };
extern uint8_t currentPlatform;

// Log Config
extern char buf[0xff];
extern int logLineCount;

// EPROM Config
#define HdDiskEEPROMaddress 0
#define IIpIIeEEPROMaddress 1
#define Fast1MhzSpeedEEPROMaddress 2
#define JoystickEEPROMaddress 3
#define VideoColorEEPROMaddress 4
#define SoundEEPROMaddress 5
#define VolumeEEPROMaddress 6
#define dacSoundEEPROMaddress 7
#define PlatformEEPROMaddress 8
#define C64AutoloadEEPROMaddress 9     // C64: auto-load the saved image on boot (0/1)
#define JoyPortEEPROMaddress 10        // C64: joystick port (1 or 2)
#define NewDeviceConfigEEPROMaddress 50
#define DiskFileNameEEPROMaddress 128
#define HdFileNameEEPROMaddress 256
#define C64FileNameEEPROMaddress 384   // C64: last-loaded .prg/.d64/.crt (for autoload)
#define NesFileNameEEPROMaddress 512   // NES: last-loaded .nes (auto-loaded on boot)
extern String selectedDiskFileName;
extern String selectedHdFileName;
extern String selectedC64FileName;
extern String selectedNesFileName;   // NES: currently-loaded ROM (settings file browser marker)
extern bool c64Autoload;          // C64: auto-load selectedC64FileName on boot
extern uint8_t joyPort;           // C64: joystick port (1 or 2)
extern String NewDeviceConfig;
extern byte selectedHdFile;
extern uint8_t firstShowFile;
extern uint8_t shownFile;

// Softswitches Config
extern bool Graphics_Text;
extern bool Page1_Page2;
extern bool DisplayFull_Split;
extern bool LoRes_HiRes;
extern bool Cols40_80;
extern bool lock_video;
extern bool IntCXRomOn_Off;
extern bool IntC8RomOn_Off;
extern bool AltCharSetOn_Off;
extern bool SlotC3RomOn_Off;
extern bool Store80On_Off;
extern bool Vertical_blankingOn_Off;
extern bool RAMReadOn_Off;
extern bool RAMWriteOn_Off;
extern bool AltZPOn_Off;
extern bool IOUDisOn_Off;
extern bool DHiResOn_Off;
extern bool IIEMemoryBankBankSelect1_2;
extern bool IIEMemoryBankReadRAM_ROM;
extern bool IIEMemoryBankWriteRAM_NoWrite;
extern uint8_t IIeExpansionCardBank;
extern bool MemoryBankBankSelect1_2;
extern bool MemoryBankReadRAM_ROM;
extern bool MemoryBankWriteRAM_NoWrite;
extern bool diskUnitNumber1_2;
extern bool DrivePhase0ON_OFF;
extern bool DrivePhase1ON_OFF;
extern bool DrivePhase2ON_OFF;
extern bool DrivePhase3ON_OFF;
extern bool FlagDO_PO;
extern bool DriveQ6H_L;
extern bool DriveQ7H_L;
extern bool DriveMotorON_OFF;

// Memory Config
extern unsigned char zp[0x200];
extern unsigned char auxzp[0x200];
extern unsigned char* ram;
extern unsigned char* auxram;
extern unsigned char* memoryBankSwitchedRAM1;
extern unsigned char* memoryBankSwitchedRAM2_1;
extern unsigned char* memoryBankSwitchedRAM2_2;
extern unsigned char* IIEAuxBankSwitchedRAM1;
extern unsigned char* IIEAuxBankSwitchedRAM2_1;
extern unsigned char* IIEAuxBankSwitchedRAM2_2;
extern unsigned char* IIEmemoryBankSwitchedRAM1;
extern unsigned char* IIEmemoryBankSwitchedRAM2_1;
extern unsigned char* IIEmemoryBankSwitchedRAM2_2;
extern unsigned char* menuScreen;
extern unsigned char* menuColor;
extern unsigned char sharedBigBuf[];   // C64 framebuffer / Apple main RAM (shared static 64K)

// Speaker Config
extern boolean speaker_state;

// Paddle / button timers
extern bool CgReset0;
extern bool CgReset1;
extern bool CgReset2;
extern bool CgReset3;
extern bool Cg0;
extern bool Cg1;
extern bool Cg2;
extern bool Cg3;
extern float timerpdl0;
extern float timerpdl1;
extern float timerpdl2;
extern float timerpdl3;
extern bool Pb0;
extern bool Pb1;
extern bool Pb2;
extern bool Pb3;

// Mouse
extern int16_t mouseX;
extern int16_t mouseY;
extern bool mouseButton;

#include "proto.h"   // cross-file function declarations (must come after the types above)
