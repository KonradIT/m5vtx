#include "composite_video.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp8bit_video.h>
#include <string.h>

namespace CompositeVideo {
namespace {
uint8_t* pixels = nullptr;

void rectangle(int x, int y, int w, int h, uint8_t color) {
    for (int row = max(0, y); row < min(height, y + h); ++row) {
        const int left = max(0, x);
        const int right = min(width, x + w);
        if (right > left) memset(pixels + row * width + left, color, right - left);
    }
}

// Tiny 5x7 glyphs required by the test card. Bits run top-to-bottom by column.
const uint8_t* glyph(char letter) {
    static const uint8_t space[5] = {0,0,0,0,0};
    static const uint8_t m[5] = {0x7f,0x02,0x0c,0x02,0x7f};
    static const uint8_t five[5] = {0x27,0x45,0x45,0x45,0x39};
    static const uint8_t v[5] = {0x1f,0x20,0x40,0x20,0x1f};
    static const uint8_t t[5] = {0x01,0x01,0x7f,0x01,0x01};
    static const uint8_t x[5] = {0x63,0x14,0x08,0x14,0x63};
    static const uint8_t r[5] = {0x7f,0x09,0x19,0x29,0x46};
    static const uint8_t g[5] = {0x3e,0x41,0x49,0x49,0x7a};
    static const uint8_t b[5] = {0x7f,0x49,0x49,0x49,0x36};
    static const uint8_t three[5] = {0x22,0x41,0x49,0x49,0x36};
    static const uint8_t two[5] = {0x42,0x61,0x51,0x49,0x46};
    static const uint8_t n[5] = {0x7f,0x04,0x08,0x10,0x7f};
    static const uint8_t s[5] = {0x46,0x49,0x49,0x49,0x31};
    static const uint8_t c[5] = {0x3e,0x41,0x41,0x41,0x22};
    switch (letter) {
        case 'M': return m; case '5': return five; case 'V': return v;
        case 'T': return t; case 'X': return x; case 'R': return r;
        case 'G': return g; case 'B': return b; case '3': return three;
        case '2': return two; case 'N': return n; case 'S': return s;
        case 'C': return c; default: return space;
    }
}

void text(int x, int y, const char* string, int scale, uint8_t color) {
    for (; *string; ++string, x += 6 * scale) {
        const uint8_t* bitmap = glyph(*string);
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (bitmap[col] & (1 << row))
                    rectangle(x + col * scale, y + row * scale, scale, scale, color);
    }
}

void card() {
    constexpr uint8_t bars[8] = {255, 252, 31, 28, 227, 224, 3, 0};
    for (int y = 0; y < 76; ++y)
        for (int x = 0; x < width; ++x)
            pixels[y * width + x] = bars[x / 32];
    rectangle(0, 76, width, 42, 0);
    text(24, 83, "M5 VTX", 4, 255);
    // A simple 8-bit mountain scene makes vertical/frame alignment visible.
    rectangle(0, 118, width, 75, rgb(45, 145, 255));
    for (int y = 124; y < 144; ++y)
        for (int x = 196; x < 216; ++x)
            if ((x - 206) * (x - 206) + (y - 134) * (y - 134) < 100)
                pixels[y * width + x] = rgb(255, 255, 0);
    for (int x = 0; x < width; ++x) {
        const int ridge = 130 + abs(x - 83) / 2;
        for (int y = ridge; y < 194; ++y)
            pixels[y * width + x] = y < ridge + 8 ? 255 : rgb(55, 75, 110);
        const int front = 157 + abs(x - 184) / 3;
        for (int y = front; y < 194; ++y)
            pixels[y * width + x] = y < front + 5 ? rgb(170, 220, 220) : rgb(0, 110, 55);
    }
    rectangle(0, 194, width, 24, 0);
    text(19, 200, "RGB332 NTSC", 2, 255);
    // Quantized neutral ramp to check contrast independently of color.
    for (int y = 218; y < 236; ++y)
        for (int x = 0; x < width; ++x)
            pixels[y * width + x] = rgb(x, x, x);
    rectangle(0, 236, width, 4, 255);
}
}

uint8_t rgb(uint8_t red, uint8_t green, uint8_t blue) {
    return (red & 0xe0) | ((green >> 3) & 0x1c) | (blue >> 6);
}

bool begin() {
    if (Esp8BitVideo::running()) return true;
    if (!pixels) {
        pixels = static_cast<uint8_t*>(heap_caps_malloc(width * height, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        if (!pixels) return false;
    }
    showTestPattern();
    return Esp8BitVideo::begin(pixels);
}

bool running() { return Esp8BitVideo::running(); }
uint32_t frames() { return Esp8BitVideo::frames(); }
uint8_t* framebuffer() { return pixels; }

void showTestPattern(uint8_t pattern) {
    if (!pixels) return;
    switch (pattern % 3) {
        case 1:
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    pixels[y * width + x] = (y / 15) * 16 + x / 16;
            break;
        case 2:
            // RGB332's exactly neutral values are black and white only. Use
            // a spatial neutral ramp, averaging all channels equally over
            // a small 8x8 Bayer-like threshold matrix.
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const int level = (y < 40) ? 255 : (y > 199 ? 0 : x);
                    const int threshold = ((x & 7) * 17 + (y & 7) * 29) & 63;
                    pixels[y * width + x] = level > threshold * 4 ? 255 : 0;
                }
            rectangle(53, 99, 150, 42, 0);
            text(57, 106, "M5 VTX", 3, 255);
            break;
        default: card(); break;
    }
}
}
