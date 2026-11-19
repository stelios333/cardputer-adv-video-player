#pragma once

#include <stdint.h>

// x = 235 - width.
const int      kMarqueeStepPx   = 2;
constexpr int  kTextLeft        = 9;
constexpr int  kTextRight       = 4;
const uint32_t kMarqueeEraseRgb = 0x00111111;
const uint32_t kMarqueeFrameUs  = 50000;

typedef struct {
    int    scroll = 0;
    int    limit  = 0;
    int8_t dir    = 1;  // 1 = moving left (-2 px), 0 = moving right (+2 px)

    bool active() const { return limit > 0; }
} Marquee;

// Recomputes the travel limit for a string of `width` pixels and rewinds the
// animation to its starting position.  Short strings leave the marquee idle.
void marqueeReset(LGFX& tft, Marquee& marquee, int width);

// Advances one frame: erase the previous frame, move two pixels, redraw in
// `colorRgb888`, then wait 50 ms (the firmware's per-frame delay).  When the
// string is short this only yields.
void marqueeStep(LGFX& tft, Marquee& marquee, const char* text, uint32_t colorRgb888);


