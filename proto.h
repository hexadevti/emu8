// proto.h - prototypes for every cross-file (non-static) function. Included at the end
// of emu.h so all translation units (src/shared, src/apple2, ...) see them, now that
// the code is split into separate .cpp units instead of one concatenated sketch.
#pragma once

// log.cpp
void logSetup();
void printLog(String txt);
void printSequence(int seq);
void printProgress(size_t prg, size_t sz);
void printCPUStatus();
void PrintHex(uint8_t data[], int length);

// sd.cpp
void FSSetup();

// eprom.cpp
void epromSetup();
int writeStringToEEPROM(int addrOffset, const String &strToWrite);
int readStringFromEEPROM(int addrOffset, String *strToRead);
void saveEEPROM();
void saveConfig();

// interface.cpp
void setCursor(uint8_t x, uint8_t y);
void print(const char *txt, bool inverted = false, uint8_t color = 0xf0);
uint8_t getChar();
uint16_t getAddressValue();
void clearScreen();
void listFiles(bool downDirection);
void colorDemo();
void showHideOptionsWindow();
void showHideDebugWindow();
void debugScreenRender();
void stackdebug();
void printDebugLine();
void optionsScreenRender();

// speaker.cpp
void speakerSetup();
void speakerToggle();

// audio_amp.cpp (no-DAC boards / ESP32-S3): external I2S amp output the audio cores feed into
void ampBegin(int sampleRate);
void ampWriteDac8(const uint16_t *dacBuf, int n);   // 8-bit DAC (value in high byte) -> 16-bit amp
void ampWriteMono(const int16_t *mono, int n);      // 16-bit signed mono -> amp (Apple speaker)

// keyboardPs2.cpp
unsigned char keyboard_read();
void keyboardStrobe();
void keyboardSetup();
void keyboard_bit();

// joystick.cpp
void processJoystick(float speedAdjust);
void joystickSetup();
void applyPlatformInput();   // push joyX/joyY/Pb0-3 to the active core (shared: analog + USB)

// usbgamepad.cpp (JC4827W543 / ESP32-S3 only): USB-HID host SNES gamepad -> joyX/joyY/Pb0-3
void usbGamepadSetup();

#if BOARD_INPUT_USB
// usbkeyboard.cpp (JC4827W543 / ESP32-S3 only): USB-HID host keyboard -> active platform.
// keys/last are the 6-key rollover arrays of the current and previous boot report.
void usbKeyboardReport(uint8_t modifier, const uint8_t *keys, const uint8_t *last);
void usbKeyboardReset();   // release held keys on device disconnect
#endif

// touchkeyboard.cpp
void oskBuildLayout();
void oskSetup();
void oskRender();
bool oskActive();
bool oskDirty();   // keyboard needs a repaint (opened / key / shift) — lets heavy renders skip idle flushes
int oskRasterTop();
int oskRasterHeight();
bool touchRead(int16_t *sx, int16_t *sy);
void oskPoll();
void oskIgnoreCurrentTouch();

// osg.cpp - on-screen virtual gamepad for the touch-only consoles (NES/Atari/SMS) on the P4
#if BOARD_PANEL_DSI
void osgPoll();      // read multi-touch + drive joyX/joyY/Pb0-3 (called instead of oskPoll for those)
void osgRender();    // redraw the translucent controller into the overlay buffer
bool osgActive();    // true on NES/Atari/SMS when no menu is open
bool osgSupported(); // true on NES/Atari/SMS
int  gt911ReadPoints(int16_t *xs, int16_t *ys, int maxPts);   // multi-touch read (touchkeyboard.cpp)
#endif

// optionsui.cpp
void optionsUiSyncSelection();
void optionsUiRender();
void optionsUiPoll();
void optionsUiOpen();
void optionsUiMarkDirty();
void uiDirScanProgress(int count);    // draw a "Loading…" bar while a directory is scanned (+yield)

// video.cpp
void videoSetup();
void requestSplashOnNextBoot();       // arrange for the boot splash to show after the next reboot
int red(int color);
int green(int color);
int blue(int color);
void renderLoop(void *pvParameters);
void displayFlush();   // push the rendered frame to the panel (Arduino_GFX canvas); no-op on TFT_eSPI
void displaySetUiMode(bool ui);   // true=UI scaled to full panel, false=emulator video centered; no-op on TFT_eSPI
void displaySetVideoRect(int topLogical, int hLogical);   // active video content rect (for fill-screen); no-op on TFT_eSPI
void displaySetVideoFill(int leftLogical, int wLogical, bool stretch);   // horizontal content rect + stretch-to-full-width (NES); no-op on TFT_eSPI

// --- Apple II core ---
// cpu.cpp
void cpuReset();
void push16(unsigned short pushval);
void push8(unsigned char pushval);
unsigned short pull16();
unsigned char pull8();
void cpuLoop();
void setflags();

// apple2_roms.cpp - system ROMs loaded from /roms/apple2 on the SD card (no longer embedded)
bool apple2LoadRoms();            // load all 5 ROMs; false if any is missing or the wrong size
bool apple2EnsureHdRom();         // load just /roms/apple2/hd.bin (shared with the IIGS slot 7); cached
bool apple2RenderLoadWarning();   // renderLoop hook: draw the "ROMs not found" screen while halted
extern bool apple2RomLoadFailed;  // set when apple2LoadRoms() failed -> the 6502 stays halted

// memory.cpp
void memoryAlloc();
void showFreeMem();
unsigned char read8(unsigned short address);
void write8(unsigned short address, unsigned char value);
unsigned short read16(unsigned short address);
void write16(unsigned short address, unsigned short value);

// softswitches.cpp
char processSoftSwitches(ushort address, char value, bool Read_Write = true);
char readSoftSwitches(ushort address);
void writeSoftSwitches(ushort address, char value);

// disk.cpp
void diskSetup();
void saveTrackAsync(void *pvParameters);
void addPhase(uint8_t phase);
bool identifyDosProdos();
void getDiskFileInfo(fs::FS &fs);
void getTrack(fs::FS &fs, int track, bool force);
void saveImage(fs::FS &fs, int track);
void nextDiskFile();
void prevDiskFile();
void saveDiskFile();
void setDiskFile();
void apple2InsertDisk(const char *path);   // hot-swap the floppy by path (no reboot)
void loadDiskFilesSync();
void loadDiskAsync(void *pvParameters);
int getOffset(int track, int sector);
int getSectorOffset(int sector);
void trackRawDataEncode(int track);
unsigned char detranlateTable(unsigned char data);
void setSectorData(uint8_t sector, const std::vector<uint8_t> &data);
void setBlockData(int sector, const std::vector<uint8_t> &data);
char diskSoftSwitchesRead(ushort address);
void diskSoftSwitchesWrite(ushort address, char value);
char processSwitchc0e0(ushort address, char value);

// hd.cpp
void HDSetup();
void loadHdFilesSync();
void loadHdAsync(void *pvParameters);
void getBlockAsync(void *pvParameters);
void loadHD();
char HDSoftSwitchesRead(ushort address);
void HDSoftSwitchesWrite(ushort address, char value);
void getHdFileInfo(fs::FS &fs);
void nextHdFile();
void prevHdFile();
void saveHdFile();
void setHdFile();
char loadBlock(unsigned short address, unsigned short block);
ushort getBlockQty();
void getBlock(fs::FS &fs, ushort block);
void loadHDDir(fs::FS &fs, const char *dirname, uint8_t levels);

// languagecard.cpp
void languagecardWrite(ushort address, byte b);
char languagecardRead(ushort address);
char processSwitch(ushort address, byte b);

// mouse.cpp
char mouseSoftSwitchesRead(ushort address);
void mouseSoftSwitchesWrite(ushort address, byte value);
char ProcessC0xx(ushort address, byte b, bool Read_Write);

// C64 core entry points (src/c64/c64.cpp)
void c64Setup();
void c64Loop();
void c64RenderFrame();
void c64KeyMatrix(uint8_t row, uint8_t col, bool down);   // touch keyboard -> CIA1 matrix
void c64SetJoystick(uint8_t mask);                        // analog stick + fire -> CIA1 (port 1/2)
void c64Autostart();                  // boot-autoload the saved image (.crt now; .prg/.d64 at READY)
extern volatile bool c64AutoloadPending;   // a .prg/.d64 autoload waiting for the BASIC READY prompt

void c64FreeBtMem();                   // release unused BT controller DRAM (call before SD mount)

// NES core entry points (src/nes/nes.cpp), called by the platform dispatch
void nesSetup();
void nesLoop();
void nesRenderFrame();
void nesSetController(uint8_t buttons);   // joystick -> NES controller 1 (active-high bits)
bool nesRenderLoadWarning();              // startup ROM-skip warning overlay (true while showing)
void nesApuSetup();                       // NES APU audio (I2S DAC GPIO26), called from setup()
bool nesLoadSelected(const char *path);   // settings: load a .nes ROM + reset the NES
void nesScanFiles();                      // settings: rescan SD root for *.nes

// Atari 2600 core entry points (src/atari/atari.cpp), called by the platform dispatch
void atariSetup();
void atariLoop();
void atariRenderFrame();
void atariSetInput(uint8_t dirBits, bool fire, bool select, bool reset);  // stick + Fire/Select/Reset
bool atariRenderLoadWarning();            // startup ROM-skip warning overlay (true while showing)
void atariAudioSetup();                   // TIA audio (I2S DAC GPIO26), called from setup()

// Apple IIGS core entry points (src/iigs/iigs_boot.cpp), called by the platform dispatch
void iigsSetup();                         // alloc banks + embedded ROM 01 + reset 65C816
void iigsLoop();                          // run the CPU (from loop())
void iigsRenderText();                    // draw the 40-col text page to the LCD (from renderLoop)
void iigsLoadDisk(const char *path);      // settings: load a .dsk from SD into PSRAM + reboot to boot it
void iigsLoadHD(const char *path);        // settings: load a .po/.2mg/.hdv block image (slot 7) + reboot
bool atariLoadSelected(const char *path); // settings: load a .a26/.bin ROM + reset the 2600
void atariScanFiles();                    // settings: rescan SD root for *.a26 / *.bin

// MSX1 core entry points (src/msx/msx.cpp), called by the platform dispatch
void msxSetup();                          // alloc RAM/VRAM + BIOS (SD or C-BIOS) + reset Z80/VDP/PPI/PSG
void msxLoop();                           // run the machine frame-by-frame (from loop())
void msxRenderFrame();                    // VDP render + push the 256x192 picture (from renderLoop)
void msxPsgSetup();                       // AY-3-8910 audio task (I2S), called from setup() LAST
void msxKeyMatrix(uint8_t row, uint8_t col, bool down);  // on-screen/USB keyboard -> MSX key matrix
void msxSetInput(uint8_t joyMask);        // joystick -> MSX joystick (PSG port A, active-low)
bool msxLoadSelected(const char *path);   // settings: load a .rom cartridge + reset the MSX
void msxScanFiles();                      // settings: rescan SD root for *.rom / *.dsk
bool msxRenderLoadWarning();              // startup no-BIOS / C-BIOS note overlay (true while showing)
void loadMsxFilesSync();                  // scan SD root -> msxFiles (ROM/disk browser)

// SMS (Sega Master System) core entry points (src/sms/sms.cpp), called by the platform dispatch
void smsSetup();                          // alloc RAM/VRAM + reset Z80/VDP/PSG; auto-load saved ROM
void smsLoop();                           // run the machine frame-by-frame (from loop())
void smsRenderFrame();                    // VDP render + push the 256x192 picture (from renderLoop)
void smsPsgSetup();                       // SN76489 audio task (I2S), called from setup() LAST
void smsSetInput(uint8_t joyMask);        // joystick -> SMS controller port 1 (active-low)
void smsPauseButton();                    // SMS PAUSE button -> Z80 NMI (mapped to F11)
void smsHardReset();                      // SMS soft power-cycle: re-run the cartridge (mapped to F12)
bool smsLoadSelected(const char *path);   // settings: load a .sms/.bin ROM + reset the SMS
void smsScanFiles();                      // settings: rescan SD root for *.sms / *.bin
bool smsRenderLoadWarning();              // startup no-ROM warning overlay (true while showing)
void loadSmsFilesSync();                  // scan SD root -> smsFiles (ROM browser)

// PC-XT (Intel 8086 + CGA) core entry points (src/pcxt/pcxt.cpp), called by the platform dispatch
void pcxtSetup();                          // alloc 1MB RAM (PSRAM) + 64K video RAM; install BIOS + wire chipset
void pcxtLoop();                           // run the 8086 in chunks (from loop()); PIT drives IRQ0 real-time
bool pcxtRenderFrame();                    // CGA render; returns false (skipped) if the picture is unchanged
void pcxtForceRedraw();                    // force a repaint after a menu/screen clear
void pcxtSetInput(uint8_t joyMask);        // gamepad -> arrow/enter scancodes
void pcxtMouseInput(int dx, int dy, uint8_t buttons);  // USB mouse -> INT 33h mouse state
void pcxtKeyDown(uint8_t hidUsage, bool shift, bool ctrl, bool alt); // USB key -> XT make scancode
void pcxtKeyUp(uint8_t hidUsage);          // USB key -> XT break scancode
void pcxtHardReset();                      // soft reboot the PC (mapped to F12)
bool pcxtMountA(const char *path);         // settings: mount image into A: (floppy), no reboot
bool pcxtMountC(const char *path);         // settings: mount image into C: (hard disk), no reboot
bool pcxtMountAuto(const char *path);      // route by size: floppy(<=2.88MB)->A:, hard disk->C: (+re-POST)
void pcxtUnmount(int slot);                // settings: eject the disk in slot 0 (A:) or 2 (C:)
void pcxtScanFiles();                      // settings: rescan SD root for *.img/.ima/.dsk/.vhd
bool pcxtRenderLoadWarning();              // startup overlay (always false: BIOS shows its own POST)
void loadPcxtFilesSync();                  // scan SD root -> pcFiles (disk browser)

// tiny386 (Intel i386 + VGA) core entry points (src/tiny386/tiny386.cpp), called by the dispatch.
// Declared in src/tiny386/tiny386.h; mirrored here so the shared dispatch/render/UI can call them.
void tiny386Setup();
void tiny386Loop();
bool tiny386RenderFrame();
void tiny386ForceRedraw();
void tiny386SetInput(uint8_t joyMask);
void tiny386KeyDown(uint8_t hidUsage, bool shift, bool ctrl, bool alt);
void tiny386KeyUp(uint8_t hidUsage);
void tiny386MouseInput(int dx, int dy, uint8_t buttons);
void tiny386HardReset();
bool tiny386LoadSelected(const char *path);
bool tiny386MountA(const char *sel);   // A: floppy: live mount/eject (no reboot)
bool tiny386MountC(const char *sel);   // C: hard disk: re-attach + soft-reboot the PC (no device restart)
void tiny386ScanFiles();
void loadTiny386FilesSync();
bool tiny386RenderLoadWarning();

// SID sound (src/c64/c64_sid.cpp)
void sidSetup();                       // init the 3-voice synth + I2S DAC output task
void sidWrite(uint8_t reg, uint8_t val);
unsigned char sidRead(uint8_t reg);
#if defined(BOARD_DESKTOP)
int  sidDebugRegs(uint8_t *out, int max);              // copy the 25 SID registers ($D400-$D418 shadow)
void sidDebugVoice(int v, float *env, uint8_t *state); // live per-voice envelope (0..255) + ADSR state
#endif

// C64 program / disk loading (src/c64/c64_disk.cpp)
void loadC64FilesSync();              // scan the current browse dir -> c64Files (dirs + images)
void c64BrowseEnter(const char *path);// navigate into a subdirectory and rescan
void c64BrowseUp();                   // navigate to the parent directory and rescan
bool c64LoadPRG(const char *path);    // load a .prg into the running C64 (autorun if BASIC)
bool c64LoadSelected(const char *path); // menu dispatch: .prg loads&runs, .d64 mounts+runs "*"
void c64MountD64(const char *path);   // mount a .d64 as the virtual drive (device 8)
bool c64DiskMounted();                // is a .d64 currently mounted?
bool c64LoadCRT(const char *path);    // mount & launch a .crt cartridge (generic 8K/16K/Ultimax)
void c64CartUnmount();                // detach the cartridge (restore normal $8000/$A000)
void c64CartBankWrite(uint16_t addr, uint8_t val);   // cart banking register ($DE00-$DEFF)
bool c64CartIsEF();                                  // is an EasyFlash cart mounted?
unsigned char c64CartRamRead(uint16_t addr);         // EasyFlash RAM read  ($DF00-$DFFF)
void c64CartRamWrite(uint16_t addr, uint8_t val);    // EasyFlash RAM write ($DF00-$DFFF)
#if defined(BOARD_DESKTOP)
int c64CartCurBank();    // currently-mapped 8K bank (-1 = no cart) — desktop cart-access map
int c64CartBankCount();  // total bank count of the mounted cart (0 = none)
#endif
// KERNAL LOAD trap helpers (called from c64_cpu.cpp). Return 0 on success, 4 = not found.
int  c64D64LoadByName(const uint8_t *name, uint8_t len, bool useFileAddr,
                      uint16_t altAddr, uint16_t *startAddr, uint16_t *endAddr);
bool c64D64LoadDirectory(uint16_t altAddr, uint16_t *endAddr);

// optionsui.cpp joystick navigation
void optionsUiNav(int dir);
void optionsUiAdjust(int dir);
void optionsUiActivate();

// --- Globals defined inside a platform .cpp (declared here for cross-file access).
//     The matching definition's first declaration here also gives the const arrays
//     external linkage (same trick as colors[]). ---
extern bool clearScr;                  // interface.cpp
extern bool diskChanged;               // disk.cpp
extern int diskTrack;                  // disk.cpp
extern volatile bool trackPendingSave; // disk.cpp
extern unsigned char actualBlock[512]; // hd.cpp

// cpu.cpp registers / decode / debug state (used by the debugger + perf log)
extern const unsigned char* activeFlags;
extern const unsigned char flagsIIe[];
extern const unsigned char flagsIIplus[];
extern unsigned short PC, lastPC, argument_addr;
extern unsigned char STP, A, X, Y, SR, opcode, opflags;
extern long count, cycleCount;
extern uint32_t cpuCycleCount, lastCpuCycleCount, diffCpuCycleCount;
extern bool debug, debugPaused, debugStep;
extern uint16_t debugAddressBreak;
