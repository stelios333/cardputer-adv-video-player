#pragma once
#ifdef CARDPUTER_ADV
#include "keypad_cardputeradv.h"

constexpr char VP_KEY_FM_UP = ';';
constexpr char VP_KEY_FM_DOWN = '.';
constexpr char VP_KEY_ENTER = KEY_ENTER;
constexpr char VP_KEY_BACK = KEY_BACKSPACE;
constexpr char VP_KEY_MOD = KEY_FN;
constexpr char VP_KEY_VOL_UP = ';';
constexpr char VP_KEY_VOL_DN = '.';
constexpr char VP_KEY_ESC = '`';
constexpr char VP_KEY_PAUSE = 'p';
constexpr char VP_KEY_FASTFORWARD = 'f';

extern VideoPlayerKeypad keyboard;
#endif