#pragma once
#include <unordered_map>
#include <cstdint>
#include <Adafruit_TCA8418.h>

// Cardputer keymap copied from https://github.com/bmorcelli/Launcher/blob/main/lib/Keyboard/Keyboard.h
constexpr int KEYPAD_SDA = 8;
constexpr int KEYPAD_SCL = 9;
constexpr int KEYPAD_INT = 11;

#define KEYPAD_ROWS 14
#define KEYPAD_COLS 4

#define KEY_SHIFT 0x80
#define KEY_LEFT_CTRL 0x80
#define KEY_LEFT_SHIFT 0x81
#define KEY_LEFT_ALT 0x82
#define KEY_FN 0xff
#define KEY_OPT 0x00
#define KEY_BACKSPACE 0x2a
#define KEY_TAB 0x2b
#define KEY_ENTER 0x28


typedef struct {
    const char value_first;
    const char value_second;
} KeyValue_t;

inline void mapRawKeyToPhysical(uint8_t event, uint8_t &row, uint8_t &col) {
    const uint8_t u = event % 10; // 1..8
    const uint8_t t = event / 10; // 0..6

    if (u >= 1 && u <= 8 && t <= 6) {
        const uint8_t u0 = u - 1;   // 0..7
        col = u0 & 0x03;            // bits [1:0] => 0..3
        row = (t << 1) | (u0 >> 2); // t*2 + bit2(u0) => 0..13
    } else {
        row = 0xFF; // invalid
        col = 0xFF;
    }
}

typedef struct 
{
 bool pressed;
 int row, col;
} KeypadEvent;


const KeyValue_t CARDPUTER_KEYMAP[KEYPAD_COLS][KEYPAD_ROWS] = {
    {{'`', '~'},
     {'1', '!'},
     {'2', '@'},
     {'3', '#'},
     {'4', '$'},
     {'5', '%'},
     {'6', '^'},
     {'7', '&'},
     {'8', '*'},
     {'9', '('},
     {'0', ')'},
     {'-', '_'},
     {'=', '+'},
     {KEY_BACKSPACE, KEY_BACKSPACE}},
    {{KEY_TAB, KEY_TAB},
     {'q', 'Q'},
     {'w', 'W'},
     {'e', 'E'},
     {'r', 'R'},
     {'t', 'T'},
     {'y', 'Y'},
     {'u', 'U'},
     {'i', 'I'},
     {'o', 'O'},
     {'p', 'P'},
     {'[', '{'},
     {']', '}'},
     {'\\', '|'}                   },
    {{KEY_FN, KEY_FN},
     {KEY_LEFT_SHIFT, KEY_LEFT_SHIFT},
     {'a', 'A'},
     {'s', 'S'},
     {'d', 'D'},
     {'f', 'F'},
     {'g', 'G'},
     {'h', 'H'},
     {'j', 'J'},
     {'k', 'K'},
     {'l', 'L'},
     {';', ':'},
     {'\'', '\"'},
     {KEY_ENTER, KEY_ENTER}        },
    {{KEY_LEFT_CTRL, KEY_LEFT_CTRL},
     {KEY_OPT, KEY_OPT},
     {KEY_LEFT_ALT, KEY_LEFT_ALT},
     {'z', 'Z'},
     {'x', 'X'},
     {'c', 'C'},
     {'v', 'V'},
     {'b', 'B'},
     {'n', 'N'},
     {'m', 'M'},
     {',', '<'},
     {'.', '>'},
     {'/', '?'},
     {' ', ' '}                    }
};




class CardputerAdvKeypad {
    private:
        Adafruit_TCA8418 keypad;
        typedef struct
        {
            bool state;
            bool state_changed;
        } keystate_t;
        std::unordered_map<char, keystate_t> key_state = 
            {{'`', {0, 0}},
            {'1', {0, 0}},
            {'2', {0, 0}},
            {'3', {0, 0}},
            {'4', {0, 0}},
            {'5', {0, 0}},
            {'6', {0, 0}},
            {'7', {0, 0}},
            {'8', {0, 0}},
            {'9', {0, 0}},
            {'0', {0, 0}},
            {'-', {0, 0}},
            {'=', {0, 0}},
            {KEY_BACKSPACE, {0, 0}},
            {KEY_TAB, {0, 0}},
            {'q', {0, 0}},
            {'w', {0, 0}},
            {'e', {0, 0}},
            {'r', {0, 0}},
            {'t', {0, 0}},
            {'y', {0, 0}},
            {'u', {0, 0}},
            {'i', {0, 0}},
            {'o', {0, 0}},
            {'p', {0, 0}},
            {'[', {0, 0}},
            {']', {0, 0}},
            {'\\', {0, 0}},                  
            {KEY_FN, {0, 0}},
            {KEY_LEFT_SHIFT, {0, 0}},
            {'a', {0, 0}},
            {'s', {0, 0}},
            {'d', {0, 0}},
            {'f', {0, 0}},
            {'g', {0, 0}},
            {'h', {0, 0}},
            {'j', {0, 0}},
            {'k', {0, 0}},
            {'l', {0, 0}},
            {';', {0, 0}},
            {'\'', {0, 0}},
            {KEY_ENTER, {0, 0}},       
            {KEY_LEFT_CTRL, {0, 0}},
            {KEY_OPT, {0, 0}},
            {KEY_LEFT_ALT, {0, 0}},
            {'z', {0, 0}},
            {'x', {0, 0}},
            {'c', {0, 0}},
            {'v', {0, 0}},
            {'b', {0, 0}},
            {'n', {0, 0}},
            {'m', {0, 0}},
            {',', {0, 0}},
            {'.', {0, 0}},
            {'/', {0, 0}},
            {' ', {0, 0}}
        };
        static void IRAM_ATTR gpio_isr_handler(void* arg);
        volatile bool TCA8418_event;
    public:
        CardputerAdvKeypad();
        ~CardputerAdvKeypad();
        void updateKeyStatus();
        bool begin(TwoWire& wire);
        bool getKey(char key);
        bool keyChanged(char key);
        bool keyPressed(char key);
        bool keyReleased(char key);
        void resetKeyStateChangedAll();
        bool anyKey();
        void waitKeyReleased(char key);
        void waitAllKeysReleased();
        KeypadEvent HandleKeypadInput();
};