#include "../../emu.h"
#include <dirent.h>   // raw POSIX readdir() for the fast SD scan (see loadHdFilesSync)

// Memory map (for slot 7):

//  C0F0	(r)   EXECUTE AND RETURN STATUS
// 	C0F1	(r)   STATUS (or ERROR): b7=busy, b0=error
// 	C0F2	(r/w) COMMAND
// 	C0F3	(r/w) UNIT NUMBER
// 	C0F4	(r/w) LOW BYTE OF MEMORY BUFFER
// 	C0F5	(r/w) HIGH BYTE OF MEMORY BUFFER
// 	C0F6	(r/w) LOW BYTE OF BLOCK NUMBER
// 	C0F7	(r/w) HIGH BYTE OF BLOCK NUMBER
// 	C0F8    (r)   NEXT BYTE (legacy read-only port - still supported)
// 	C0F9    (r)   LOW BYTE OF DISK IMAGE SIZE IN BLOCKS
// 	C0FA    (r)   HIGHT BYTE OF DISK IMAGE SIZE IN BLOCKS

unsigned char hdCommand;
unsigned char hdStatus;
bool hdUnitNumber1_2 = true;
unsigned short hdMemoryBuffer;
unsigned short hdBlockNumber;
size_t hdDiskImageSize ;
ushort fileHeaderSize = 0;
unsigned char actualBlock[512];
std::vector<std::string> fileExtensions = { ".hdv", ".po", ".2mg" };
File hdFile;
ushort lastBlock = -1;

void HDSetup()
{
  if (hdAttached) {
    initializedHdDisk = true;
    printLog("HD Setup...");
    xTaskCreate(loadHdAsync, "loadHdAsync", 4096, NULL, 2, NULL);
    sprintf(buf, "FS.freeSpace = %d bytes", FSTYPE.totalBytes() - FSTYPE.usedBytes());
    printLog(buf);
    getHdFileInfo(FSTYPE);
    xTaskCreate(getBlockAsync, "getBlockAsync", 4096, NULL, 1, NULL);
  }
}

// Scan the SD root for HD images into hdFiles. Synchronous so it can be called
// directly (e.g. from the Settings device toggle) without racing the renderer.
void loadHdFilesSync()
{
  hdFiles.clear();
  // Fast scan via raw readdir() - see loadDiskFilesSync() for why (avoids per-entry fopen/stat).
  DIR *dp = opendir(SD_VFS_ROOT);
  if (!dp)
  {
    printLog("Failed to open directory");
    return;
  }
  struct dirent *de;
  while ((de = readdir(dp)) != nullptr)
  {
    if (de->d_type == DT_DIR) continue;                 // root files only (matches old behavior)
    std::string fileName = de->d_name;
    for (int j = 0; j < (int)fileExtensions.size(); j++)
    {
      if ((int)fileName.find(fileExtensions[j].c_str()) > 0)
      {
        hdFiles.push_back("/" + fileName);
        break;                                          // one extension match is enough
      }
    }
  }
  closedir(dp);
}

void loadHdAsync(void *pvParameters)
{
  loadHdFilesSync();
  listFiles(false); // Refresh the file list
  vTaskDelete(NULL); // Self-deletion
}


void getBlockAsync(void *pvParameters) {
  int count = 0;
  while (running)
  {
    if (trackPendingSave && !DriveMotorON_OFF) {
      if (count > 5) {
        Serial.println("Late Save.");
        saveImage(FSTYPE, diskTrack);
        getTrack(FSTYPE, diskTrack, true);
        trackPendingSave = false;
        count = 0;
      }
      count++;
    }
    delay(10);
  }
  
}

void loadHD() 
{
  loadHDDir(FSTYPE, "/", 1);
}

char HDSoftSwitchesRead(ushort address)
{
  // sprintf(buf,"HDSoftSwitchesRead %04X", address);
  // printLog(buf);
  if (address == 0xc0f0) { 
    switch (hdCommand) {
      case 0x01:
        hdStatus = 0xb7;
        hdStatus = loadBlock(hdMemoryBuffer, hdBlockNumber);
        return hdStatus;
      default:
        break;
    }
  } else if (address == 0xc0f1) {
    return hdStatus;
  } else if (address == 0xc0f2) {
    return hdCommand;
  } else if (address == 0xc0f3) {
    return (char)((hdUnitNumber1_2 ? 1 : 0) << 7);
  } else if (address == 0xc0f4) {
    return (char)(hdMemoryBuffer & 0x00ff);
  } else if (address == 0xc0f5) {
    return (char)(hdMemoryBuffer & 0xff00);
  } else if (address == 0xc0f6) {
    return (char)(hdBlockNumber & 0x00ff);
  } else if (address == 0xc0f7) {
    return (char)(hdBlockNumber & 0xff00);
  } else if (address == 0xc0f8) {
  } else if (address == 0xc0f9) {
    return (char)((hdUnitNumber1_2 ? getBlockQty() : getBlockQty()) & 0x00ff);
  } else if (address == 0xc0fa) {
    return (char)((hdUnitNumber1_2 ? getBlockQty() : getBlockQty()) & 0xff00);
  }
}

void HDSoftSwitchesWrite(ushort address, char value) {
  if (address == 0xc0f0) {
  } else if (address == 0xc0f1) {
  } else if (address == 0xc0f2) {
    hdCommand = value;
  } else if (address == 0xc0f3) {
    hdUnitNumber1_2 = value >> 7 == 0;
  } else if (address == 0xc0f4) {
    hdMemoryBuffer = (unsigned short)(hdMemoryBuffer & 0xff00 | value);
  } else if (address == 0xc0f5) {
    hdMemoryBuffer = (unsigned short)(hdMemoryBuffer & 0x00ff | value << 8);
  } else if (address == 0xc0f6) {
    hdBlockNumber = (unsigned short)(hdBlockNumber & 0xff00 | value);
  } else if (address == 0xc0f7) {
    hdBlockNumber = (unsigned short)(hdBlockNumber & 0x00ff | value << 8);
  } else if (address == 0xc0f8) {
  } else if (address == 0xc0f9) {
  } else if (address == 0xc0fa) {
  }
}

void getHdFileInfo(fs::FS &fs)
{
  if (!fs.exists(selectedHdFileName.c_str())) 
  {
    selectedHdFileName = "/";
  }
  File file = fs.open(selectedHdFileName.c_str());
  size_t len = file.size();
  hdDiskImageSize = len;
  if (len % 512 > 0)
  {
    fileHeaderSize = len - floor(len / 512) * 512;
    printLog("File Header Size: ");Serial.println(fileHeaderSize);
  }
  file.close();
}

void nextHdFile()
{
  if (shownFile < (int)((hdFiles.size())-1)) {
    shownFile++;
  }
}

void prevHdFile()
{
  if (shownFile > 0) {
    shownFile--;
  }
}

void saveHdFile()
{
  paused = true;
  Serial.printf("Saving HD file: %s\n", selectedHdFileName.c_str());
  writeStringToEEPROM(HdFileNameEEPROMaddress, selectedHdFileName.c_str());
  saveEEPROM();
  EEPROM.commit();
  paused = false;
}

void setHdFile()
{
  paused = true;
  if (hdFiles.size() == 0) {
    printLog("No HD files found");
  }
  else if (shownFile < 0 || shownFile >= (int)hdFiles.size()) {
    printLog("Invalid HD file index");
    shownFile = 0;
    selectedHdFileName = "/";
  }
  else {
    selectedHdFileName = hdFiles[shownFile].c_str();
  }
  paused = false;

}

char loadBlock(unsigned short address, unsigned short block)
{
  diskLed(LOW);

  getBlock(FSTYPE, block);
  try
  {
    // sprintf(buf,"Write block to memory: %d", block);
    // printLog(buf);
    for (int i = 0; i < 512; i++)
    {
      write8((address + i), actualBlock[i]);
    }
    //printLog("512 bytes written");
    diskLed(HIGH);
    
    return 0;
  }
  catch(std::exception ex)
  {
    diskLed(HIGH);
    
    
    return 0xb0;
  }
}

ushort getBlockQty()
{
  return (ushort)((hdDiskImageSize - fileHeaderSize) / 512);
}

void getBlock(fs::FS &fs, ushort block)
{
  busTake();   // hold the shared HSPI bus for the whole HD block read (touch must wait)
  if (block != lastBlock + 1) {
    //printLog("New");
    if (hdFile.available())
      hdFile.close();
      
    hdFile = fs.open(selectedHdFileName.c_str());
    if (hdFile) {
      size_t positionToRead = block * 512 + fileHeaderSize;
      if (hdFile.seek(positionToRead))
      {
        hdFile.read(actualBlock, 512);
        //printSequence(20);
        
      }
    }
  }
  else
  {
    //printLog("Sequential");
    if (hdFile.available()) {
      hdFile.read(actualBlock, 512);
      //printSequence(20);
    }
  }
  lastBlock = block;
  busGive();
}

void loadHDDir(fs::FS &fs, const char *dirname, uint8_t levels) {
  sprintf(buf,"Loading directory: %s\n", dirname);
  printLog(buf);
  hdFiles.clear();

  File root = fs.open(dirname);
  if (!root) {
    printLog("Failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    printLog("Not a directory");
    return;
  }

  File file = root.openNextFile();
  int i = 0;
  while (file) {
    if (file.isDirectory()) {
      printLog("  DIR : ");
      printLog(file.name());
      if (levels) {
        loadHDDir(fs, file.path(), levels - 1);
      }
    } else {
      bool acepted = false;
      std::string fileName = file.name();
      for (int j = 0; j < fileExtensions.size(); j++)
      {
        if ((int)fileName.find(fileExtensions[j].c_str()) > 0)
        {
          acepted = true;
          break;
        }
      }
      
      if (acepted)
      {
        sprintf(buf, " FOUND FILE: %s SIZE: %d", file.name(), file.size());
        printLog(buf);
        std::string str(file.name());
        hdFiles.push_back("/" + str);
      }
      i++;
    }
    file = root.openNextFile();
  }
  file.close();
  root.close();
}