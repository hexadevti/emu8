#include "../../emu.h"

char processSoftSwitches(ushort address, char value, bool Read_Write)   // default in proto.h
{
  switch (address)
  {
  // Speaker
  case 0xC030:
    speakerToggle();
    break;
  // Keyboard Data
  case 0xC000:
    if (Read_Write)
      return keyboard_read();
    else
      Store80On_Off = false;
    break;
  case 0xc001:
    if (!Read_Write)
      Store80On_Off = true;
    break;
  case 0xc002:
    RAMReadOn_Off = false;
    break;
  case 0xc003:
    RAMReadOn_Off = true;
    break;
  case 0xc004:
    RAMWriteOn_Off = false;
    break;
  case 0xc005:
    RAMWriteOn_Off = true;
    break;
  case 0xc006:
    IntCXRomOn_Off = false;
    break;
  case 0xc007:
    IntCXRomOn_Off = true;
    break;
  case 0xc008:
    AltZPOn_Off = false;
    break;
  case 0xc009:
    AltZPOn_Off = true;
    break;
  case 0xc00a:
    SlotC3RomOn_Off = false;
    break;
  case 0xc00b:
    SlotC3RomOn_Off = true;
    break;
  case 0xc00c:
    if (!Read_Write)
      Cols40_80 = true; // Apple IIc IIe
    break;
  case 0xc00d:
    if (!Read_Write)
      Cols40_80 = false; // Apple IIc IIe
    break;
  case 0xc00e:
    AltCharSetOn_Off = false;
    break;
  case 0xc00f:
    AltCharSetOn_Off = true;
    break;
  case 0xC010:
    keyboardStrobe();
    break;
  case 0xc011:
    return (char)(IIEMemoryBankBankSelect1_2 ? 0x00 : 0xff);
    break;
  case 0xc012:
    return (char)(IIEMemoryBankReadRAM_ROM ? 0xff : 0x00);
    break;
  case 0xc013:
    return (byte)(RAMReadOn_Off ? 0xff : 0x00);
    break;
  case 0xc014:
    return (byte)(RAMWriteOn_Off ? 0xff : 0x00);
    break;
  case 0xc015:
    return (char)(IntCXRomOn_Off ? 0xff : 0x00);
    break;
  case 0xc016:
    return (char)(AltZPOn_Off ? 0xff : 0x00);
    break;
  case 0xc017:
    return (char)(SlotC3RomOn_Off ? 0xff : 0x00);
    break;
  case 0xc018:
    return (char)(Store80On_Off ? 0xff : 0x00);
    break;
  case 0xc019:
    return (char)(Vertical_blankingOn_Off ? 0xff : 0x00);
    break;
  case 0xc01a:
    return (char)(Graphics_Text ? 0x00 : 0xff);
    break;
  case 0xc01b:
    return (char)(DisplayFull_Split ? 0x00 : 0xff);
    break;
  case 0xc01c:
    return (char)(Page1_Page2 ? 0x00 : 0xff);
    break;
  case 0xc01d:
    return (char)(LoRes_HiRes ? 0x00 : 0xff);
    break;
  case 0xc01e:
    return (char)(AltCharSetOn_Off ? 0xff : 0x00);
    break;
  case 0xc01f:
    return (char)(Cols40_80 ? 0x00 : 0xff);
    break;
  case 0xc050:
    Graphics_Text = true;
    break;
  case 0xc051:
    Graphics_Text = false;
    break;
  case 0xc052:
    DisplayFull_Split = true;
    break;
  case 0xc053:
    DisplayFull_Split = false;
    break;
  case 0xc054:
    page_lock.lock();
    Page1_Page2 = true;
    page_lock.unlock();
    break;
  case 0xc055:
    page_lock.lock();
    Page1_Page2 = false;
    page_lock.unlock();
    break;
  case 0xc056:
    LoRes_HiRes = true;
    break;
  case 0xc057:
    LoRes_HiRes = false;
    break;
  case 0xc058:
    Cols40_80 = true;
    break;
  case 0xc059:
    Cols40_80 = false;
    break;
  case 0xc05e:
    if (IOUDisOn_Off)
      DHiResOn_Off = true;
    break;
  case 0xc05f:
    if (IOUDisOn_Off)
      DHiResOn_Off = false;
    break;
  case 0xc061:
    return (char)(Pb0 ? 0x80 : 0x00);
    break;
  case 0xc062:
    return (char)(Pb1 ? 0x80 : 0x00);
    break;
  case 0xc063:
    return (char)(Pb2 ? 0x80 : 0x00);
    break;
    //  case 0xc063:
    //     return 0x80; // Apple II+ default. For Apple IIe it is defined by shift key pressed
    // break;
  case 0xc064:
    return (char)(Cg0 ? 0x80 : 0x00);
    break;
  case 0xc065:
    return (char)(Cg1 ? 0x80 : 0x00);
    break;
  case 0xc066:
    return (char)(Cg2 ? 0x80 : 0x00);
    break;
  case 0xc067:
    return (char)(Cg3 ? 0x80 : 0x00);
    break;
  case 0xc070:
    CgReset0 = true;
    CgReset1 = true;
    // CgReset2 = true;
    // CgReset3 = true;
    Cg0 = true;
    Cg1 = true;
    // Cg2 = true;
    // Cg3 = true;
    break;
  case 0xc071:
  case 0xc073:
  case 0xc075:
  case 0xc077:
    /*if (b < mainBoard.IIEAuxBanks)
        IIeExpansionCardBank = b;*/
    break;
  case 0xc07e:
    if (Read_Write)
      return (byte)(IOUDisOn_Off ? 0x00 : 0xff);
    else
      IOUDisOn_Off = true;
    break;
  case 0xc07f:
    if (Read_Write)
      return (byte)(DHiResOn_Off ? 0xff : 0x00);
    else
      IOUDisOn_Off = false;
    break;
  default:
    if (address >= 0xc080 && address < 0xc090) // Slot 0 - LanguageCard
    {
      if (AppleIIe)
      {
        if (address >= 0xc080 && address < 0xc090)
        {
          ushort last4bits = (address & 0b00001111);

          bool bits[4];
          for (int i = 0; i < 4; i++)
          {
            bits[i] = (last4bits >> i) & 1;
          }
          IIEMemoryBankBankSelect1_2 = bits[3];
          if (bits[1] && bits[0])
          {
            IIEMemoryBankReadRAM_ROM = true;
            IIEMemoryBankWriteRAM_NoWrite = true;
          }
          else if (!bits[1] && bits[0])
          {
            IIEMemoryBankReadRAM_ROM = false;
            IIEMemoryBankWriteRAM_NoWrite = true;
          }
          else if (bits[1] && !bits[0])
          {
            IIEMemoryBankReadRAM_ROM = false;
            IIEMemoryBankWriteRAM_NoWrite = false;
          }
          else if (!bits[1] && !bits[0])
          {
            IIEMemoryBankReadRAM_ROM = true;
            IIEMemoryBankWriteRAM_NoWrite = false;
          }
        }
      }
      else
      {
        if (Read_Write)
          return languagecardRead(address);
        else
          languagecardWrite(address, value);
      }
    }
    else if (address >= 0xc0c0 && address < 0xc0d0) {// Slot 4 - Mouse
      if (Read_Write)
        return mouse ? mouseSoftSwitchesRead(address) : 0;
      else
        mouseSoftSwitchesWrite(address, value);
      
    }
    else if (address >= 0xc0e0 && address < 0xc0f0) // Slot 6 - Disk
      if (Read_Write)
        return diskAttached ? diskSoftSwitchesRead(address) : 0;
      else
        diskSoftSwitchesWrite(address, value);
    else if (address >= 0xc0f0 && address < 0xc100) // Slot 7 - HD
    {
      if (Read_Write)
        return hdAttached ? HDSoftSwitchesRead(address) : 0;
      else
        HDSoftSwitchesWrite(address, value);
    }
    break;
  }
  return 0;
}

char readSoftSwitches(ushort address)
{
  return processSoftSwitches(address, 0, true);
}

void writeSoftSwitches(ushort address, char value)
{
  processSoftSwitches(address, value, false);
}
