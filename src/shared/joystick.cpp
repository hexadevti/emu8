#include "../../emu.h"

static void analogJoystickTask(void *pvParameters);   // defined below; used by joystickSetup

void processJoystick(float speedAdjust)
{
    if (CgReset0)
    {
        joystickCycles0++;
    }
    if (CgReset1)
    {
        joystickCycles1++;
    }
    if (CgReset2)
    {
        joystickCycles2++;
    }
    if (CgReset3)
    {
        joystickCycles3++;
    }

    if (joystickCycles0 >= static_cast<int>(timerpdl0 * speedAdjust))
    {
        joystickCycles0 = 0;
        Cg0 = false;
        CgReset0 = false;
    }

    if (joystickCycles1 >= static_cast<int>(timerpdl1 * speedAdjust))
    {
        joystickCycles1 = 0;
        Cg1 = false;
        CgReset1 = false;
    }

    if (joystickCycles2 >= static_cast<int>(timerpdl2 * speedAdjust))
    {
        joystickCycles2 = 0;
        Cg2 = false;
        CgReset2 = false;
    }

    if (joystickCycles3 >= static_cast<int>(timerpdl3 * speedAdjust))
    {
        joystickCycles3 = 0;
        Cg3 = false;
        CgReset3 = false;
    }
}

int joyCenterX = 0;
int joyCenterY = 0;

void joystickSetup()
{
    timerpdl0 = JOY_MID;
    timerpdl1 = JOY_MID;
    joyCenterX = 4095 - analogRead(ANALOG_X_PIN);
    joyCenterY = 4095 - analogRead(ANALOG_Y_PIN);
    pinMode(ANALOG_X_PIN, INPUT);
    pinMode(ANALOG_Y_PIN, INPUT);
    pinMode(DIGITAL_BUTTON12_PIN, INPUT);
    pPb0 = Pb0;
    pPb1 = Pb1;
    pPb2 = Pb2;
    pPb3 = Pb3;
    xTaskCreatePinnedToCore(analogJoystickTask, "analogJoystickTask", 4096, NULL, 3, NULL, 0); // core 0
}

static void buttonDown(uint8_t btn)
{
    if (btn == 3)
    {
        // Button 3 opens/closes the settings menu — except on NES, where it is the Start
        // button (the NES menu is opened by a screen tap instead, see oskPoll).
        if (currentPlatform != PLATFORM_NES) showHideOptionsWindow();
        return;
    }
    if (OptionsWindow)
    {
        if (btn == 0)              // fire: activate the focused control
            optionsUiActivate();
        return;
    }
    if (!joystick)
    {
        if (btn == 0 && !mouse) // when the mouse is active, button 0 is the click, not a keystroke
        {
            keymem = 0xa0;
        }
        else if (btn == 1)
        {
            keymem = 0x8d;
        }
        else if (btn == 2)
        {
            keymem = 0x9b;
        }
    }
}

static void buttonUp(uint8_t btn)
{
    (void)btn;   // options-window actions now fire on button-down (optionsUiActivate)
}

static void changeDirection(bool x, uint8_t dir)
{
    if (OptionsWindow)
    {
        if (x)
        { // left / right: move the focus highlight between controls
            if (dir == 0)      optionsUiNav(-1);
            else if (dir == 2) optionsUiNav(+1);
        }
        else
        { // up / down: act on the focused control
            if (dir == 0)      optionsUiAdjust(-1);
            else if (dir == 2) optionsUiAdjust(+1);
        }
    }
    else if (!joystick)
    {
        if (x)
        {
            if (dir == 0)
            {
                keymem = 0x88; // back key
            }
            else if (dir == 2)
            {
                keymem = 0x95; // forward key
            }
        }
        else
        {
            if (dir == 0)
            {
                keymem = 0x8b; // up
            }
            else if (dir == 2)
            {
                keymem = 0x8a; // down key
            }
        }
    }
}

static void analogJoystickTask(void *pvParameters)
{
    while (running)
    {
        int joyRawX = analogRead(ANALOG_X_PIN);
        int joyRawY = analogRead(ANALOG_Y_PIN);
        analogX = 4095 - joyRawX;
        analogY = 4095 - joyRawY;
        digital_button1 = analogRead(DIGITAL_BUTTON12_PIN);
        //Serial.printf("analog x=%d, y=%d, centerX=%d, centerY=%d\n", analogX, analogY, joyCenterX, joyCenterY);
        // Serial.printf(" Pb0=%d, Pb1=%d Pb2=%d, Pb3=%d (%d)\n", Pb0, Pb1, Pb2, Pb3, digital_button1);
        //Serial.printf("timer PDL(0)=%f PDL(1)=%f\n", timerpdl0, timerpdl1);
        // Serial.println(buf);
        if (joystick)
        {
            if (analogX > joyCenterX) {
               timerpdl0 = 512 + ((analogX - joyCenterX) * 512 / (4095 - joyCenterX)) ;
            } else {
               timerpdl0 = 512 * analogX / joyCenterX;
            }
            if (analogY > joyCenterY) {
               timerpdl1 = 512 + ((analogY - joyCenterY) * 512 / (4095 - joyCenterY)) ;
            } else {
               timerpdl1 = 512 * analogY / joyCenterY;
            }
        }
    
        if (digital_button1 > 3000 && digital_button1 <= 4095)
        { // 0000
            Pb0 = false;
            Pb1 = false;
            Pb2 = false;
            Pb3 = false;
        }
        else if (digital_button1 > 205 && digital_button1 < 215)
        { // 0001
            Pb0 = true;
            Pb1 = false;
            Pb2 = false;
            Pb3 = false;
        }
        else if (digital_button1 > 1890 && digital_button1 < 1900)
        { // 0010
            Pb0 = false;
            Pb1 = true;
            Pb2 = false;
            Pb3 = false;
        }
        else if (digital_button1 > 175 && digital_button1 < 185)
        { // 0011
            Pb0 = true;
            Pb1 = true;
            Pb2 = false;
            Pb3 = false;
        }
        else if (digital_button1 > 1505 && digital_button1 < 1515)
        { // 0100
            Pb0 = false;
            Pb1 = false;
            Pb2 = true;
            Pb3 = false;
        }
        else if (digital_button1 > 165 && digital_button1 < 175)
        { // 0101
            Pb0 = true;
            Pb1 = false;
            Pb2 = true;
            Pb3 = false;
        }
        else if (digital_button1 > 1015 && digital_button1 < 1025)
        { // 0110
            Pb0 = false;
            Pb1 = true;
            Pb2 = true;
            Pb3 = false;
        }
        else if (digital_button1 > 140 && digital_button1 < 150)
        { // 0111
            Pb0 = true;
            Pb1 = true;
            Pb2 = true;
            Pb3 = false;
        }
        else if (digital_button1 > 570 && digital_button1 < 580)
        { // 1000
            Pb0 = false;
            Pb1 = false;
            Pb2 = false;
            Pb3 = true;
        }
        else if (digital_button1 > 95 && digital_button1 < 110)
        { // 1001
            Pb0 = true;
            Pb1 = false;
            Pb2 = false;
            Pb3 = true;
        }
        else if (digital_button1 > 455 && digital_button1 < 465)
        { // 1010
            Pb0 = false;
            Pb1 = true;
            Pb2 = false;
            Pb3 = true;
        }
        else if (digital_button1 > 81 && digital_button1 < 95)
        { // 1011
            Pb0 = true;
            Pb1 = true;
            Pb2 = false;
            Pb3 = true;
        }
        else if (digital_button1 > 415 && digital_button1 < 425)
        { // 1100
            Pb0 = false;
            Pb1 = false;
            Pb2 = true;
            Pb3 = true;
        }
        else if (digital_button1 > 70 && digital_button1 < 82)
        { // 1101
            Pb0 = true;
            Pb1 = false;
            Pb2 = true;
            Pb3 = true;
        }
        else if (digital_button1 > 335 && digital_button1 < 350)
        { // 1110
            Pb0 = false;
            Pb1 = true;
            Pb2 = true;
            Pb3 = true;
        }
        else if (digital_button1 > 60 && digital_button1 < 70)
        { // 1111
            Pb0 = true;
            Pb1 = true;
            Pb2 = true;
            Pb3 = true;
        }

        // Debounce the resistor-ladder buttons: a decoded reading is only accepted
        // once it repeats on the next poll. This rejects the brief mid-sweep values
        // the ADC passes through during a press/release (which otherwise register as
        // spurious other-button presses, e.g. opening the options menu / pausing when
        // clicking button 0).
        {
            uint8_t curBtns = (Pb0 ? 1 : 0) | (Pb1 ? 2 : 0) | (Pb2 ? 4 : 0) | (Pb3 ? 8 : 0);
            static uint8_t lastBtns = 0, stableBtns = 0;
            if (curBtns == lastBtns)
                stableBtns = curBtns;
            lastBtns = curBtns;
            Pb0 = stableBtns & 1;
            Pb1 = stableBtns & 2;
            Pb2 = stableBtns & 4;
            Pb3 = stableBtns & 8;
        }

        if (pPb0 != Pb0)
            if (Pb0)
                buttonDown(0);
            else
                buttonUp(0);
        if (pPb1 != Pb1)
            if (Pb1)
                buttonDown(1);
            else
                buttonUp(1);
        if (pPb2 != Pb2)
            if (Pb2)
                buttonDown(2);
            else
                buttonUp(2);
        if (pPb3 != Pb3)
            if (Pb3)
                buttonDown(3);
            else
                buttonUp(3);

        pPb0 = Pb0;
        pPb1 = Pb1;
        pPb2 = Pb2;
        pPb3 = Pb3;

        // DEBUG: report the button ADC value + decoded buttons on a change (edge), not every poll.
        {
            static uint8_t dbgPrev = 0;
            uint8_t nowBtns = (Pb0 ? 1 : 0) | (Pb1 ? 2 : 0) | (Pb2 ? 4 : 0) | (Pb3 ? 8 : 0);
            if (nowBtns != dbgPrev)
            {
                Serial.printf("BTN raw=%d Pb0=%d Pb1=%d Pb2=%d Pb3=%d\n", digital_button1, Pb0, Pb1, Pb2, Pb3);
                dbgPrev = nowBtns;
            }
        }

        // Joystick-controlled mouse pointer: stick deflection moves the AppleMouse II
        // cursor (velocity proportional to deflection); button 0 is the mouse click.
        // Active only when the mouse card is on and the options menu is closed.
        if (mouse && !OptionsWindow)
        {
            int dx = analogX - joyCenterX;
            int dy = analogY - joyCenterY;
            const int deadzone = 300; // ignore small drift around center
            const int sensitivity = 700; // larger = slower pointer
            if (abs(dx) > deadzone)
                mouseX += dx / sensitivity;
            if (abs(dy) > deadzone)
                mouseY += dy / sensitivity; // up = toward top of screen
            if (mouseX < 0) mouseX = 0; else if (mouseX > 560) mouseX = 560;
            if (mouseY < 0) mouseY = 0; else if (mouseY > 192) mouseY = 192;
            mouseButton = Pb0; // button 0 = select / click
        }

        // Direction = deflection past a deadzone from the rest position captured at
        // boot. The old code only reacted at the exact ADC rails (0/4095), which a real
        // stick rarely hits, so nothing ever registered. Threshold-relative is robust.
        const int joyThresh = 900;
        if (analogY > joyCenterY + joyThresh)      joyX = 2;
        else if (analogY < joyCenterY - joyThresh) joyX = 0;
        else                                       joyX = 1;

        if (analogX > joyCenterX + joyThresh)      joyY = 2;
        else if (analogX < joyCenterX - joyThresh) joyY = 0;
        else                                       joyY = 1;

        (void)joyRawX; (void)joyRawY;   // (joystick raw-value diagnostic removed)

        // C64: map the 8-way stick + fire onto joystick port 2 (CIA1 $DC00). Active-low:
        // bit0=up, bit1=down, bit2=left, bit3=right, bit4=fire. Only while JOYSTICK is on
        // and the menu is closed; otherwise release all (0xff).
        if (currentPlatform == PLATFORM_C64)
        {
            uint8_t m = 0xff;
            if (joystick && !OptionsWindow)
            {
                if (joyX == 0) m &= ~0x01;   // up
                if (joyX == 2) m &= ~0x02;   // down
                if (joyY == 0) m &= ~0x04;   // left
                if (joyY == 2) m &= ~0x08;   // right
                if (Pb0)       m &= ~0x10;   // fire (button 0)
            }
            c64SetJoystick(m);
        }

        // NES: map the 8-way stick + buttons onto controller 1. Active-HIGH bits:
        // bit0=A, bit1=B, bit2=Select, bit3=Start, bit4=Up, bit5=Down, bit6=Left, bit7=Right.
        // Pb3 = Start here (the settings menu is opened by a screen tap on NES, see oskPoll).
        if (currentPlatform == PLATFORM_NES)
        {
            uint8_t b = 0;
            if (joystick && !OptionsWindow)
            {
                if (joyX == 0) b |= 0x10;   // up
                if (joyX == 2) b |= 0x20;   // down
                if (joyY == 0) b |= 0x40;   // left
                if (joyY == 2) b |= 0x80;   // right
                if (Pb0)       b |= 0x01;   // A
                if (Pb1)       b |= 0x02;   // B
                if (Pb2)       b |= 0x04;   // Select
                if (Pb3)       b |= 0x08;   // Start
            }
            nesSetController(b);
        }

        if (pJoyX != joyX)
            changeDirection(0, joyX);
        if (pJoyY != joyY)
            changeDirection(1, joyY);

        pJoyX = joyX;
        pJoyY = joyY;
    
        vTaskDelay(pdMS_TO_TICKS(30)); // ~33 Hz for responsive mouse/joystick polling
    }
}