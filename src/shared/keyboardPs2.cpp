#include "../../emu.h"


const unsigned char scancode_to_apple[] = {
    //$0    $1    $2    $3    $4    $5    $6    $7    $8    $9    $A    $B    $C    $D    $E    $F
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //$00
    0x00, 0x00, 0x00, 0x00, 0x00, 0xD1, 0xB1, 0x00, 0x00, 0x00, 0xDA, 0xD3, 0xC1, 0xD7, 0xB2, 0x00, //$10
    0x00, 0xC3, 0xD8, 0xC4, 0xC5, 0xB4, 0xB3, 0x00, 0x00, 0xA0, 0xD6, 0xC6, 0xD4, 0xD2, 0xB5, 0x00, //$20
    0x00, 0xCE, 0xC2, 0xC8, 0xC7, 0xD9, 0xB6, 0x00, 0x00, 0x00, 0xCD, 0xCA, 0xD5, 0xB7, 0xB8, 0x00, //$30
    0x00, 0xAC, 0xCB, 0xC9, 0xCF, 0xB0, 0xB9, 0x00, 0x00, 0xAE, 0xAF, 0xCC, 0xBB, 0xD0, 0xAD, 0x00, //$40
    0x00, 0x00, 0xA7, 0x00, 0x00, 0xBD, 0x00, 0x00, 0x00, 0x00, 0x8D, 0x00, 0x00, 0x00, 0x00, 0x00, //$50
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x00, 0x00, 0xB1, 0x00, 0xB4, 0xB7, 0x00, 0x00, 0x00, //$60
    0xB0, 0xAE, 0xB2, 0xB5, 0xB6, 0xB8, 0x9B, 0x00, 0x00, 0xAB, 0xB3, 0xAD, 0xAA, 0xB9, 0x00, 0x00, //$70
    // High mirror, shift modified keys
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //$80 0
    0x00, 0x00, 0x00, 0x00, 0x00, 0xD1, 0xA1, 0x00, 0x00, 0x00, 0xDA, 0xD3, 0xC1, 0xD7, 0xC0, 0x00, //$90 1
    0x00, 0xC3, 0xD8, 0xC4, 0xC5, 0xA4, 0xA3, 0x00, 0x00, 0xA0, 0xD6, 0xC6, 0xD4, 0xD2, 0xA5, 0x00, //$A0 2
    0x00, 0xCE, 0xC2, 0xC8, 0xC7, 0xD9, 0xDE, 0x00, 0x00, 0x00, 0xCD, 0xCA, 0xD5, 0xA6, 0xAA, 0x00, //$B0 3
    0x00, 0xBC, 0xCB, 0xC9, 0xCF, 0xA9, 0xA8, 0x00, 0x00, 0xBE, 0xBF, 0xCC, 0xBA, 0xD0, 0xAD, 0x00, //$C0 4
    0x00, 0x00, 0xA2, 0x00, 0x00, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x8D, 0x00, 0x00, 0x00, 0x00, 0x00, //$D0 5
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x00, 0x00, 0xB1, 0x00, 0xB4, 0xB7, 0x00, 0x00, 0x00, //$E0 6
    0xB0, 0xAE, 0xB2, 0xB5, 0xB6, 0xB8, 0x9B, 0x00, 0x00, 0xAB, 0xB3, 0xAD, 0xAA, 0xB9, 0x00, 0x00  //$F0 7
};


// In apple II scancode format


unsigned char keyboard_read()
{
  return keymem;
}

void keyboardStrobe()
{
  keymem &= 0x7F;
}

void keyboardSetup()
{
  // Boards without a PS/2 keyboard header (e.g. JC4827W543) define KEYBOARD_*_PIN as -1;
  // skip the interrupt wiring there (Apple II keyboard input comes from USB + the touch OSK).
  if (KEYBOARD_DATA_PIN < 0 || KEYBOARD_IRQ_PIN < 0) return;
  pinMode(KEYBOARD_DATA_PIN, INPUT_PULLUP);
  attachInterrupt(KEYBOARD_IRQ_PIN, keyboard_bit, FALLING);
}

void keyboard_bit()
{
  if (digitalRead(KEYBOARD_DATA_PIN))
    keyboard_data[2] |= _BV(keyboard_buf_indx);
  else
    keyboard_data[2] &= ~(_BV(keyboard_buf_indx));
  if (++keyboard_buf_indx == 11)
  {
    // Ignore parity checks for now
    keyboard_data[2] = (keyboard_data[2] >> 1) & 0xFF;
    // sprintf(buf, "initial keybdata: %02x", keyboard_data[2]);
    // Serial.println(buf);
    //  extended keys
    if (keyboard_data[2] == 0xF0 || keyboard_data[2] == 0xE0)
      keyboard_mbyte = 1;
    else
    {
      // decrement counter for multibyte commands
      if (keyboard_mbyte)
        keyboard_mbyte--;
      // multibyte command is finished / normal command, process it
      if (!keyboard_mbyte)
      {
        if (keyboard_data[1] != 0xF0 && keyboard_data[1] != 0xE0)
        {
          // Standard keys
          if (keyboard_data[2] == 0x12 || keyboard_data[2] == 0x59)
          {
            shift_enabled = true; // shift modifiers
            Pb1 = true;
          }
          else if (keyboard_data[2] == 0x14)
          {
            ctrl_enabled = true;
            Pb0 = true;
          }
          else
          {
            keymem = scancode_to_apple[keyboard_data[2] + ((shift_enabled) ? 0x80 : 0x00)];
            if (ctrl_enabled)
            {
              if (keyboard_data[2] == 0x07) // CTRL-F12
              {
                cpuReset();
              }
              else if (keyboard_data[2] == 0x03) // CTRL-F5
              {
                requestSplashOnNextBoot();   // explicit reboot -> show the boot splash
                ESP.restart();
              }
              else if (keyboard_data[2] == 0x76) // CTRL-ESC
              {
                showHideOptionsWindow();
                keymem = 0;
              }
              else if (keyboard_data[2] == 0x5a) // CTRL-ENTER
              {
                if (OptionsWindow)
                {
                  if (HdDisk == initializedHdDisk) {
                    if (HdDisk) {
                      setHdFile();
                      saveHdFile();
                    }
                    else {
                      setDiskFile();
                      saveDiskFile();
                    }
                  }
                  else {
                    saveEEPROM();
                    EEPROM.commit();
                  }
                  ESP.restart();
                }
              }
              else
              {
                keymem -= 0x40;
              }
            }
            else
            {
              if (OptionsWindow) // Option Window Opened
              {
                if (keyboard_data[2] == 0x76) // ESC
                {
                  showHideOptionsWindow();
                }
                else if (keyboard_data[2] == 0x5a) // enter
                {
                  if (HdDisk)
                    setHdFile();
                  else
                    setDiskFile();
                  diskChanged = true;
                  showHideOptionsWindow();
                }
                keymem = 0;
              }
                if (keyboard_data[2] == 0x05) // F1
                {
                  HdDisk = !HdDisk;
                  optionsScreenRender();
                }
                else if (keyboard_data[2] == 0x06) // F2
                {
                  AppleIIe = !AppleIIe;
                  activeFlags = AppleIIe ? flagsIIe : flagsIIplus;
                  optionsScreenRender();
                }
                else if (keyboard_data[2] == 0x04) // F3
                {
                  Fast1MhzSpeed = !Fast1MhzSpeed;
                  optionsScreenRender();
                }
                else if (keyboard_data[2] == 0x0c) // F4
                {
                  sound = !sound;
                  optionsScreenRender();
                }
                else if (keyboard_data[2] == 0x03) // F5
                {
                  joystick = !joystick;
                  optionsScreenRender();
                }
                else if (keyboard_data[2] == 0x0b) // F6
                {
                  videoColor = !videoColor;
                  optionsScreenRender();
                }
                else if (keyboard_data[2] == 0x0a) // F8
                {
                  optionsScreenRender();
                }
                else if (keyboard_data[2] == 0x01) // F9
                {
                  optionsScreenRender();
                }
                else if (keyboard_data[2] == 0x09) // F10
                {
                  optionsScreenRender();
                }
                else if (keyboard_data[2] == 0x78) // F11
                {
                  if (volume > 0) {
                    volume -= 0x10;
                    if (volume > 0xf0)
                      volume = 0;
                  }
                  optionsScreenRender();
                }
                else if (keyboard_data[2] == 0x07) // F12
                {
                  if (volume < 0xf0)
                    volume += 0x10;
                  optionsScreenRender();
                }
                
              
            }

            //  Serial.print("keyboard_data:");
            //  Serial.printf("%02x\n",keyboard_data[2]);
             //Serial.print("shift:");
            //  Serial.println((shift_enabled) ? "1" : "0");
            //  Serial.print("ctrl:");
            //  Serial.println((ctrl_enabled) ? "1" : "0");
            //  Serial.print("key:");
            //  Serial.println(keymem);
          }
        }
        else if (keyboard_data[0] != 0xF0 && keyboard_data[1] == 0xE0)
        {
          // Extended keys
          if (keyboard_data[2] == 0x6B) // LEFT ARROW
          {
            keymem = 0x88; // back key
            if (!joystick) {
              timerpdl0 = JOY_MIN;
            }
          }
          else if (keyboard_data[2] == 0x74) // RIGHT ARROW
          {
            keymem = 0x95; // forward key
            if (!joystick)
              timerpdl0 = JOY_MAX;
          }
          else if (keyboard_data[2] == 0x75) // UP ARROW
          {
            keymem = 0x8b;
            if (!joystick)
              timerpdl1 = JOY_MIN;

          }
          else if (keyboard_data[2] == 0x72) // DOWN ARROW
          {
            keymem = 0x8a;
            if (!joystick)
              timerpdl1 = JOY_MAX;
          }

          // Power management keys, hardware reset
          if (OptionsWindow)
          {
            if (keyboard_data[2] == 0x72) // Down Arrow
            {
              if (!HdDisk)
                nextDiskFile();
              else
                nextHdFile();

              listFiles(true);
            }
            else if (keyboard_data[2] == 0x75) // Up Arrow
            {
              if (!HdDisk)
                prevDiskFile();
              else
                prevHdFile();
              listFiles(false);
            }
            keymem = 0;
          }
        }
        else if (keyboard_data[1] == 0xF0 && (keyboard_data[2] == 0x12 || keyboard_data[2] == 0x59))
        {
          shift_enabled = false;
          if (!joystick)
            Pb1 = false;
        }
        else if (keyboard_data[1] == 0xF0 && keyboard_data[2] == 0x14)
        {
          ctrl_enabled = false;
          if (!joystick)
            Pb0 = false;
        }
        else if (keyboard_data[1] == 0xF0 && (keyboard_data[2] == 0x75 || keyboard_data[2] == 0x72))
        {
          if (!joystick)
            timerpdl1 = JOY_MID;
        }
        else if (keyboard_data[1] == 0xF0 && (keyboard_data[2] == 0x6b || keyboard_data[2] == 0x74))
        {
          if (!joystick)
            timerpdl0 = JOY_MID;
        }
        else
        {
          if (!joystick)
          {
            timerpdl0 = JOY_MID;
            timerpdl1 = JOY_MID;
          }
        }
      }
    }

    // shuffle buffer
    keyboard_data[0] = keyboard_data[1];
    keyboard_data[1] = keyboard_data[2];
    keyboard_buf_indx = 0;
  }
}
