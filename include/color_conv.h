#pragma once 
#include <cstdint>

inline uint16_t scale5(uint32_t v) { return static_cast<uint16_t>((v * 31u) / 255u); }
inline uint16_t scale6(uint32_t v) { return static_cast<uint16_t>((v * 63u) / 255u); }

static inline constexpr uint16_t rgb888ToRgb565(uint32_t rgb)
{
    const uint16_t r = scale5((rgb >> 16) & 0xff);
    const uint16_t g = scale6((rgb >> 8) & 0xff);
    const uint16_t b = scale5(rgb & 0xff);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);

}

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static inline uint16_t YCbCrToRGB565(int y, int rOff, int gOff, int bOff) {
  int c = (y - 16) * 298 + 128;
  uint8_t r = clamp8((c + rOff) >> 8);
  uint8_t g = clamp8((c + gOff) >> 8);
  uint8_t b = clamp8((c + bOff) >> 8);
  uint16_t px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
#ifdef SWAP565
  px = __builtin_bswap16(px);
#endif
  return px;
}