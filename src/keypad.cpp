#include "keypad.h"





CardputerKeypad::CardputerKeypad()
{
}

CardputerKeypad::~CardputerKeypad()
{
}

void CardputerKeypad::updateKeyStatus()
{
    while (TCA8418_event) {
        KeypadEvent event = CardputerKeypad::HandleKeypadInput();
        char key = CARDPUTER_KEYMAP[event.col][event.row].value_first;
        key_status[key] = event.pressed;
    }
}

KeypadEvent CardputerKeypad::HandleKeypadInput() {
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
    if ((intstat & 0x03) == 0) TCA8418_event = false;
    return {pressed, row, col};
}

bool CardputerKeypad::begin(TwoWire& wire)
{
  if (! keypad.begin(TCA8418_DEFAULT_ADDR, &wire)) {
    return 1;
  }
  keypad.matrix(7, 8);
  keypad.flush();
    
  pinMode(KEYPAD_INT, INPUT_PULLUP);
  attachInterruptArg(digitalPinToInterrupt(KEYPAD_INT), gpio_isr_handler, this, CHANGE);
  keypad.enableInterrupts();
  return 0;
}

bool CardputerKeypad::getKey(char key)
{
    return key_status[key];
}

bool CardputerKeypad::anyKey()
{
    for (const auto& pair : key_status) {
        if (pair.second != 0)
            return true;
    }
    return false;
}

void CardputerKeypad::waitKeyReleased(char key)
{
    while (getKey(key)) {
       updateKeyStatus();
       delay(20);
    }
}

void CardputerKeypad::waitAllKeysReleased()
{
    while (anyKey()) {
       updateKeyStatus();
       delay(20);
    }
}

void IRAM_ATTR CardputerKeypad::gpio_isr_handler(void* arg)
{
    CardputerKeypad* keypad = static_cast<CardputerKeypad*>(arg);
    keypad->TCA8418_event = true;
}