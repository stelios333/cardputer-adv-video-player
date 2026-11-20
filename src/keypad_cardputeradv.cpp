#include "keypad_cardputeradv.h"

VideoPlayerKeypad::VideoPlayerKeypad()
{
}

VideoPlayerKeypad::~VideoPlayerKeypad()
{
}

void VideoPlayerKeypad::updateKeyStatus()
{
    while (TCA8418_event)
    {
        KeypadEvent event = VideoPlayerKeypad::HandleKeypadInput();
        char key = CARDPUTER_KEYMAP[event.col][event.row].value_first;
        key_state[key].state = event.pressed;
        key_state[key].state_changed = 1;
    }
}

KeypadEvent VideoPlayerKeypad::HandleKeypadInput()
{
    int intStat = keypad.readRegister(TCA8418_REG_INT_STAT);

    if (intStat & 0x02)
    {
        //  reading the registers is mandatory to clear IRQ flag
        //  can also be used to find the GPIO changed
        //  as these registers are a bitmap of the gpio pins.
        keypad.readRegister(TCA8418_REG_GPIO_INT_STAT_1);
        keypad.readRegister(TCA8418_REG_GPIO_INT_STAT_2);
        keypad.readRegister(TCA8418_REG_GPIO_INT_STAT_3);
        //  clear GPIO IRQ flag
        keypad.writeRegister(TCA8418_REG_INT_STAT, 2);
    }

    uint8_t row = 0xFF, col = 0xFF;
    bool pressed = 0;

    if (intStat & 0x01)
    {
        int event = keypad.getEvent();
        pressed = event & 0x80;
        event &= 0x7F;

        mapRawKeyToPhysical(event, row, col);

        //  clear the EVENT IRQ flag
        keypad.writeRegister(TCA8418_REG_INT_STAT, 1);
    }

    //  check pending events
    int intstat = keypad.readRegister(TCA8418_REG_INT_STAT);
    if ((intstat & 0x03) == 0)
        TCA8418_event = false;
    return {pressed, row, col};
}

bool VideoPlayerKeypad::begin(TwoWire &wire)
{
    if (!keypad.begin(TCA8418_DEFAULT_ADDR, &wire))
    {
        return 1;
    }
    keypad.matrix(7, 8);
    keypad.flush();

    pinMode(KEYPAD_INT, INPUT_PULLUP);
    attachInterruptArg(digitalPinToInterrupt(KEYPAD_INT), gpio_isr_handler, this, CHANGE);
    keypad.enableInterrupts();
    return 0;
}

bool VideoPlayerKeypad::getKey(char key)
{
    return key_state[key].state;
}

bool VideoPlayerKeypad::keyChanged(char key) 
{
    bool old_state = key_state[key].state_changed;
    key_state[key].state_changed = 0;
    return old_state;
}

bool VideoPlayerKeypad::keyPressed(char key)
{
    return key_state[key].state && keyChanged(key);
}

bool VideoPlayerKeypad::keyReleased(char key)
{
    return !key_state[key].state && keyChanged(key);
}

void VideoPlayerKeypad::resetKeyStateChangedAll()
{
    for (auto &pair : key_state)
    {
        pair.second.state_changed = 0;
    }
}

bool VideoPlayerKeypad::anyKey()
{
    for (const auto &pair : key_state)
    {
        if (pair.second.state != 0)
            return true;
    }
    return false;
}

void VideoPlayerKeypad::waitKeyReleased(char key)
{
    while (getKey(key))
    {
        updateKeyStatus();
        delay(20);
    }
}

void VideoPlayerKeypad::waitAllKeysReleased()
{
    while (anyKey())
    {
        updateKeyStatus();
        delay(20);
    }
}

void IRAM_ATTR VideoPlayerKeypad::gpio_isr_handler(void *arg)
{
    VideoPlayerKeypad *keypad = static_cast<VideoPlayerKeypad *>(arg);
    keypad->TCA8418_event = true;
}