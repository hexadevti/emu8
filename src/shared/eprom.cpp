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
  if (currentPlatform > PLATFORM_IIGS) currentPlatform = PLATFORM_APPLE2;  // unset/garbage -> default

  // C64 settings (validated so old/uninitialised EEPROM doesn't enable surprises).
  c64Autoload = (EEPROM.readChar(C64AutoloadEEPROMaddress) == 1);
  joyPort = EEPROM.readChar(JoyPortEEPROMaddress);
  if (joyPort != 1 && joyPort != 2) joyPort = 2;
  // ==1 test (not readBool) so an uninitialised 0xFF on older devices reads as OFF, not ON.
  screenFill = (EEPROM.readChar(ScreenFillEEPROMaddress) == 1);
  { char s = EEPROM.readChar(NesDisplaySkipEEPROMaddress); nesDisplaySkip = (s >= 1 && s <= 3) ? (uint8_t)s : 3; }  // default 3; fresh EEPROM (0xFF) -> 3
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
    EEPROM.commit();
  }
  