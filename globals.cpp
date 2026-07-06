// globals.cpp - single definitions for the shared state declared extern in emu.h.
// Definition order matters: tft must be constructed before the colors[] tables, which
// call tft.color565() at static-init time.

#include "emu.h"

// Video (tft first!)
#if BOARD_HAS_TFT_ESPI
TFT_eSPI tft = TFT_eSPI();
#else
DisplayGFX tft;                 // Arduino_GFX backend (JC4827W543); color565() is pure math
#endif
const uint16_t colors[8] = {TFT_BLACK, TFT_GREEN, TFT_PURPLE, TFT_WHITE, TFT_BLACK, tft.color565(255, 20, 0), TFT_SKYBLUE, TFT_WHITE};
const uint16_t colors16[16] = {tft.color565(0, 0, 0), tft.color565(147, 11, 124), tft.color565(98, 76, 0), tft.color565(249, 86, 29),
                               tft.color565(0, 118, 12), tft.color565(126, 126, 126), tft.color565(67, 200, 0), tft.color565(220, 205, 22),
                               tft.color565(31, 53, 211), tft.color565(187, 54, 255), tft.color565(126, 126, 126), tft.color565(255, 129, 236),
                               tft.color565(7, 168, 224), tft.color565(157, 172, 255), tft.color565(93, 247, 132), tft.color565(255, 255, 255)};
int margin_x = 20;
int margin_y = 24;
std::mutex page_lock;
uint16_t tx = 0, ty = 0;

// FS / misc
size_t content_len;
File file;
bool opened = false;
String filelist = "";
int freeSpace = 0;
std::vector<std::string> hdFiles;
std::vector<std::string> diskFiles;
std::vector<std::string> c64Files;     // C64 .prg/.d64 images on SD
std::vector<std::string> nesFiles;     // NES .nes ROMs on SD
std::vector<std::string> atariFiles;   // Atari 2600 .a26/.bin ROMs on SD
std::vector<std::string> msxFiles;     // MSX1 .rom/.mx1/.dsk images on SD
std::vector<std::string> smsFiles;     // SMS .sms/.bin ROM images on SD
std::vector<std::string> pcFiles;      // PCXT .img/.ima/.dsk/.vhd disk images on SD
std::vector<std::string> tiny386Files; // tiny386 .img/.ima/.vhd/.hdd disk images on SD

// keyboard
unsigned short keyboard_data[3] = {0, 0, 0};
unsigned char keyboard_buf_indx = 0, keyboard_mbyte = 0;
boolean shift_enabled = false;
boolean ctrl_enabled = false;
char keymem = 0;

// joystick
int fnSelected = 0;
int joystickCycles0 = 0;
int joystickCycles1 = 0;
int joystickCycles2 = 0;
int joystickCycles3 = 0;
int analogX = 0;
int analogY = 0;
int digital_button1;
bool pPb0 = false;
bool pPb1 = false;
bool pPb2 = false;
bool pPb3 = false;
int joyX = 1;
int joyY = 1;
int pJoyX = 1;
int pJoyY = 1;

// feature flags / runtime state
bool running = true;
bool paused = false;
bool sound = true;
bool dacSound = false;
bool upscale = false;
bool smoothUpscale = false;
bool screenFill = false;   // JC4827W543: fill the 480x272 panel with the 320x240 video (keep 4:3)
uint8_t nesDisplaySkip = 3; // JC4827W543 NES: draw 1 of every N frames (core-0 display-skip frees the core-1 interpreter)
bool nesFast = false;       // NES: NORMAL (paced ~60fps) by default; true = FAST (uncapped)
float nesMeasuredMhz = 0.0f; // NES: measured 2A03 speed (fps-derived), updated in cpuLoop
bool AppleIIe = true;
bool OptionsWindow = false;
bool DebugWindow = false;
bool ClearWindow = false;
bool initializedHdDisk = false;
bool HdDisk = false;
bool Fast1MhzSpeed = true;
bool joystick = true;
bool mouse = true;
bool diskAttached = false;
bool hdAttached = false;
bool serialVideoAttached = false;
bool serialKeyboardAttached = false;
bool videoColor = true;
bool wifiConnected = false;
uint8_t volume = 0x40;
uint8_t currentPlatform = PLATFORM_APPLE2;

// log
char buf[0xff];
int logLineCount = 1;

// EEPROM-backed selections
String selectedDiskFileName;
String selectedHdFileName;
String selectedC64FileName;   // last C64 image highlighted/loaded
String selectedNesFileName;   // currently-loaded NES ROM (for the settings file browser)
String selectedAtariFileName; // currently-loaded Atari 2600 ROM (for the settings file browser)
String selectedMsxFileName;   // currently-loaded MSX1 .rom cartridge (for the settings file browser)
bool msxFast = false;         // MSX: NORMAL (paced to 3.58 MHz) by default; true = FAST (uncapped)
float msxMeasuredMhz = 0.0f;  // MSX: measured uncapped Z80 speed from the boot benchmark
String selectedSmsFileName;   // currently-loaded SMS .sms/.bin ROM (for the settings file browser)
bool smsFast = false;         // SMS: NORMAL (paced to 3.58 MHz) by default; true = FAST (uncapped)
float smsMeasuredMhz = 0.0f;  // SMS: measured uncapped Z80 speed from the boot benchmark
String selectedPcFileName;    // PCXT A: floppy image (for the settings file browser)
String selectedPcHdFileName;  // PCXT C: hard-disk image (auto-mounted on boot)
bool pcFast = false;          // PCXT: reserved speed flag
float pcMeasuredMhz = 0.0f;   // PCXT: measured 8086 equivalent speed from the boot benchmark
String selectedTiny386FileName;   // tiny386: C: hard-disk image (hda) (settings file browser)
String selectedTiny386FileNameA;  // tiny386: A: floppy image (fda) (settings file browser)
float tiny386MeasuredMhz = 0.0f;  // tiny386: measured i386 throughput (boot benchmark)
float appleMeasuredMhz = 0.0f; // Apple II: live measured 6502 speed (updated in cpuLoop)
float appleClockMhz = 1.0f;    // Apple II: target clock when throttled (1.0 = stock 1 MHz; UI-adjustable on desktop)
volatile int  g_pcSpkFreq = 0;     // PCXT PC-speaker: PIT ch2 frequency (Hz)
volatile bool g_pcSpkOn   = false; // PCXT PC-speaker: gate+data enabled (port 0x61)
bool c64Autoload = false;     // C64: auto-load the saved image on boot
uint8_t joyPort = 2;          // C64: joystick port (1 or 2)
volatile bool c64AutoloadPending = false;  // .prg/.d64 autoload deferred to the BASIC READY trap
String NewDeviceConfig;
byte selectedHdFile;
uint8_t firstShowFile = 0;
uint8_t shownFile = 0xff;

// softswitches
bool Graphics_Text = false;
bool Page1_Page2 = true;
bool DisplayFull_Split = true;
bool LoRes_HiRes = true;
bool Cols40_80 = true;
bool lock_video = false;
bool IntCXRomOn_Off = false;
bool IntC8RomOn_Off = false;
bool AltCharSetOn_Off = false;
bool SlotC3RomOn_Off = false;
bool Store80On_Off = false;
bool Vertical_blankingOn_Off = false;
bool RAMReadOn_Off = false;
bool RAMWriteOn_Off = false;
bool AltZPOn_Off = false;
bool IOUDisOn_Off = true;
bool DHiResOn_Off = false;
bool IIEMemoryBankBankSelect1_2 = true;
bool IIEMemoryBankReadRAM_ROM = false;
bool IIEMemoryBankWriteRAM_NoWrite = false;
uint8_t IIeExpansionCardBank = 0;
bool MemoryBankBankSelect1_2 = true;
bool MemoryBankReadRAM_ROM = false;
bool MemoryBankWriteRAM_NoWrite = false;
bool diskUnitNumber1_2 = true;
bool DrivePhase0ON_OFF;
bool DrivePhase1ON_OFF;
bool DrivePhase2ON_OFF;
bool DrivePhase3ON_OFF;
bool FlagDO_PO;
bool DriveQ6H_L;
bool DriveQ7H_L;
bool DriveMotorON_OFF;

// memory
unsigned char zp[0x200];
unsigned char auxzp[0x200];
unsigned char* ram;
unsigned char* auxram;
unsigned char* memoryBankSwitchedRAM1;
unsigned char* memoryBankSwitchedRAM2_1;
unsigned char* memoryBankSwitchedRAM2_2;
unsigned char* IIEAuxBankSwitchedRAM1;
unsigned char* IIEAuxBankSwitchedRAM2_1;
unsigned char* IIEAuxBankSwitchedRAM2_2;
unsigned char* IIEmemoryBankSwitchedRAM1;
unsigned char* IIEmemoryBankSwitchedRAM2_1;
unsigned char* IIEmemoryBankSwitchedRAM2_2;
unsigned char* menuScreen;
unsigned char* menuColor;

// One STATIC 64K buffer shared by the two (mutually exclusive) platforms: the C64 VIC
// framebuffer (two 32016-byte halves) AND the Apple II main RAM (0xC000). Static (not malloc)
// so the framebuffer is deterministic (no heap-fragmentation failure) without permanently
// stealing heap from whichever platform isn't using it. 320*100+16 = 32016 per C64 half.
unsigned char sharedBigBuf[2 * (320 * 100 + 16)];

// speaker
boolean speaker_state = false;

// paddle / button timers
bool CgReset0 = false;
bool CgReset1 = false;
bool CgReset2 = false;
bool CgReset3 = false;
bool Cg0 = false;
bool Cg1 = false;
bool Cg2 = false;
bool Cg3 = false;
float timerpdl0 = 0;
float timerpdl1 = 0;
float timerpdl2 = 0;
float timerpdl3 = 0;
bool Pb0 = false;
bool Pb1 = false;
bool Pb2 = false;
bool Pb3 = false;

// mouse
int16_t mouseX = 0;
int16_t mouseY = 0;
bool mouseButton = false;
