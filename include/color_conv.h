#pragma once 
#include <cstdint>

inline uint16_t scale5(uint32_t v) { return static_cast<uint16_t>((v * 31u) / 255u); }
inline uint16_t scale6(uint32_t v) { return static_cast<uint16_t>((v * 63u) / 255u); }

inline constexpr uint16_t rgb888ToRgb565(uint32_t rgb)
{
    const uint16_t r = scale5((rgb >> 16) & 0xff);
    const uint16_t g = scale6((rgb >> 8) & 0xff);
    const uint16_t b = scale5(rgb & 0xff);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}