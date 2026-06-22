#include "../../emu.h"
#include <dirent.h>   // raw POSIX readdir() for the fast SD scan (see loadDiskFilesSync)


ushort diskFileHeaderSize = 0;
#define trackEncodedSize 5856
#define trackRawSize 4096
uint8_t trackRawData[trackRawSize];
uint8_t trackEncodedData[trackEncodedSize];
uint8_t diskVolume;
bool diskChanged = false;
bool trackChanged = false;
std::vector<std::string> diskFileExtensions = {".dsk", ".DSK", ".po", ".PO", ".do", ".DO"};

bool Drive1_2 = true;

int pointer = 0;

int diskTrack = -1;
int lastSec = -1;
int lastTrack = -1;
int diskSector = 0;
std::queue<uint8_t> phaseBuffer;
std::vector<uint8_t> outputSectorData;

const std::array<uint8_t, 4> odd_even_asc = {0x31, 0x20, 0x01, 0x30};
const std::array<uint8_t, 4> even_odd_asc = {0x11, 0x00, 0x21, 0x10};
const std::array<uint8_t, 4> odd_even_desc = {0x11, 0x20, 0x01, 0x10};
const std::array<uint8_t, 4> even_odd_desc = {0x31, 0x00, 0x21, 0x30};

const std::array<uint8_t, 4> start_sequence_1 = {0x31, 0x30, 0x21, 0x20};
const std::array<uint8_t, 4> start_sequence_2 = {0x11, 0x10, 0x01, 0x00};

const std::array<uint8_t, 64> translateTable = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6,
    0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC,
    0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
    0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
    0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};

const char translateDOTrack[] = {0x00, 0x07, 0x0e, 0x06, 0x0d, 0x05, 0x0c, 0x04, 0x0b, 0x03, 0x0a, 0x02, 0x09, 0x01, 0x08, 0x0f};
const char translatePOTrack[] = {0x00, 0x08, 0x01, 0x09, 0x02, 0x0A, 0x03, 0x0b, 0x04, 0x0C, 0x05, 0x0d, 0x06, 0x0E, 0x07, 0x0f};
const ushort secoffset[] = {0, 0x700, 0xe00, 0x600, 0xd00, 0x500, 0xc00, 0x400, 0xb00, 0x300, 0xa00, 0x200, 0x900, 0x100, 0x800, 0xf00};

volatile bool trackPendingSave = false;
void diskSetup()
{
  if (diskAttached)
  {
    initializedHdDisk = false;
    printLog("DiskII Setup...");
    xTaskCreate(loadDiskAsync, "loadDiskAsync", 4096, NULL, 2, NULL);
    sprintf(buf, "FS.freeSpace = %d bytes", FSTYPE.totalBytes() - FSTYPE.usedBytes());
    printLog(buf);
    getDiskFileInfo(FSTYPE);
    phaseBuffer = std::queue<uint8_t>();
    xTaskCreate(saveTrackAsync, "saveTrackAsync", 4096, NULL, 1, NULL);
  }
}

void saveTrackAsync(void *pvParameters)
{
  int count = 0;
  while (running)
  {
    if (trackPendingSave && !DriveMotorON_OFF)
    {
      if (count > 5)
      {
        Serial.println("Late Save.");
        saveImage(FSTYPE, diskTrack);
        getTrack(FSTYPE, diskTrack, true);
        trackPendingSave = false;
        count = 0;
      }
      count++;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void addPhase(uint8_t phase)
{
  int track = diskTrack;
  phaseBuffer.push(phase);
  // sprintf(buf, "Add Phase: %02X, size: %d", phase, phaseBuffer.size());
  // printLog(buf);
  if (phaseBuffer.size() > 4)
  {
    phaseBuffer.pop();
    // sprintf(buf, "Buffer > 4");
    // printLog(buf);
  }

  std::array<uint8_t, 4> currentBuffer;
  for (size_t i = 0; i < currentBuffer.size() && !phaseBuffer.empty(); ++i)
  {
    currentBuffer[i] = phaseBuffer.front();
    phaseBuffer.pop();
  }
  // Serial.println("currentBuffer: ");
  // for (int i = 0; i < currentBuffer.size(); i++)
  // {
  //   sprintf(buf, "%02X, ", currentBuffer[i]);
  //   printLog(buf);
  // }
  // Serial.println("");
  for (const auto &val : currentBuffer)
  {
    phaseBuffer.push(val); // Restore the queue
  }

  if (track % 2 == 0 && std::equal(currentBuffer.begin(), currentBuffer.end(), even_odd_asc.begin()))
  {
    track++;
  }
  else if (track % 2 != 0 && std::equal(currentBuffer.begin(), currentBuffer.end(), odd_even_asc.begin()))
  {
    track++;
  }
  else if (track % 2 == 0 && std::equal(currentBuffer.begin(), currentBuffer.end(), even_odd_desc.begin()) && track > 0)
  {
    track--;
  }
  else if (track % 2 != 0 && std::equal(currentBuffer.begin(), currentBuffer.end(), odd_even_desc.begin()) && track > 0)
  {
    track--;
  }
  else if (track > 0 && (std::equal(currentBuffer.begin(), currentBuffer.end(), start_sequence_1.begin()) ||
                         std::equal(currentBuffer.begin(), currentBuffer.end(), start_sequence_2.begin())))
  {
    track--;
  }

  // getTrack(FSTYPE, track, false);
  if (track != diskTrack)
  {
    // sprintf(buf, "Track changed: %d ", track);
    // printLog(buf);
    if (trackPendingSave)
    {
      saveImage(FSTYPE, diskTrack);
      getTrack(FSTYPE, diskTrack, true);
      trackPendingSave = false;
    }
    diskTrack = track;
    trackChanged = true;
  }
}

bool identifyDosProdos()
{
  std::array<uint8_t, 6> prodosstr;
  for (int i = 0; i < 6; i++)
  {
    prodosstr[i] = trackRawData[0xe03 + i];
  }
  return !std::equal(prodosstr.begin(), prodosstr.end(), std::begin(std::array<uint8_t, 6>{0x50, 0x52, 0x4f, 0x44, 0x4f, 0x53}));
}

void getDiskFileInfo(fs::FS &fs)
{
  
  Serial.printf("selectedDiskFileName = %s\n", selectedDiskFileName.c_str());
  if (!fs.exists(selectedDiskFileName.c_str()))
  {
    Serial.println("File not found");
    shownFile = 0;

  }
  // File file = fs.open(selectedDiskFileName.c_str());
  // size_t len = file.size();
  // sprintf(buf, "File Size: %d", len);
  // printLog(buf);
  // file.close();
  getTrack(FSTYPE, 17, true);
  diskVolume = trackRawData[0x06];
  sprintf(buf, "Disk Volume: %d", diskVolume);
  printLog(buf);
  getTrack(FSTYPE, 0, true);
  FlagDO_PO = identifyDosProdos();
  sprintf(buf, "Disk format: %s", FlagDO_PO ? "DOS" : "PRODOS");
  printLog(buf);
  
}

void getTrack(fs::FS &fs, int track, bool force)
{
  if (track != diskTrack || force)
  {
    busTake();   // hold the shared HSPI bus for the whole open+seek+read (touch must wait)
    size_t positionToRead = getOffset(track, 0);
    // sprintf(buf, "Reading track %d - %s (%s)", track, selectedDiskFileName.c_str(), force ? "force" : "");
    // printLog(buf);
    File file = fs.open(selectedDiskFileName.c_str(), FILE_READ);

    if (file)
    {
      if (file.seek(positionToRead))
      {
        // sprintf(buf, "File Read %d - %d", track, diskTrack);
        // printLog(buf);
        file.read(trackRawData, trackRawSize);
        diskTrack = track;
        trackRawDataEncode(diskTrack);
      }

      file.close();
    }
    else
    {
      printLog("Failed to open file for reading");
    }
    busGive();
  }
}

void saveImage(fs::FS &fs, int track)
{
  sprintf(buf, "Saving Track %0d", track);
  Serial.println(buf);
  int positionToWrite = getOffset(track, 0);

  busTake();   // hold the shared HSPI bus for the whole open+seek+write
  File file = fs.open(selectedDiskFileName.c_str(), "r+");
  if (file) {
    file.seek(positionToWrite, SeekSet);
    size_t s = file.write(trackRawData, trackRawSize);
    Serial.printf("size append: %d\n",s);
    file.close();
  }
  else
  {
    Serial.println("File failed to open");
  }
  busGive();
}

void nextDiskFile()
{
  if (shownFile < (int)((diskFiles.size()) - 1))
  {
    shownFile++;
  }
  // Serial.printf("Selected File: %s\n", diskFiles[shownFile].c_str());
}

void prevDiskFile()
{
  if (shownFile > 0)
  {
    shownFile--;
  }
  // Serial.printf(" Selected File: %s\n", diskFiles[shownFile].c_str());
}

void saveDiskFile()
{
  paused = true;
  writeStringToEEPROM(DiskFileNameEEPROMaddress, selectedDiskFileName.c_str());
  saveEEPROM();
  EEPROM.commit();
  paused = false;
}

void setDiskFile()
{
  paused = true;
  if (diskFiles.size() == 0) {
    printLog("No disk files found");
  }
  else if (shownFile < 0 || shownFile >= (int)diskFiles.size()) {
    printLog("Invalid disk file index");
    shownFile = 0;
    selectedDiskFileName = "/";
  }
  else {
    selectedDiskFileName = diskFiles[shownFile].c_str();
  }
  paused = false;
}

// Scan the SD root for disk images into diskFiles. Synchronous so it can be called
// directly (e.g. from the Settings device toggle) without racing the renderer.
void loadDiskFilesSync()
{
  diskFiles.clear();
  // Fast scan: raw readdir() returns each name + a file/dir flag directly, avoiding the
  // per-entry fopen()/stat() that Arduino's openNextFile() does (which re-walks the path from
  // the FS root for every entry - hundreds of ms each on this 4MHz-ish SPI SD).
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
    for (int j = 0; j < (int)diskFileExtensions.size(); j++)
    {
      if ((int)fileName.find(diskFileExtensions[j].c_str()) > 0)
      {
        diskFiles.push_back("/" + fileName);
        break;                                          // one extension match is enough
      }
    }
  }
  closedir(dp);
}

void loadDiskAsync(void *pvParameters)
{
  loadDiskFilesSync();
  listFiles(false); // Refresh the file list
  vTaskDelete(NULL); // Self-deletion
}

int getOffset(int track, int sector)
{
  return diskFileHeaderSize + (sector * 256) + (track * (256 * 16));
}

int getSectorOffset(int sector)
{
  return (sector * 256);
}

std::vector<uint8_t> getSectorData(int sector)
{
  std::vector<uint8_t> output(256);
  if (sector < 16)
  {
    int offset = getSectorOffset(sector);

    for (int i = 0; i < 256; i++)
    {
      output[i] = trackRawData[offset + i];
    }
  }
  else
  {
    // Handle the else case if needed
  }

  return output;
}

std::array<uint8_t, 2> encodeByte(uint8_t data)
{
  std::array<uint8_t, 2> output = {0, 0};
  std::bitset<16> bitsEncoded;
  std::bitset<8> bitsData(data);

  for (int i = 0; i < 16; i++)
  {
    if (i % 2 == 0)
      bitsEncoded[i] = true;
    else
    {
      if (i > 8)
        bitsEncoded[i] = bitsData[8 - (i - 7)];
      else
        bitsEncoded[i] = bitsData[8 - i];
    }
  }

  for (int i = 0; i < 8; i++)
  {
    output[0] += (bitsEncoded[i] ? static_cast<uint8_t>(std::pow(2, 7 - i)) : 0);
    output[1] += (bitsEncoded[i + 8] ? static_cast<uint8_t>(std::pow(2, 7 - i)) : 0);
  }
  return output;
}

std::array<uint8_t, 2> checksum(uint8_t volume, uint8_t sector, uint8_t track)
{
  std::array<uint8_t, 2> output = {0, 0};
  std::bitset<16> checkedBits;
  std::bitset<16> checkedBitsInverted;
  std::bitset<16> bitsVolume(encodeByte(volume)[0] | (encodeByte(volume)[1] << 8));
  std::bitset<16> bitsSector(encodeByte(sector)[0] | (encodeByte(sector)[1] << 8));
  std::bitset<16> bitsTrack(encodeByte(track)[0] | (encodeByte(track)[1] << 8));

  for (int i = 0; i < 16; i++)
  {
    int sumBits = bitsVolume[i] + bitsSector[i] + bitsTrack[i];
    checkedBits[i] = (sumBits == 1 || sumBits == 3);
  }

  for (int i = 0; i < 16; i++)
  {
    if (i < 8)
      checkedBitsInverted[i] = checkedBits[7 - i];
    else
      checkedBitsInverted[i] = checkedBits[23 - i];
  }

  for (int i = 0; i < 8; i++)
  {
    output[0] += (checkedBitsInverted[i] ? static_cast<uint8_t>(std::pow(2, 7 - i)) : 0);
    output[1] += (checkedBitsInverted[i + 8] ? static_cast<uint8_t>(std::pow(2, 7 - i)) : 0);
  }
  return output;
}

std::array<uint8_t, 343> encode6_2(uint8_t sector)
{
  int offset = getSectorOffset(sector); // sector * 256, read straight from trackRawData

  uint8_t outputData[256];
  uint8_t outputlast2[0x56] = {0};
  std::array<uint8_t, 343> agregate;

  for (int i = 0; i < 256; i++)
  {
    uint8_t in = trackRawData[offset + i];
    outputData[i] = static_cast<uint8_t>(in >> 2);
    std::bitset<8> bitsData(in);
    if (i < 86)
      outputlast2[i] |= (bitsData[0] ? 2 : 0) + (bitsData[1] ? 1 : 0);
    else if (i < 172)
      outputlast2[i - 86] |= ((bitsData[0] ? 2 : 0) + (bitsData[1] ? 1 : 0)) << 2;
    else
      outputlast2[i - 172] |= ((bitsData[0] ? 2 : 0) + (bitsData[1] ? 1 : 0)) << 4;
  }

  uint8_t lastByte = 0;
  for (int i = 0; i < 86; i++)
  {
    agregate[i] = translateTable[outputlast2[i] ^ lastByte];
    lastByte = outputlast2[i];
  }
  for (int i = 0; i < 256; i++)
  {
    agregate[86 + i] = translateTable[outputData[i] ^ lastByte];
    lastByte = outputData[i];
  }
  agregate[342] = translateTable[lastByte];

  return agregate;
}

void trackRawDataEncode(int track)
{
  static const std::array<uint8_t, 16> sectors = {0xa, 0xb, 0xc, 0xd, 0xe, 0xf, 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9}; // DOS
  int pos = 0;
  for (uint8_t isec : sectors)
  {
    trackEncodedData[pos++] = 0xff;
    trackEncodedData[pos++] = 0xff;
    trackEncodedData[pos++] = 0xff;
    trackEncodedData[pos++] = 0xd5; // Prologue address
    trackEncodedData[pos++] = 0xaa;
    trackEncodedData[pos++] = 0x96;
    std::array<uint8_t, 2> b = encodeByte(diskVolume); // Volume
    trackEncodedData[pos++] = b[0];
    trackEncodedData[pos++] = b[1];
    b = encodeByte(static_cast<uint8_t>(track)); // Track
    trackEncodedData[pos++] = b[0];
    trackEncodedData[pos++] = b[1];
    b = encodeByte(isec); // Sector
    trackEncodedData[pos++] = b[0];
    trackEncodedData[pos++] = b[1];
    b = checksum(diskVolume, static_cast<uint8_t>(track), isec); // checksum
    trackEncodedData[pos++] = b[0];
    trackEncodedData[pos++] = b[1];
    trackEncodedData[pos++] = 0xde; // Epilogue address
    trackEncodedData[pos++] = 0xaa;
    trackEncodedData[pos++] = 0xeb;
    trackEncodedData[pos++] = 0xd5; // Prologue data
    trackEncodedData[pos++] = 0xaa;
    trackEncodedData[pos++] = 0xad;
    std::array<uint8_t, 343> data = encode6_2(translateDOTrack[isec]); // Data field + checksum
    for (uint8_t v : data)
      trackEncodedData[pos++] = v;
    trackEncodedData[pos++] = 0xde; // Epilogue
    trackEncodedData[pos++] = 0xaa;
    trackEncodedData[pos++] = 0xeb;
  }
  // printLog("Raw Data");
  // PrintHex(trackRawData, trackRawSize);
  // printLog("Raw Encoded");
  // PrintHex(trackEncodedData,trackEncodedSize);
}

unsigned char detranlateTable(unsigned char data)
{

  for (size_t j = 0; j < translateTable.size(); j++)
  {
    if (translateTable[j] == data)
    {
      return static_cast<unsigned char>(j);
    }
  }
  return 0;
}

std::vector<uint8_t> decode6_2(const std::vector<uint8_t> &diskData)
{
  std::vector<uint8_t> dataTranslated(343);
  std::vector<uint8_t> bufferData(343);

  std::vector<uint8_t> inputlast2Encoded(0x56);
  std::vector<uint8_t> inputDataEncoded(256);
  std::vector<uint8_t> inputDataDecoded(256);
  std::vector<uint8_t> outputlast2(0x56);
  std::vector<uint8_t> outputData(256);

  uint8_t prevByte = 0;
  for (size_t i = 0; i < diskData.size(); i++)
  {
    dataTranslated[i] = detranlateTable(diskData[i]);
    bufferData[i] = dataTranslated[i] ^ prevByte;
    prevByte = bufferData[i];
  }

  for (size_t i = 0; i < bufferData.size() - 1; i++)
  {
    if (i < 86)
      inputlast2Encoded[i] = bufferData[i];
    else
      inputDataEncoded[i - 86] = bufferData[i];
  }

  for (size_t i = 0; i < inputDataEncoded.size(); i++)
  {
    outputData[i] = inputDataEncoded[i] << 2;
  }

  for (size_t i = 0; i < outputData.size(); i++)
  {
    if (i < 86)
    {
      std::bitset<8> bitsVolume(inputlast2Encoded[i]);
      inputDataDecoded[i] = outputData[i] + (bitsVolume[0] ? 2 : 0) + (bitsVolume[1] ? 1 : 0);
    }
    else if (i < 172)
    {
      std::bitset<8> bitsVolume(inputlast2Encoded[i - 86]);
      inputDataDecoded[i] = outputData[i] + (bitsVolume[2] ? 2 : 0) + (bitsVolume[3] ? 1 : 0);
    }
    else
    {
      std::bitset<8> bitsVolume(inputlast2Encoded[i - 172]);
      inputDataDecoded[i] = outputData[i] + (bitsVolume[4] ? 2 : 0) + (bitsVolume[5] ? 1 : 0);
    }
  }

  return inputDataDecoded;
}

void setSectorData(uint8_t sector, const std::vector<uint8_t> &data)
{
  if (sector < 16)
  {
    auto offset = getSectorOffset(sector);

    for (int i = 0; i < 256; i++)
    {
      trackRawData[offset + i] = data[i];
    }
  }
}

void setBlockData(int sector, const std::vector<uint8_t> &data)
{
  if (sector < 16)
  {
    auto offset = secoffset[sector];

    for (int i = 0; i < 256; i++)
    {
      trackRawData[offset + i] = data[i];
    }
  }
}

char diskSoftSwitchesRead(ushort address)
{

  if (address == 0xc0ec)
  {
    if (DriveQ6H_L == false && DriveQ7H_L == false)
    {
      if (diskChanged || trackChanged)
      {
        getTrack(FSTYPE, diskTrack, true);
        diskChanged = false;
        trackChanged = false;
      }

      if (pointer > trackEncodedSize - 1)
        pointer = 0;

      //   sprintf(buf, "Disk Track: %d, Disk Read: %04X, Pointer: %d, Data: %02X", diskTrack, address, pointer, trackEncodedData[pointer]);
      //   printLog(buf);
      //  sprintf(buf, "(%04x)[R]%04X: %02X", PC, address, trackEncodedData[pointer]);
      //  printLog(buf);
      return trackEncodedData[pointer++];
    }
  }

  return processSwitchc0e0(address, 0);
}

void diskSoftSwitchesWrite(ushort address, char value)
{
  int sec = FlagDO_PO ? read8(0x2d) : read8(0xd357);

  if (address == 0xc0ed)
  {
    if (DriveQ6H_L == false && DriveQ7H_L == true)
    {
      outputSectorData.push_back(value);

      if (outputSectorData.size() == 354)
      {
        sprintf(buf, "Preparing Track: %d, Sector: %d", diskTrack, sec);
        printLog(buf);
        if (lastTrack == diskTrack && lastSec == sec)
        {
          saveImage(FSTYPE, diskTrack);
          getTrack(FSTYPE, diskTrack, true);
        }
        lastTrack = diskTrack;
        lastSec = sec;
        std::vector<uint8_t> cleanData(outputSectorData.begin() + 7, outputSectorData.begin() + 350);
        std::vector<uint8_t> decsecData = decode6_2(cleanData);
        if (FlagDO_PO)
        {
          setSectorData(translateDOTrack[sec], decsecData);
          trackPendingSave = true;
        }
        else
        {
          setBlockData(sec, decsecData);
          trackPendingSave = true;
          if (diskTrack == 0 && sec == 11)
          {
            saveImage(FSTYPE, diskTrack);
            getTrack(FSTYPE, diskTrack, true);
            trackPendingSave = false;
          }
        }
        // saveImage(FSTYPE, diskTrack);
        // getTrack(FSTYPE, diskTrack, true);
        outputSectorData.clear();
      }
    }
  }

  processSwitchc0e0(address, value);
}

char processSwitchc0e0(ushort address, char value)
{
  if (address == 0xc0e0)
  {
    DrivePhase0ON_OFF = false;
    addPhase(0x00);
  }
  else if (address == 0xc0e1)
  {
    DrivePhase0ON_OFF = true;
    addPhase(0x01);
  }
  else if (address == 0xc0e2)
  {
    DrivePhase1ON_OFF = false;
    addPhase(0x10);
  }
  else if (address == 0xc0e3)
  {
    DrivePhase1ON_OFF = true;
    addPhase(0x11);
  }
  else if (address == 0xc0e4)
  {
    DrivePhase2ON_OFF = false;
    addPhase(0x20);
  }
  else if (address == 0xc0e5)
  {
    DrivePhase2ON_OFF = true;
    addPhase(0x21);
  }
  else if (address == 0xc0e6)
  {
    DrivePhase3ON_OFF = false;

    addPhase(0x30);
  }
  else if (address == 0xc0e7)
  {
    DrivePhase3ON_OFF = true;

    addPhase(0x31);
  }
  else if (address == 0xc0e8)
  {
    DriveMotorON_OFF = false;
    diskLed(HIGH);
  }
  else if (address == 0xc0e9)
  {
    DriveMotorON_OFF = true;
    diskLed(LOW);
  }
  else if (address == 0xc0ea)
    Drive1_2 = true;
  else if (address == 0xc0eb)
    Drive1_2 = false;
  else if (address == 0xc0ec)
    DriveQ6H_L = false;
  else if (address == 0xc0ed)
    DriveQ6H_L = true;
  else if (address == 0xc0ee)
  {
    DriveQ7H_L = false;
    return 0; // Not Write Protected, 9f Write protected
  }
  else if (address == 0xc0ef)
  {
    DriveQ7H_L = true;
  }
  return 0;
}