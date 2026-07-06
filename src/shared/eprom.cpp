#include "../../emu.h"

void epromSetup() {
    
  EEPROM.begin(EEPROM_SIZE);
  //delay(3000);
  readStringFromEEPROM(NewDeviceConfigEEPROMaddress, &NewDeviceConfig);
  sprintf(buf, "EEPROM NewDeviceConfig value: %s", NewDeviceConfig.c_str());
  printLog(buf);

  
  if (NewDeviceConfig != "ok")
  {
    
    Serial.println("New Device");
    int e1 = EEPROM.writeBool(HdDiskEEPROMaddress, false);
    int e2 = EEPROM.writeBool(IIpIIeEEPROMaddress, false);
    int e3 = EEPROM.writeBool(Fast1MhzSpeedEEPROMaddress, true);
    int e4 = EEPROM.writeBool(JoystickEEPROMaddress, true);
    int e8 = EEPROM.writeBool(VideoColorEEPROMaddress, true);
    int e9 = EEPROM.writeBool(SoundEEPROMaddress, true);
    int e10 = EEPROM.writeChar(VolumeEEPROMaddress, 0x40);
    int e11 = EEPROM.writeBool(dacSoundEEPROMaddress, false);
    int e12 = EEPROM.writeChar(PlatformEEPROMaddress, PLATFORM_APPLE2);
    int e7 = writeStringToEEPROM(NewDeviceConfigEEPROMaddress, "ok");
    int e5 = writeStringToEEPROM(HdFileNameEEPROMaddress, "/");
    int e6 = writeStringToEEPROM(DiskFileNameEEPROMaddress, "/karateka.dsk");
    EEPROM.commit();
  }
  
  HdDisk = EEPROM.readBool(HdDiskEEPROMaddress);
  AppleIIe = EEPROM.readBool(IIpIIeEEPROMaddress);
  Fast1MhzSpeed = EEPROM.readBool(Fast1MhzSpeedEEPROMaddress);
  joystick = EEPROM.readBool(JoystickEEPROMaddress);
  videoColor = EEPROM.readBool(VideoColorEEPROMaddress);
  sound = EEPROM.readBool(SoundEEPROMaddress);
  dacSound = EEPROM.readBool(dacSoundEEPROMaddress);
  volume = EEPROM.readChar(VolumeEEPROMaddress);
  currentPlatform = EEPROM.readChar(PlatformEEPROMaddress);
  if (currentPlatform > PLATFORM_TINY386) currentPlatform = PLATFORM_APPLE2;  // unset/garbage -> default

#if defined(BOARD_DESKTOP)
  // Desktop debug: EMU_PLATFORM picks the platform (and esp_reset_reason() then skips the splash).
  if (const char *p = getenv("EMU_PLATFORM")) {
    String s(p); s.toLowerCase();
    if      (s == "apple2" || s == "apple") currentPlatform = PLATFORM_APPLE2;
    else if (s == "c64")                     currentPlatform = PLATFORM_C64;
    else if (s == "nes")                     currentPlatform = PLATFORM_NES;
    else if (s == "atari")                   currentPlatform = PLATFORM_ATARI;
    else if (s == "iigs")                    currentPlatform = PLATFORM_IIGS;
    else if (s == "msx")                     currentPlatform = PLATFORM_MSX;
    else if (s == "sms")                     currentPlatform = PLATFORM_SMS;
    else if (s == "pcxt" || s == "pc")       currentPlatform = PLATFORM_PCXT;
    else if (s == "tiny386" || s == "386")   currentPlatform = PLATFORM_TINY386;
  }
#endif

  // C64 settings (validated so old/uninitialised EEPROM doesn't enable surprises).
  c64Autoload = (EEPROM.readChar(C64AutoloadEEPROMaddress) == 1);
  joyPort = EEPROM.readChar(JoyPortEEPROMaddress);
  if (joyPort != 1 && joyPort != 2) joyPort = 2;
  // ==1 test (not readBool) so an uninitialised 0xFF on older devices reads as OFF, not ON.
  screenFill = (EEPROM.readChar(ScreenFillEEPROMaddress) == 1);
  { char s = EEPROM.readChar(NesDisplaySkipEEPROMaddress); nesDisplaySkip = (s >= 1 && s <= 3) ? (uint8_t)s : 3; }  // default 3; fresh EEPROM (0xFF) -> 3
  msxFast = (EEPROM.readChar(MsxSpeedEEPROMaddress) == 1);   // ==1 so fresh EEPROM (0xFF) -> NORMAL
  nesFast = (EEPROM.readChar(NesSpeedEEPROMaddress) == 1);   // ==1 so fresh EEPROM (0xFF) -> NORMAL
  smsFast = (EEPROM.readChar(SmsSpeedEEPROMaddress) == 1);   // ==1 so fresh EEPROM (0xFF) -> NORMAL
  readStringFromEEPROM(C64FileNameEEPROMaddress, &selectedC64FileName);
  if (selectedC64FileName.length() == 0 || selectedC64FileName.length() > 120 ||
      selectedC64FileName[0] != '/') {
    selectedC64FileName = "";
    c64Autoload = false;
  }

  // NES: last-loaded ROM, auto-loaded on boot (validated; garbage -> none).
  readStringFromEEPROM(NesFileNameEEPROMaddress, &selectedNesFileName);
  if (selectedNesFileName.length() == 0 || selectedNesFileName.length() > 120 ||
      selectedNesFileName[0] != '/')
    selectedNesFileName = "";

  // Atari 2600: last-loaded ROM, auto-loaded on boot (validated; garbage -> none).
  readStringFromEEPROM(AtariFileNameEEPROMaddress, &selectedAtariFileName);
  if (selectedAtariFileName.length() == 0 || selectedAtariFileName.length() > 120 ||
      selectedAtariFileName[0] != '/')
    selectedAtariFileName = "";

  // MSX: last-loaded .rom cartridge, auto-loaded on boot (validated; garbage -> none).
  readStringFromEEPROM(MsxFileNameEEPROMaddress, &selectedMsxFileName);
  if (selectedMsxFileName.length() == 0 || selectedMsxFileName.length() > 120 ||
      selectedMsxFileName[0] != '/')
    selectedMsxFileName = "";

  // SMS: last-loaded .sms/.bin ROM, auto-loaded on boot (validated; garbage -> none).
  readStringFromEEPROM(SmsFileNameEEPROMaddress, &selectedSmsFileName);
  if (selectedSmsFileName.length() == 0 || selectedSmsFileName.length() > 120 ||
      selectedSmsFileName[0] != '/')
    selectedSmsFileName = "";

  // PCXT: last-mounted A: floppy + C: hard-disk images, auto-mounted on boot (validated; garbage -> none).
  readStringFromEEPROM(PcxtFileNameEEPROMaddress, &selectedPcFileName);
  if (selectedPcFileName.length() == 0 || selectedPcFileName.length() > 120 ||
      selectedPcFileName[0] != '/')
    selectedPcFileName = "";
  readStringFromEEPROM(PcxtHdFileNameEEPROMaddress, &selectedPcHdFileName);
  if (selectedPcHdFileName.length() == 0 || selectedPcHdFileName.length() > 120 ||
      selectedPcHdFileName[0] != '/')
    selectedPcHdFileName = "";

  readStringFromEEPROM(Tiny386FileNameEEPROMaddress, &selectedTiny386FileName);
  if (selectedTiny386FileName.length() == 0 || selectedTiny386FileName.length() > 120 ||
      selectedTiny386FileName[0] != '/')
    selectedTiny386FileName = "";

  readStringFromEEPROM(Tiny386FileNameAEEPROMaddress, &selectedTiny386FileNameA);
  if (selectedTiny386FileNameA.length() == 0 || selectedTiny386FileNameA.length() > 120 ||
      selectedTiny386FileNameA[0] != '/')
    selectedTiny386FileNameA = "";


  if (HdDisk) {
    int size = readStringFromEEPROM(HdFileNameEEPROMaddress, &selectedHdFileName);
    sprintf(buf, "EEPROM selectedHdFile value: %s", selectedHdFileName.c_str());
    printLog(buf);
  } else {
    int size = readStringFromEEPROM(DiskFileNameEEPROMaddress, &selectedDiskFileName);
    sprintf(buf, "EEPROM selectedDiskFileName value: %s", selectedDiskFileName.c_str());
    printLog(buf);
  }
  
  sprintf(buf, "EEPROM values\nHdDisk=%d,AppleIIe=%d,Fast1MhzSpeed=%d,joystick=%d,videoColor=%d,sound=%d,\nvolume=%d,dacSound=%d,\nselectedHdFileName=%s,selectedDiskFileName=%s,NewDeviceConfig=%s", HdDisk,AppleIIe,Fast1MhzSpeed,joystick,videoColor,sound,volume,dacSound,selectedHdFileName.c_str(),selectedDiskFileName.c_str(),NewDeviceConfig.c_str());
  printLog(buf);
  
  if (selectedHdFileName == "")
  {
    selectedHdFileName = "/";
  }
  if (selectedDiskFileName == "")
  {
    selectedDiskFileName = "/";
  }
#if defined(BOARD_DESKTOP)
  // EMU_DISK overrides the auto-mounted floppy (debug aid) — AFTER the EEPROM read so it actually wins.
  if (const char *d = getenv("EMU_DISK")) { selectedDiskFileName = d; HdDisk = false; }
  // EMU_C64=/x.d64 (or .prg) auto-loads + runs it on the C64 once BASIC is ready (debug/boot aid).
  if (const char *c = getenv("EMU_C64")) { selectedC64FileName = c; c64Autoload = true; }
  // Default the Apple II to THROTTLED 1 MHz on desktop: uncapped runs the 6502 at tens of MHz on a PC,
  // which makes the 1-bit speaker ultrasonic/garbage. (Toggle it off in Control > Clock speed for a
  // speed-up, accepting bad audio.) EMU_FAST=1 forces uncapped instead.
  Fast1MhzSpeed = (getenv("EMU_FAST") != nullptr);
#endif
}

int writeStringToEEPROM(int addrOffset, const String &strToWrite)
{
  byte len = strToWrite.length();
  EEPROM.write(addrOffset, len);
  for (int i = 0; i < len; i++)
  {
    EEPROM.write(addrOffset + 1 + i, strToWrite[i]);
  }
  return addrOffset + 1 + len;
}

int readStringFromEEPROM(int addrOffset, String *strToRead)
{
  int newStrLen = EEPROM.read(addrOffset);
  char data[newStrLen + 1];
  for (int i = 0; i < newStrLen; i++)
  {
    data[i] = EEPROM.read(addrOffset + 1 + i);
  }
  data[newStrLen] = '\0'; 
  *strToRead = String(data);
  return addrOffset + 1 + newStrLen;
}

void saveEEPROM() {
    EEPROM.writeBool(HdDiskEEPROMaddress, HdDisk);
    EEPROM.writeBool(IIpIIeEEPROMaddress, AppleIIe);
    EEPROM.writeBool(Fast1MhzSpeedEEPROMaddress, Fast1MhzSpeed);
    EEPROM.writeBool(JoystickEEPROMaddress, joystick);
    EEPROM.writeBool(VideoColorEEPROMaddress, videoColor);
    EEPROM.writeBool(SoundEEPROMaddress, sound);
    EEPROM.writeChar(VolumeEEPROMaddress, volume);
    EEPROM.writeBool(dacSoundEEPROMaddress, dacSound);
    EEPROM.writeChar(PlatformEEPROMaddress, currentPlatform);
    EEPROM.writeChar(ScreenFillEEPROMaddress, screenFill ? 1 : 0);
    EEPROM.writeChar(NesDisplaySkipEEPROMaddress, (char)nesDisplaySkip);
    EEPROM.writeChar(MsxSpeedEEPROMaddress, msxFast ? 1 : 0);
    EEPROM.writeChar(NesSpeedEEPROMaddress, nesFast ? 1 : 0);
    EEPROM.writeChar(SmsSpeedEEPROMaddress, smsFast ? 1 : 0);
  }

// Persist every user-configurable option (all toggles, volume, and the selected
// disk/HD image) and commit. Called when the settings window closes, so changes
// survive a reboot even without using "Save & Reboot".
void saveConfig() {
    saveEEPROM();
    EEPROM.writeChar(C64AutoloadEEPROMaddress, c64Autoload ? 1 : 0);
    EEPROM.writeChar(JoyPortEEPROMaddress, joyPort);
    writeStringToEEPROM(HdFileNameEEPROMaddress, selectedHdFileName);
    writeStringToEEPROM(DiskFileNameEEPROMaddress, selectedDiskFileName);
    writeStringToEEPROM(C64FileNameEEPROMaddress, selectedC64FileName);
    writeStringToEEPROM(NesFileNameEEPROMaddress, selectedNesFileName);
    writeStringToEEPROM(AtariFileNameEEPROMaddress, selectedAtariFileName);
    writeStringToEEPROM(MsxFileNameEEPROMaddress, selectedMsxFileName);
    writeStringToEEPROM(SmsFileNameEEPROMaddress, selectedSmsFileName);
    writeStringToEEPROM(PcxtFileNameEEPROMaddress, selectedPcFileName);
    writeStringToEEPROM(PcxtHdFileNameEEPROMaddress, selectedPcHdFileName);
    writeStringToEEPROM(Tiny386FileNameEEPROMaddress, selectedTiny386FileName);
    writeStringToEEPROM(Tiny386FileNameAEEPROMaddress, selectedTiny386FileNameA);
    EEPROM.commit();
  }
  