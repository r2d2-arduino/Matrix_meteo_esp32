#pragma once
#include <stdint.h>

struct Glyph {
    uint16_t bitmapOffset;
    uint8_t  width;
    uint8_t  height;
    uint8_t  xAdvance;
    int8_t   xOffset;
    int8_t   yOffset;
};

struct Font {
    const uint8_t*  bitmap;
    const Glyph*    glyphs;
    uint8_t         firstChar;
    uint8_t         lastChar;
    uint8_t         yAdvance;
    uint8_t         size;
};