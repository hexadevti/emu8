// emu8 - multi-platform retro emulator for the ESP32 Cheap Yellow Display
// (ESP32 + ILI9341 TFT via TFT_eSPI), SD-card storage.
//
// This header declares the shared state as `extern`; the single definitions live in
// globals.cpp. That lets the source files compile as separate translation units
// (src/<platform>/...), instead of the old single-.ino concatenation.

#pragma once

#include <Arduino.h>   // .cpp units (unlike .ino) don't get this implicitly
#include "board.h"     // board selection (CYD vs JC4827W543) + capability macros + pin map
#include "FS.h"
#include <SPI.h>
#if BOARD_HAS_TFT_ESPI
  #include <TFT_eSPI.h>
#elif defined(BOARD_DESKTOP)
  #include "src/desktop/display_sdl.h"  // DisplayGFX: TFT_eSPI-compatible SDL2 backend (desktop)
#elif defined(BOARD_PICOCALC)
  #include "src/picocalc/display_picocalc.h"  // DisplayGFX: ST7365P 320x320 over spi1+DMA (PicoCalc)
#else
  #include "src/shared/display_gfx.h"   // DisplayGFX: TFT_eSPI-compatible Arduino_GFX backend
#endif
#if BOARD_SD_MMC
#include "SD_MMC.h"     // ESP32-P4: microSD over the SDMMC (SDIO) peripheral, not SPI
#elif defined(BOARD_PICOCALC)
#include <SDFS.h>       // arduino-pico: SDFS is the SPI SD filesystem (its SD.h wrapper cannot
                        // take a bus/speed, and has no cardType()/cardSize())
#else
#include "SD.h"
#endif
#if defined(BOARD_PICOCALC)
// arduino-pico inherits FILE_READ / FILE_WRITE from SdFat, where they are integer open() flags
// (O_RDONLY and O_RDWR|O_CREAT|O_AT_END). But fs::FS::open() here takes a MODE STRING, and
// O_RDONLY is 0 -- so every open(path, FILE_READ) in the shared core code was quietly handing it
// a null mode pointer. The card mounted, reported its size, and then "found" nothing on it: not
// one ROM, disk or cart image would open. Redefine both to the fopen-style strings the rest of
// the tree already means by these names (ESP32 SD.h and the desktop shim both spell them "r"/"w").
// This has to come AFTER the SD include above, which is what drags the SdFat definitions in.
#undef  FILE_READ
#define FILE_READ  "r"
#undef  FILE_WRITE
#define FILE_WRITE "w"
#endif

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

#if BOARD_HAS_BLE
#include "BLEDevice.h"

// BLE (currently unused; kept for reference)
static BLEUUID serviceUUID((uint16_t) 0x1812);
static BLEUUID    charUUID((uint16_t) 0x2a4d);
static bool doConnect = false;
static bool connected = false;
static bool doScan = false;
static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* myDevice;
#endif

extern size_t content_len;
extern File file;
extern bool opened;
extern String filelist;
extern int freeSpace;

// SD storage
#if BOARD_SD_MMC
#define FSTYPE SD_MMC
#elif defined(BOARD_PICOCALC)
#define FSTYPE SDFS
#else
#define FSTYPE SD
#endif
// VFS mount path the Arduino SD class uses (the default mountpoint passed to SD.begin() in
// FSSetup()). Raw opendir()/readdir() need the full VFS path, so file scans prefix their
// SD-relative directory with this. If FSSetup() ever passes a different mountpoint, update it.
#if defined(BOARD_DESKTOP)
#define SD_VFS_ROOT "./sdcard"   // desktop: the emulated SD card is this host dir (see sd_host.cpp).
#elif defined(BOARD_PICOCALC)
#define SD_VFS_ROOT ""           // arduino-pico has no global VFS: SDFS paths ARE SD-relative.
#else
#define SD_VFS_ROOT "/sd"
#endif
#define SD_SPI_HZ   20000000   // SD SPI clock (Hz). 20MHz >> the 4MHz default -> ~5x faster reads.
#if !BOARD_SD_MMC && !defined(BOARD_PICOCALC)
extern SPIClass hspi;          // SD HSPI bus (shared with the XPT2046 touch on the JC4827W543).
                               // Absent on the P4 (SD is SD_MMC, touch is I2C — no shared SPI bus)
                               // and on the PicoCalc (panel and card are on different buses).
#endif
// Serializes XPT2046 touch reads against SD-card operations: both use the same HSPI bus, and the
// SPIClass mutex only protects ONE transaction, not the SD library's multi-transaction
// (command -> data) sequences. A touch read sneaking in mid-SD-op corrupted the card and the USB
// host. This mutex makes each touch read and each SD read/write atomic w.r.t. each other.
extern SemaphoreHandle_t gBusLock;
static inline void busTake() { if (gBusLock) xSemaphoreTake(gBusLock, portMAX_DELAY); }
static inline void busGive() { if (gBusLock) xSemaphoreGive(gBusLock); }
extern std::vector<std::string> hdFiles;
extern std::vector<std::string> diskFiles;
extern std::vector<std::string> c64Files;
extern std::vector<std::string> nesFiles;
extern std::vector<std::string> atariFiles;   // Atari 2600 .a26/.bin ROMs on SD
extern std::vector<std::string> msxFiles;      // MSX1 .rom/.mx1/.dsk images on SD
extern std::vector<std::string> smsFiles;      // SMS .sms/.bin ROM images on SD
extern std::vector<std::string> colecoFiles;   // ColecoVision .col/.rom/.bin cartridges on SD
extern std::vector<std::string> zxFiles;       // ZX Spectrum .sna/.z80/.tap/.tzx on SD
extern std::vector<std::string> pcFiles;       // PCXT .img/.ima/.dsk/.vhd disk images on SD

// Board Pins, capability macros, and the display backend selection now live in board.h
// (included above). Pins: SD_*, KEYBOARD_*, ANALOG_*, LED_PIN, DIGITAL_BUTTON12_PIN, SPEAKER_PIN.

// Disk-activity LED: the Apple II disk/HD code blinks LED_PIN during I/O. Boards without an
// LED define LED_PIN = -1; route the writes through here so they are a safe no-op there
// (a raw digitalWrite(-1, ...) spams gpio_set_level() "gpio_num error" during every disk access).
static inline void diskLed(uint8_t level) { if (LED_PIN >= 0) digitalWrite(LED_PIN, level); }

// Video Config
#if BOARD_HAS_TFT_ESPI
extern TFT_eSPI tft;
#else
extern DisplayGFX tft;
#endif
extern int margin_x;
extern int margin_y;
static const uint16_t screenWidth  = 240;
static const uint16_t screenHeight = 320;
#if defined(BOARD_PICOCALC)
extern PicoMutex page_lock;   // arm-none-eabi libstdc++ has no std::mutex; see pico_shim.h
#else
extern std::mutex page_lock;
#endif
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
#define EEPROM_SIZE 1792   // ... + the ColecoVision cartridge name (1280) + the ZX Spectrum file name (1408) + the Apple IIe's own disk/HD names (1536, 1664)
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
extern bool screenFill;   // JC4827W543: scale the emulator video to fill the panel (keep 4:3 aspect)
extern uint8_t nesDisplaySkip;  // JC4827W543 NES: draw 1 of every N emulated frames (1=every frame; 2-3 trade picture smoothness for game speed)
extern bool nesFast;            // NES: false = NORMAL (paced ~60fps/1.79MHz), true = FAST (uncapped)
// The Apple II guest's OWN clock, in microseconds, published by the pacer in src/apple2/cpu.cpp
// and consumed by src/shared/speaker.cpp. Real time is the wrong timebase for $C030: the host
// cannot quite sustain 1.02MHz, so real time records how fast the EMULATOR ran, not what the
// guest asked for. 0 means "no Apple II pacing this instruction" -- the speaker then falls back
// to the hardware timer, which is what every other core wants.
extern volatile uint32_t appleGuestUs;

extern float nesMeasuredMhz;    // NES: measured 2A03 speed (derived from fps x cycles-per-frame)
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
// splashService and the dispatch in emu8.ino). Persisted in EEPROM -- all but PLATFORM_SDMANAGER,
// which is not an emulator: the SD card file manager as a boot mode of its own (sdserial.cpp), so it
// has the whole heap to itself. It is entered for one boot only (sdManagerBootRequested) and never
// saved, so leaving it lands back on the system the user was running.
enum Platform : uint8_t { PLATFORM_APPLE2 = 0, PLATFORM_C64 = 1, PLATFORM_NES = 2, PLATFORM_ATARI = 3, PLATFORM_IIGS = 4, PLATFORM_MSX = 5, PLATFORM_SMS = 6, PLATFORM_PCXT = 7, PLATFORM_SDMANAGER = 8, PLATFORM_COLECO = 9, PLATFORM_ZX = 10 };
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
#define ScreenFillEEPROMaddress 11     // JC4827W543: fill-screen video upscale (char: 1 = fill)
#define NesDisplaySkipEEPROMaddress 12 // JC4827W543 NES: display frame-skip 1..3 (char)
#define MsxSpeedEEPROMaddress 13       // MSX: 1 = FAST (uncapped) / 0 = NORMAL (paced to 3.58 MHz)
#define SmsSpeedEEPROMaddress 14       // SMS: 1 = FAST (uncapped) / 0 = NORMAL (paced to 3.58 MHz)
#define PcxtSpeedEEPROMaddress 15      // PCXT: reserved speed flag (currently always uncapped)
#define ColecoSpeedEEPROMaddress 16    // Coleco: 1 = FAST (uncapped) / 0 = NORMAL (paced to 3.58 MHz)
#define NesSpeedEEPROMaddress 17       // NES: 1 = FAST (uncapped) / 0 = NORMAL (paced ~60fps/1.79MHz)
#define ZxSpeedEEPROMaddress 21        // ZX Spectrum: 1 = FAST (uncapped) / 0 = NORMAL (paced to 3.5 MHz)
// The Apple IIe keeps its own copy of the Apple-only settings (DEVICE, SPEED, the disk and HD
// images); the addresses above without "IIe" are the II+'s (and the IIGS's). See eprom.cpp.
#define IIeHdDiskEEPROMaddress 18
#define IIeFast1MhzSpeedEEPROMaddress 19
#define IIeConfigMarkerEEPROMaddress 20  // IIE_CONFIG_MARKER once the IIe set has been seeded
#define NewDeviceConfigEEPROMaddress 50
#define DiskFileNameEEPROMaddress 128
#define HdFileNameEEPROMaddress 256
#define C64FileNameEEPROMaddress 384   // C64: last-loaded .prg/.d64/.crt (for autoload)
#define NesFileNameEEPROMaddress 512   // NES: last-loaded .nes (auto-loaded on boot)
#define AtariFileNameEEPROMaddress 640 // Atari: last-loaded .a26/.bin (auto-loaded on boot)
#define MsxFileNameEEPROMaddress 768   // MSX: last-loaded .rom cartridge (auto-loaded on boot)
#define SmsFileNameEEPROMaddress 896   // SMS: last-loaded .sms/.bin ROM (auto-loaded on boot)
#define PcxtFileNameEEPROMaddress 1024 // PCXT: last-mounted A: floppy image (auto-mounted on boot)
#define PcxtHdFileNameEEPROMaddress 1152 // PCXT: last-mounted C: hard-disk image (auto-mounted on boot)
#define ColecoFileNameEEPROMaddress 1280 // Coleco: last-loaded .col/.rom cartridge (auto-loaded on boot)
#define ZxFileNameEEPROMaddress 1408   // ZX Spectrum: last-loaded .sna/.z80/.tap/.tzx (auto-loaded on boot)
#define IIeDiskFileNameEEPROMaddress 1536  // Apple IIe: last .dsk (the II+'s is DiskFileNameEEPROMaddress)
#define IIeHdFileNameEEPROMaddress 1664    // Apple IIe: last HD image (the II+'s is HdFileNameEEPROMaddress)
extern String selectedDiskFileName;
extern String selectedHdFileName;
extern String selectedC64FileName;
extern String selectedNesFileName;   // NES: currently-loaded ROM (settings file browser marker)
extern String selectedAtariFileName; // Atari: currently-loaded ROM (settings file browser marker)
extern String selectedMsxFileName;   // MSX: currently-loaded .rom cartridge (settings file browser marker)
extern bool msxFast;                 // MSX: true = run uncapped (FAST), false = pace to real 3.58 MHz
extern float msxMeasuredMhz;         // MSX: measured uncapped Z80 speed (one-time boot benchmark)
extern String selectedSmsFileName;   // SMS: currently-loaded .sms/.bin ROM (settings file browser marker)
extern bool smsFast;                 // SMS: true = run uncapped (FAST), false = pace to real 3.58 MHz
extern float smsMeasuredMhz;         // SMS: measured uncapped Z80 speed (one-time boot benchmark)
extern String selectedColecoFileName;// Coleco: currently-loaded .col/.rom cartridge (settings file browser marker)
extern bool colecoFast;              // Coleco: true = run uncapped (FAST), false = pace to real 3.58 MHz
extern float colecoMeasuredMhz;      // Coleco: measured uncapped Z80 speed (one-time boot benchmark)
extern String selectedZxFileName;    // ZX Spectrum: currently-loaded snapshot / tape (settings file browser marker)
extern bool zxFast;                  // ZX Spectrum: true = run uncapped (FAST), false = pace to real 3.5 MHz
extern float zxMeasuredMhz;          // ZX Spectrum: measured uncapped Z80 speed (one-time boot benchmark)
extern String selectedPcFileName;    // PCXT: A: floppy image (settings file browser marker)
extern String selectedPcHdFileName;  // PCXT: C: hard-disk image (auto-mounted on boot)
extern bool pcFast;                  // PCXT: reserved (8086 always runs uncapped for now)
extern float pcMeasuredMhz;          // PCXT: measured 8086 equivalent speed (one-time boot benchmark)
extern float appleMeasuredMhz;       // Apple II: live measured 6502 speed (updated in cpuLoop)
extern float appleClockMhz;          // Apple II: target clock when throttled (1.0 = stock 1 MHz)
extern volatile int  g_pcSpkFreq;    // PCXT PC-speaker: PIT ch2 frequency (Hz), read by the audio ISR
extern volatile bool g_pcSpkOn;      // PCXT PC-speaker: gate+data enabled (port 0x61), read by the audio ISR
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
extern unsigned char sharedBigBuf[2 * (320 * 100 + 16)];   // C64 framebuffer / Apple main RAM
                                       // (shared static 64K). Sized here, not just declared, so
                                       // memoryAlloc() can static_assert against its tail.

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
