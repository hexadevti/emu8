#include "../../emu.h"

#include <cstring>
#include <iostream>

    std::map<unsigned char, std::string> opcodeMap = {
        {0x00, "BRK       "}, 
        {0x01, "ORA(IND,X)"}, 
        {0x05, "ORA ZP    "}, 
        {0x06, "ASL ZP    "}, 
        {0x08, "PHP       "}, 
        {0x09, "ORA #     "}, 
        {0x0A, "ASL A     "}, 
        {0x0D, "ORA ABS   "}, 
        {0x0E, "ASL ABS   "}, 
        {0x10, "BPL       "}, 
        {0x11, "ORA(IND),Y"}, 
        {0x15, "ORA ZP,X  "}, 
        {0x16, "ASL ZP,X  "}, 
        {0x18, "CLC       "}, 
        {0x19, "ORA ABS,Y "}, 
        {0x1D, "ORA ABS,X "}, 
        {0x1E, "ASL ABS,X "}, 
        {0x20, "JSR       "}, 
        {0x21, "AND(IND,X)"}, 
        {0x24, "BIT ZP    "}, 
        {0x25, "AND ZP    "}, 
        {0x26, "ROL ZP    "}, 
        {0x28, "PLP       "}, 
        {0x29, "AND #     "}, 
        {0x2A, "ROL A     "}, 
        {0x2C, "BIT ABS   "}, 
        {0x2D, "AND ABS   "}, 
        {0x2E, "ROL ABS   "}, 
        {0x30, "BMI       "}, 
        {0x31, "AND(IND),Y"}, 
        {0x35, "AND ZP,X  "}, 
        {0x36, "ROL ZP,X  "}, 
        {0x38, "SEC       "}, 
        {0x39, "AND ABS,Y "}, 
        {0x3D, "AND ABS,X "}, 
        {0x3E, "ROL ABS,X "}, 
        {0x40, "RTI       "}, 
        {0x41, "EOR(IND,X)"}, 
        {0x45, "EOR ZP    "}, 
        {0x46, "LSR ZP    "}, 
        {0x48, "PHA       "}, 
        {0x49, "EOR #     "}, 
        {0x4A, "LSR A     "}, 
        {0x4C, "JMP ABS   "}, 
        {0x4D, "EOR ABS   "}, 
        {0x4E, "LSR ABS   "}, 
        {0x50, "BVC       "}, 
        {0x51, "EOR(IND),Y"}, 
        {0x55, "EOR ZP,X  "}, 
        {0x56, "LSR ZP,X  "}, 
        {0x58, "CLI       "}, 
        {0x59, "EOR ABS,Y "}, 
        {0x5D, "EOR ABS,X "}, 
        {0x5E, "LSR ABS,X "}, 
        {0x60, "RTS       "}, 
        {0x61, "ADC(IND,X)"}, 
        {0x65, "ADC ZP    "}, 
        {0x66, "ROR ZP    "}, 
        {0x68, "PLA       "}, 
        {0x69, "ADC #     "}, 
        {0x6A, "ROR A     "}, 
        {0x6C, "JMP (IND) "}, 
        {0x6D, "ADC ABS   "}, 
        {0x6E, "ROR ABS   "}, 
        {0x70, "BVS       "}, 
        {0x71, "ADC(IND),Y"}, 
        {0x75, "ADC ZP,X  "}, 
        {0x76, "ROR ZP,X  "}, 
        {0x78, "SEI       "}, 
        {0x79, "ADC ABS,Y "}, 
        {0x7D, "ADC ABS,X "}, 
        {0x7E, "ROR ABS,X "}, 
        {0x81, "STA(IND,X)"}, 
        {0x84, "STY ZP    "}, 
        {0x85, "STA ZP    "}, 
        {0x86, "STX ZP    "}, 
        {0x88, "DEY       "}, 
        {0x8A, "TXA       "}, 
        {0x8C, "STY ABS   "}, 
        {0x8D, "STA ABS   "}, 
        {0x8E, "STX ABS   "}, 
        {0x90, "BCC       "}, 
        {0x91, "STA(IND),Y"}, 
        {0x94, "STY ZP,X  "}, 
        {0x95, "STA ZP,X  "}, 
        {0x96, "STX ZP,Y  "}, 
        {0x98, "TYA       "}, 
        {0x99, "STA ABS,Y "}, 
        {0x9A, "TXS       "}, 
        {0x9D, "STA ABS,X "}, 
        {0xA0, "LDY #     "}, 
        {0xA1, "LDA(IND,X)"}, 
        {0xA2, "LDX #     "}, 
        {0xA4, "LDY ZP    "}, 
        {0xA5, "LDA ZP    "}, 
        {0xA6, "LDX ZP    "}, 
        {0xA8, "TAY       "}, 
        {0xA9, "LDA #     "}, 
        {0xAA, "TAX       "}, 
        {0xAC, "LDY ABS   "}, 
        {0xAD, "LDA ABS   "}, 
        {0xAE, "LDX ABS   "}, 
        {0xB0, "BCS       "}, 
        {0xB1, "LDA(IND),Y"}, 
        {0xB4, "LDY ZP,X  "}, 
        {0xB5, "LDA ZP,X  "}, 
        {0xB6, "LDX ZP,Y  "}, 
        {0xB8, "CLV       "}, 
        {0xB9, "LDA ABS,Y "}, 
        {0xBA, "TSX       "}, 
        {0xBC, "LDY ABS,X "}, 
        {0xBD, "LDA ABS,X "}, 
        {0xBE, "LDX ABS,Y "}, 
        {0xC0, "CPY #     "}, 
        {0xC1, "CMP(IND,X)"}, 
        {0xC4, "CPY ZP    "}, 
        {0xC5, "CMP ZP    "}, 
        {0xC6, "DEC ZP    "}, 
        {0xC8, "INY       "}, 
        {0xC9, "CMP #     "}, 
        {0xCA, "DEX       "}, 
        {0xCC, "CPY ABS   "}, 
        {0xCD, "CMP ABS   "}, 
        {0xCE, "DEC ABS   "}, 
        {0xD0, "BNE       "}, 
        {0xD1, "CMP(IND),Y"}, 
        {0xD5, "CMP ZP,X  "}, 
        {0xD6, "DEC ZP,X  "}, 
        {0xD8, "CLD       "}, 
        {0xD9, "CMP ABS,Y "}, 
        {0xDD, "CMP ABS,X "}, 
        {0xDE, "DEC ABS,X "}, 
        {0xE0, "CPX #     "}, 
        {0xE1, "SBC(IND,X)"}, 
        {0xE4, "CPX ZP    "}, 
        {0xE5, "SBC ZP    "}, 
        {0xE6, "INC ZP    "}, 
        {0xE8, "INX       "}, 
        {0xE9, "SBC #     "}, 
        {0xEA, "NOP       "}, 
        {0xEC, "CPX ABS   "}, 
        {0xED, "SBC ABS   "}, 
        {0xEE, "INC ABS   "}, 
        {0xF0, "BEQ       "}, 
        {0xF1, "SBC(IND),Y"}, 
        {0xF5, "SBC ZP,X  "}, 
        {0xF6, "INC ZP,X  "}, 
        {0xF8, "SED       "}, 
        {0xF9, "SBC ABS,Y "}, 
        {0xFD, "SBC ABS,X "}, 
        {0xFE, "INC ABS,X "}  
    };
bool clearScr = false;
bool demo = false;
std::deque<std::string> debugLines;

uint8_t cursorX = 0;
uint8_t cursorY = 0;

uint8_t inputCursorX = 29;
uint8_t inputCursorY = 5;



void setCursor(uint8_t x, uint8_t y) {
  cursorX = x;
  cursorY = y;
}

void print(const char * txt, bool inverted, uint8_t color) {   // defaults in proto.h
  size_t length = std::strlen(txt);

  uint16_t addr = cursorY * 45 + cursorX;
  for (size_t i = 0; i < length; i++) {
    char currChar = txt[i];
    if (inverted && currChar < 0x60 && currChar >= 0x40) currChar-=0x40;
    menuScreen[addr+i] = currChar+(inverted ? 0 : 0x80);
    menuColor[addr+i] = color;
    cursorX++;
    if (cursorX > 44) {
      cursorY++;
      cursorX = 0;
    }
  }
}

uint8_t getChar() {
  uint16_t addr = cursorY * 45 + cursorX;
  return menuScreen[addr];
}

uint16_t getAddressValue() {
  uint16_t addr = cursorY * 45 + cursorX;
  uint16_t ret = 0;
  for (size_t i = 0; i < 4; i++) {
    ret <<= 8;
    ret |= menuScreen[addr + i];
  }
  return ret;
}

void clearScreen() {
  if (menuScreen) memset(menuScreen, 0xa0, 0x546 * sizeof(unsigned char));
  if (menuColor)  memset(menuColor, 0x00, 0x546 * sizeof(unsigned char));
  clearScr = true;
}

void listFiles(bool downDirection)
{
  uint8_t startX = 0;
  uint8_t startY = 2;
  uint8_t pageSize = 9;
  uint8_t fileNameMax = 45;

  for (int y = startY; y < startY + pageSize; y++)
  {
    for (int x = startX; x < fileNameMax; x++)
    {
      setCursor(x,y);
      print(" ");
    }
  }
  
  pageSize--; // transform to index
  std::vector<std::string> files;
  if (!HdDisk)
  {
    files = diskFiles;
  }
  else
  {
    files = hdFiles;
  }
  if (shownFile > files.size())
    shownFile = 0xff;
  if (downDirection)
  {
    if (shownFile != 0xff && shownFile >= firstShowFile + pageSize)
    {
      firstShowFile = shownFile - pageSize;
    }
  }
  else
  {
    if (shownFile != 0xff && shownFile < firstShowFile && firstShowFile > 0)
      firstShowFile--;
  }
  int id = 0;
  if (shownFile == 0xff) {
    if ((HdDisk && selectedHdFileName == "/") || (!HdDisk && selectedDiskFileName == "/")) {
      shownFile = 0;
    } 
    else {
      for (auto &&i : files)
      {
        if (i.compare(HdDisk ? selectedHdFileName.c_str() : selectedDiskFileName.c_str()) == 0) {
          shownFile = id;
          if (shownFile > pageSize)
            firstShowFile = shownFile - pageSize;
          break;
        }
        id++;
      }
    }
  }
  id = 0;
  int shown = 0;
  // sprintf(buf, "firstShowFile: %d, shownFile: %d, filename: %s", firstShowFile, shownFile, HdDisk ? selectedHdFileName.c_str() : selectedDiskFileName.c_str());
  // Serial.println(buf);
  
  for (auto &&i : files)
  {
    if (id < firstShowFile)
    {
      id++;
      continue;
    }
    if (shown > pageSize)
      break;
    setCursor(startX, startY + id - firstShowFile);
    // sprintf(buf, "i: %s, selectedDiskFileName: %s", i.c_str(), selectedDiskFileName.c_str());
    // Serial.println(buf);

    if (i.size() > fileNameMax)
      i = i.substr(0, fileNameMax-6) + "..." + i.substr(i.size() - 3, 3);
    print(i.c_str(), id == shownFile);
    for (size_t b = 0; b<fileNameMax-i.size();b++)
      print(" ", id == shownFile);
    shown++;
    id++;
  }
  optionsUiMarkDirty();   // refresh the modern touch UI on PS/2 file navigation
}

void colorDemo() {
  char *txtColor = (char *)malloc(8);
  for (uint8_t h = 0; h < 0x10; h++) {
    setCursor(h*2 + 2, 0);
    sprintf(txtColor, "x%X", h);
    print(txtColor, false, 0);
    for (uint8_t v = 0; v < 0x10; v++) {
      setCursor(0, v + 1);
      sprintf(txtColor, "%2X", v);
      print(txtColor, false, 0);

      setCursor(h*2 + 2, v + 1);
      print("  ", false, (h | v << 4));
    }
  }
}

void showHideOptionsWindow() {
  OptionsWindow = !OptionsWindow;
  clearScreen();
  if (OptionsWindow) {
    optionsUiOpen();   // modern touch UI (see optionsui.ino)
  } else {
    saveConfig();              // persist all settings on close (no reboot needed)
    oskIgnoreCurrentTouch();   // closing tap must not pop up the on-screen keyboard
  }

  paused = OptionsWindow;
}

void showHideDebugWindow() {
  DebugWindow = !DebugWindow;

  clearScreen();
  if (DebugWindow) {
    debugScreenRender();
    printDebugLine();
  }
}

void debugScreenRender()
{
  setCursor(0, 0);
  print("debug:");
  setCursor(24,2);
  print("< F5 >");
  setCursor(24,3);
  if (debugPaused) {
    print("Continue");
  } else {
    print("Pause   ");
  }
  setCursor(34,2);
  print("< F10 >");
  setCursor(34,3);
  print(" Step ");

  setCursor(0, 5);
  print("Set breakpoint at address: 0x");
  setCursor(29, 5);
  print("_");
 
}

void stackdebug() {
  std::string sFlags = ""; 
  for (int f = 0;f<8;f++) {
    sFlags +=(SR & (1 << f)) != 0 ? "1" : "0";
  }
  uint8_t opcodeLocal = read8(lastPC);
  std::string opcodeName = opcodeMap[opcodeLocal];

  // Addressing modes
  sprintf(buf, "%04X:%02X:%s%04X:%02X %02X %02X %02X %s", lastPC, opcodeLocal, opcodeName.c_str(), argument_addr, read8(argument_addr), A, X, Y, sFlags.c_str());
  debugLines.push_front(buf);
  if (debugLines.size() > 20) {
    debugLines.pop_back();
  }
  printDebugLine();
}

void printDebugLine()
{
  setCursor(0,8);
  print("PC  :Op:Instruct  Addr:Va A  X  Y  NOFBDIZC");
  int size = debugLines.size();
  for (int i = 0; i < size; i++) {
    setCursor(0,9+size-i);  
    if (i < size) {
      print(debugLines[i].c_str(), i == 0);
    }
  }
}

void optionsScreenRender()
{
  setCursor(0, 0);
  print("Available files:", fnSelected == 0, 0xA0);
  setCursor(0,12);
  print("< F1 >", fnSelected == 1);
  setCursor(0,13);
  print(" HD   ", HdDisk);
  setCursor(0,14);
  print(" DISK ", !HdDisk);

  setCursor(8,12);
  print("MODEL ", false);   // read-only: the II+/IIe choice is made on the boot splash now
  setCursor(8,13);
  print(" IIe  ", AppleIIe);
  setCursor(8,14);
  print(" II+  ", !AppleIIe);

  setCursor(16,12);
  print("< F3 >", fnSelected == 3);
  setCursor(16,13);
  print(" Fast ", Fast1MhzSpeed);
  setCursor(16,14);
  print(" 1Mhz ", !Fast1MhzSpeed);

  setCursor(24,12);
  print("< F4 >", fnSelected == 4);
  setCursor(24,13);
  print(" Spkr ", sound);
  setCursor(24,14);
  print(" Mute ", !sound);

  setCursor(32,12);
  print("< F5 >", fnSelected == 5);
  setCursor(32,13);
  print(" Joy ", joystick);
  setCursor(32,14);
  print(" Off ", !joystick);

  setCursor(0,16);
  print("< F6 >", fnSelected == 6);
  setCursor(0,17);
  print(" Col. ", videoColor);
  setCursor(0,18);
  print(" Mono ", !videoColor);
  setCursor(8,16);
  print("< F7 >", fnSelected == 7);
  setCursor(8,17);
  print(" Upsc.", upscale);
  setCursor(8,18);
  print(" Regu.", !upscale);

  setCursor(16,16);
  print("< F8 >", fnSelected == 8);
  setCursor(16,17);
  print(" Smooth", smoothUpscale);
  setCursor(16,18);
  print(" Regu.", !smoothUpscale);
  setCursor(0,20);
  print("<ESC> Exit from menu");
  setCursor(0,21);
  print("<Enter> Select Disk");
  setCursor(0,22);
  print("<Crtl> + <Enter> Save");
  setCursor(0,23);
  print(" and Reboot");
  optionsUiMarkDirty();   // refresh the modern touch UI when PS/2 changes a setting
}
  