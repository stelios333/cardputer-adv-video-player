#include "color_conv.h"
#include "message_box.h"
namespace {

struct BoxGeometry {
    int x;
    int y;
    int w;
    int h;
    int line1Height;
    int line2Height;
    int line1Width;
    int line2Width;
};

BoxGeometry computeBox(LGFX& tft, const char* line1, const char* line2, int line1Size,
                       int line2Size, int padding, int centerX)
{
    tft.setTextSize(line1Size);
    const int w1 = tft.textWidth(line1);
    const int h1 = tft.fontHeight();

    int w2 = 0;
    int h2 = 0;
    if (line2 && line2[0]) {
        tft.setTextSize(line2Size);
        w2 = tft.textWidth(line2);
        h2 = tft.fontHeight();
    }

    const int boxW = std::max(w1, w2) + 2 + padding;
    const int boxH = h1 + h2 + 2 + padding;

    BoxGeometry g;
    g.w = boxW;
    g.h = boxH;
    g.x = 120 - boxW / 2;
    g.y = (centerX + 0x43) - boxH / 2;
    g.line1Height = h1;
    g.line2Height = h2;
    g.line1Width = w1;
    g.line2Width = w2;
    return g;
}

} 

void messageBoxDraw(LGFX& tft, int centerX, const char* line1, const char* line2,
                    uint16_t line1Color, uint16_t line2Color,
                    int line1Size, int line2Size, int padding)
{

    const BoxGeometry g = computeBox(tft, line1, line2, line1Size, line2Size,
                                     padding, centerX);

    tft.fillRect(g.x, g.y, g.w, g.h,
                                 rgb888ToRgb565(0x00666666));
    tft.drawRect(g.x, g.y, g.w, g.h, 0xFFFF);

    tft.setTextSize(line1Size);
    tft.setTextColor(line1Color);
    const int line1Y = g.y + 1 + padding;
    tft.drawString(line1, 120 - g.line1Width / 2, line1Y);

    if (line2 && line2[0]) {
        tft.setTextSize(line2Size);
        tft.setTextColor(line2Color);
        tft.drawString(line2, 120 - g.line2Width / 2,
                                       line1Y + g.line1Height);
    }
}