#pragma once
#include "marquee.h"
#include "keypad.h"
#include <cstdint>

constexpr int kMaxFiles       = 1024;
constexpr int kMaxPath        = 256;
constexpr int kDelayKeyRepeatUs = 300000;

void drawPicker(LGFX& tft, const String& dir, const std::vector<String>& names,
                const std::vector<bool>& isDir, int selected,
                Marquee& marquee);

void filePicker(LGFX& tft, char* out, size_t outSize);
