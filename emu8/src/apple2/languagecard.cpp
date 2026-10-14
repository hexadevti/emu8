#include "../../emu.h"


void languagecardWrite(ushort address, byte b)
{
    if (address >= 0xd000)
    {
        if (address >= 0xd000 && address < 0xe000)
        {
            if (MemoryBankBankSelect1_2)
                memoryBankSwitchedRAM2_1[address - 0xd000] = b;
            else
                memoryBankSwitchedRAM2_2[address - 0xd000] = b;
        }
        else
            memoryBankSwitchedRAM1[address - 0xe000] = b;
    }
    processSwitch(address, b);
}

char languagecardRead(ushort address)
{
    char ret = 0;
    if (address >= 0xd000 && address < 0xe000)
    {
        if (MemoryBankBankSelect1_2)
            ret = memoryBankSwitchedRAM2_1[address - 0xd000];
        else
            ret = memoryBankSwitchedRAM2_2[address - 0xd000];
    }
    else if (address >= 0xd000)
        ret = memoryBankSwitchedRAM1[address - 0xe000];

    return processSwitch(address, ret);
}

char processSwitch(ushort address, byte b)
{
    if (address >= 0xc080 && address < 0xc090)
    {
        ushort last4bits = (address & 0b00001111);

        bool bits[4];
        for(int i = 0; i < 4; i++)
        {
          bits[i] = (last4bits >> i) & 1;
        }
        
        //sprintf(buf, "Languege card switch: %04X", address);
        // Serial.println(buf);
        MemoryBankBankSelect1_2 = bits[3];
        if (bits[1] && bits[0])
        {
            MemoryBankReadRAM_ROM = true;
            MemoryBankWriteRAM_NoWrite = true;
        }
        else if (!bits[1] && bits[0])
        {
            MemoryBankReadRAM_ROM = false;
            MemoryBankWriteRAM_NoWrite = true;
        }
        else if (bits[1] && !bits[0])
        {
            MemoryBankReadRAM_ROM = false;
            MemoryBankWriteRAM_NoWrite = false;
        }
        else if (!bits[1] && !bits[0])
        {
            MemoryBankReadRAM_ROM = true;
            MemoryBankWriteRAM_NoWrite = false;
        }
    }
    return b;
}


