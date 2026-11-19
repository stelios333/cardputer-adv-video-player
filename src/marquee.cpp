#include <Arduino.h>
#include "lgfx_config.h"
#include "color_conv.h"
#include "marquee.h"





void marqueeReset(LGFX& tft, Marquee& marquee, int width)
{
    const int visible  = tft.width() - kTextLeft - kTextRight;
    const int overflow = width - visible;

    marquee.limit  = (overflow > 0) ? overflow : 0;
    marquee.scroll = 0;
    marquee.dir    = 1; 
}

void marqueeStep(LGFX& tft, Marquee& marquee, const char* text, uint32_t colorRgb888)
{
    if (!marquee.active()) {
        delay(10);
        return;
    }

    const int height = tft.fontHeight();
    const int y      = tft.height() / 2 - height / 2;

    // Erase the previous frame with the highlight colour.
    tft.setTextColor(rgb888ToRgb565(kMarqueeEraseRgb));
    tft.drawString(text, kTextLeft + marquee.scroll, y);

    if (marquee.dir) {
        marquee.scroll -= kMarqueeStepPx;
        if (marquee.scroll <= -marquee.limit) {
            marquee.scroll = -marquee.limit;
            marquee.dir    = 0;
        }
    } else {
        marquee.scroll += kMarqueeStepPx;
        if (marquee.scroll >= 0) {
            marquee.scroll = 0;
            marquee.dir    = 1;
        }
    }

    tft.setTextColor(rgb888ToRgb565(colorRgb888));
    tft.drawString(text, kTextLeft + marquee.scroll, y);



    delay(kMarqueeFrameUs / 1000);
}

