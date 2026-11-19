#pragma once
#include "lgfx_config.h"

void messageBoxDraw(LGFX& tft, int centerX, const char* line1, const char* line2,
                    uint16_t line1Color, uint16_t line2Color,
                    int line1Size, int line2Size, int padding);