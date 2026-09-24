#include "../../emu.h"
#include "../shared/filebrowser.h"   // shared SD image browser (subdirectories + sorting)

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
#if defined(BOARD_PICOCALC)
    // sdFreeBytes() returns 0 on this board by design (see the note in sd.cpp): the only
    // route to a free-space figure here walks the entire FAT, ~25s on a 32GB card over SPI.
    // Printing "0 bytes" made a perfectly good card look like an empty or broken volume.
    printLog("FS.freeSpace = not measured on this board");
#else
    sprintf(buf, "FS.freeSpace = %llu bytes", (unsigned long long)sdFreeBytes());
    printLog(buf);
#endif
    getHdFileInfo(FSTYPE);
    xTaskCreate(getBlockAsync, "getBlockAsync", 4096, NULL, 1, NULL);
  }
}

// SD image browser -- same shape as the DiskII one in disk.cpp, different extension list.
static bool hdAccept(const std::string &name) {
  for (int j = 0; j < (int)fileExtensions.size(); j++)
    if ((int)name.find(fileExtensions[j].c_str()) > 0) return true;
  return false;
}
static FileBrowser hdBrowser = { "HD", &hdFiles, hdAccept, nullptr, 250, "/" };

// Rescan the current browse directory into hdFiles. Synchronous so it can be called
// directly (e.g. from the Settings device toggle) without racing the renderer.
void loadHdFilesSync()      { fbScan(hdBrowser); }
void hdBrowseEnter(const char *path) { fbEnter(hdBrowser, path); }
void hdBrowseUp()           { fbUp(hdBrowser); }

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
  busTake();               // loadHDDir() recurses, so the lock is taken here, at the one entry
  loadHDDir(FSTYPE, "/", 1);   // point -- gBusLock is not recursive.
  busGive();
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
  return 0;   // $C0F8 and any address this card does not decode read back as 0
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
  busTake();               // exists()+open()+size() is one FS transaction; keep other cores out
  if (!fs.exists(selectedHdFileName.c_str())) 
  {
    selectedHdFileName = "/";
  }
  File file = fs.open(selectedHdFileName.c_str(), "r");
  size_t len = file.size();
  hdDiskImageSize = len;
  if (len % 512 > 0)
  {
    fileHeaderSize = len - floor(len / 512) * 512;
    printLog("File Header Size: ");Serial.println(fileHeaderSize);
  }
  file.close();
  busGive();
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
  else if (hdFiles[shownFile] == ".." || hdFiles[shownFile].back() == '/') {
    // A directory row: navigate instead of mounting it (see setDiskFile()).
    if (hdFiles[shownFile] == "..") hdBrowseUp();
    else                            hdBrowseEnter(hdFiles[shownFile].c_str());
    shownFile = 0; firstShowFile = 0;
  }
  else {
    selectedHdFileName = hdFiles[shownFile].c_str();
    closeHdFile();   // the cached handle points at the OLD image -- drop it before anything reads
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

// Open the image ONCE and keep the handle; seek within it for random access.
//
// This used to close and fs.open(selectedHdFileName) again on every block that was not the
// immediate successor of the last one. ProDOS reads are overwhelmingly non-sequential (volume
// bitmap, directory, then file data), so that was a full open-by-path -- a directory walk from
// the FAT root -- per 512-byte block. On the ESP32's SD it was merely wasteful; on the PicoCalc,
// where the card hangs off a ribbon and the mount ladder may have settled at 4 MHz, it cost tens
// of milliseconds per block and a ProDOS boot looked like a hung machine. A seek is one FAT
// cluster-chain walk at worst and usually nothing at all.
//
// hdFile is invalidated (closed) by closeHdFile() whenever selectedHdFileName changes, so the
// handle can never be left pointing at the previously mounted image.
void getBlock(fs::FS &fs, ushort block)
{
  busTake();   // hold the shared bus for the whole HD block read (touch/other cores must wait)
  if (!hdFile)
    hdFile = fs.open(selectedHdFileName.c_str(), "r");
  if (hdFile) {
    if (block != lastBlock + 1)                       // random access: reposition
      hdFile.seek((size_t)block * 512 + fileHeaderSize);
    hdFile.read(actualBlock, 512);                    // sequential: the handle is already there
  }
  lastBlock = block;
  busGive();
}

// Drop the cached handle. Called whenever the mounted image changes; the next getBlock() reopens.
void closeHdFile()
{
  busTake();
  if (hdFile) hdFile.close();
  lastBlock = (ushort)-1;
  busGive();
}

void loadHDDir(fs::FS &fs, const char *dirname, uint8_t levels) {
  sprintf(buf,"Loading directory: %s\n", dirname);
  printLog(buf);
  hdFiles.clear();

  File root = fs.open(dirname, "r");
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
#if defined(BOARD_PICOCALC)
        loadHDDir(fs, file.fullName(), levels - 1);   // arduino-pico's name for path()
#else
        loadHDDir(fs, file.path(), levels - 1);
#endif
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