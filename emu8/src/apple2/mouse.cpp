#include "../../emu.h"

bool lastMouseButton = false;

char mouseSoftSwitchesRead(ushort address) {
    return mouse ? ProcessC0xx(address, 0, true) : 0;
}

void mouseSoftSwitchesWrite(ushort address, byte value) {
  ProcessC0xx(address, value, false);
}

char ProcessC0xx(ushort address, byte b, bool Read_Write)
{
    const int _slotNumber = 4; // Mouse in slot 4
    if (address == 0xc080 + _slotNumber * 0x10)
    {
    }
    else if (address == 0xc081 + _slotNumber * 0x10)
    {
    }
    else if (address == 0xc082 + _slotNumber * 0x10)
    {
        if (Read_Write)
        {
            ram[0x478 + _slotNumber] = mouseX & 0xff;
            ram[0x578 + _slotNumber] = (mouseX & 0xff00) >> 8;
            ram[0x4f8 + _slotNumber] = mouseY & 0xff;
            ram[0x5f8 + _slotNumber] = (mouseY & 0xff00) >> 8;

            byte actualMouseButton = (byte)((mouseButton ? 0x80 : 0x00) + (lastMouseButton ? 0x40 : 0x00));
            lastMouseButton = mouseButton;
            ram[0x778 + _slotNumber] = (byte)actualMouseButton;
        }
    }
    else if (address == 0xc083 + _slotNumber * 0x10)
    {
        //if (Read_Write)
        //    return 63;
    }
    else if (address == 0xc084 + _slotNumber * 0x10)
    {
        //if (Read_Write)
        //    return 63;
    }
    else if (address == 0xc085 + _slotNumber * 0x10)
    {
        //if (Read_Write)
        //    return 63;
    }
    else if (address == 0xc086 + _slotNumber * 0x10)
    {
        //if (Read_Write)
        //    return 63;
    }
    else if (address == 0xc087 + _slotNumber * 0x10)
    {
        //if (Read_Write)
        //    return 63;
    } 
    else if (address == 0xc08a + _slotNumber * 0x10)
    {
        //if (Read_Write)
        //    return 63;
    }

    return 0;
}