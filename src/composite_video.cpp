#include "composite_video.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp8bit_video.h>
#include <math.h>
#include <string.h>

namespace CompositeVideo {
namespace {
uint8_t* pixels = nullptr;
uint8_t* buffers[2] = {};
int rows = height;                      // of the image being drawn

bool waitForPendingPresent() {
    const uint32_t started = millis();
    while (Esp8BitVideo::presentPending()) {
        // Normally at most one 16.7ms raster. Stay bounded if output has failed.
        if (millis() - started >= 100) return false;
        delay(1);
    }
    return true;
}

void rectangle(int x, int y, int w, int h, uint8_t color) {
    for (int row = max(0, y); row < min(rows, y + h); ++row) {
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
    static const uint8_t p[5] = {0x7f,0x09,0x09,0x09,0x06};
    static const uint8_t a[5] = {0x7e,0x11,0x11,0x11,0x7e};
    static const uint8_t l[5] = {0x7f,0x40,0x40,0x40,0x40};
    static const uint8_t six[5] = {0x3e,0x49,0x49,0x49,0x32};
    switch (letter) {
        case 'M': return m; case '5': return five; case 'V': return v;
        case 'T': return t; case 'X': return x; case 'R': return r;
        case 'G': return g; case 'B': return b; case '3': return three;
        case '2': return two; case 'N': return n; case 'S': return s;
        case 'C': return c; case 'P': return p; case 'A': return a;
        case 'L': return l; case '6': return six; default: return space;
    }
}

void text(int x, int y, const char* string, int scale, uint8_t color, int rowScale = 0) {
    if (!rowScale) rowScale = scale;
    for (; *string; ++string, x += 6 * scale) {
        const uint8_t* bitmap = glyph(*string);
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (bitmap[col] & (1 << row))
                    rectangle(x + col * scale, y + row * rowScale, scale, rowScale, color);
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

// RGB332 has neither card's levels, so each brings its colors, as a movie does.
bool loadColors(const uint8_t (*colors)[3], int count) {
    uint8_t palette[768] = {};
    memcpy(palette, colors, count * 3);
    return Esp8BitVideo::setBackPalette(palette);
}

// SMPTE EG 1: 75% bars over two thirds of the height, a twelfth of reversed bars, and
// -I, white, +Q and PLUGE. Their levels are IRE above the 7.5 of black, of which 92.5
// reach white. -I and +Q have black's luma, so they and the sub-black are out of RGB.
enum SmpteColor : uint8_t {
    SmpteGray, SmpteYellow, SmpteCyan, SmpteGreen, SmpteMagenta, SmpteRed, SmpteBlue,
    SmpteBlack, SmpteWhite, SmpteAboveBlack, SmpteColorCount, SmpteBelowBlack = SmpteColorCount, SmpteMinusI, SmptePlusQ
};

bool loadSmpteColors() {
    static const uint8_t colors[SmpteColorCount][3] = {
        {191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0}, {191, 0, 191}, {191, 0, 0}, {0, 0, 191},
        {0, 0, 0}, {255, 255, 255}, {11, 11, 11}};
    constexpr float pluge = 4.0f / 92.5f;
    constexpr float carrier = 20.0f / 92.5f;
    constexpr float cos33 = 0.838671f, sin33 = 0.544639f;       // -I is at 303 degrees, +Q at 33
    return loadColors(colors, SmpteColorCount) &&
           Esp8BitVideo::setBackPaletteSignal(SmpteBelowBlack, -pluge, 0, 0) &&
           Esp8BitVideo::setBackPaletteSignal(SmpteMinusI, 0, carrier * sin33, -carrier * cos33) &&
           Esp8BitVideo::setBackPaletteSignal(SmptePlusQ, 0, carrier * cos33, carrier * sin33);
}

// Column n/d of the picture, whose first sent column is 1, and 253 are sent.
int smpteEdge(int n, int d) { return n == 0 ? 0 : n == d ? width : 1 + n * 253 / d; }

void smpteCard() {
    constexpr int barsEnd = height * 2 / 3;
    constexpr int reversedEnd = barsEnd + height / 12;
    static const uint8_t reversed[7] = {SmpteBlue, SmpteBlack, SmpteMagenta, SmpteBlack, SmpteCyan, SmpteBlack, SmpteGray};
    for (int bar = 0; bar < 7; ++bar) {
        const int left = smpteEdge(bar, 7);
        rectangle(left, 0, smpteEdge(bar + 1, 7) - left, barsEnd, bar);
        rectangle(left, barsEnd, smpteEdge(bar + 1, 7) - left, reversedEnd - barsEnd, reversed[bar]);
    }
    // Four blocks of 5/4 bar, then PLUGE's three thirds under the red bar and black.
    static const uint8_t blocks[4] = {SmpteMinusI, SmpteWhite, SmptePlusQ, SmpteBlack};
    for (int block = 0; block < 4; ++block) {
        const int left = smpteEdge(block * 5, 28);
        rectangle(left, reversedEnd, smpteEdge((block + 1) * 5, 28) - left, height - reversedEnd, blocks[block]);
    }
    static const uint8_t pluge[3] = {SmpteBelowBlack, SmpteBlack, SmpteAboveBlack};
    for (int third = 0; third < 3; ++third) {
        const int left = smpteEdge(15 + third, 21);
        rectangle(left, reversedEnd, smpteEdge(16 + third, 21) - left, height - reversedEnd, pluge[third]);
    }
    rectangle(smpteEdge(6, 7), reversedEnd, width - smpteEdge(6, 7), height - reversedEnd, SmpteBlack);
}

// Philips PM5544, from a 768x576 picture at a third of its columns and half its rows.
// A grid cell is then 14 columns by 21 rows, and a column is 1.5 times a row's height.
enum PhilipsColor : uint8_t {
    PhilipsBlack, PhilipsWhite, PhilipsBackground, PhilipsSquare, PhilipsYellow, PhilipsCyan, PhilipsGreen,
    PhilipsMagenta, PhilipsRed, PhilipsBlue, Philips270, Philips90, Philips326, Philips146, Philips180, Philips0,
    PhilipsBurstA, PhilipsBurstB, PhilipsStep1, PhilipsStep2, PhilipsStep3, PhilipsStep4, PhilipsColorCount
};

bool loadPhilipsColors() {
    static const uint8_t colors[PhilipsColorCount][3] = {
        {0, 0, 0}, {255, 255, 255}, {122, 122, 122}, {191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
        {191, 0, 191}, {191, 0, 0}, {0, 0, 191},
        // Chroma on the background's luma, named by its angle from +U.
        {55, 157, 122}, {190, 88, 122}, {85, 122, 222}, {160, 122, 23}, {122, 146, 2}, {122, 99, 243},
        {68, 0, 0}, {0, 35, 0}, {51, 51, 51}, {102, 102, 102}, {153, 153, 153}, {204, 204, 204}};
    return loadColors(colors, PhilipsColorCount);
}

// The bars' and the gray scale's edges, 28 columns apart around the center.
int philipsStep(int x) { return x < 72 ? 0 : x < 100 ? 1 : x < 128 ? 2 : x < 156 ? 3 : x < 184 ? 4 : 5; }

// Inside the circle, by row.
uint8_t philipsCircle(int x, int y) {
    static const uint8_t bars[6] = {PhilipsYellow, PhilipsCyan, PhilipsGreen, PhilipsMagenta, PhilipsRed, PhilipsBlue};
    static const uint8_t steps[6] = {PhilipsBlack, PhilipsStep1, PhilipsStep2, PhilipsStep3, PhilipsStep4, PhilipsWhite};
    const bool center = x >= 122 && x <= 134;
    if (center && y >= 112 && y < 175) return x == 128 || y == 144 ? PhilipsWhite : PhilipsBlack;
    if (y < 49) return y >= 28 && x >= 100 && x < 156 ? PhilipsBlack : PhilipsWhite;
    // A black edge and a needle on white: whatever echoes shows up to their right.
    if (y < 70) return x >= 86 && x < 170 && x != 92 ? PhilipsWhite : PhilipsBlack;
    if (y < 91) return ((x + 72) / 10) & 1 ? PhilipsSquare : PhilipsBlack;   // 250 kHz
    if (y < 134) return bars[philipsStep(x)];
    if (y < 154) return (x - 9) % 14 == 0 || y == 144 ? PhilipsWhite : PhilipsBlack;
    if (y < 196) return PhilipsBlack;   // the gratings, between the band's columns
    if (y < 217) return steps[philipsStep(x)];
    if (y < 238) return x >= 86 && x < 170 && x != 92 ? PhilipsBlack : PhilipsWhite;
    return center ? PhilipsRed : PhilipsYellow;
}

void philipsCard() {
    for (int y = 0; y < palHeight; ++y) {
        uint8_t* row = pixels + y * width;
        for (int x = 0; x < width; ++x) {
            const int dx = (x - 128) * 3, dy = (y - 144) * 2;
            if (dx * dx + dy * dy <= 252 * 252) row[x] = philipsCircle(x, y);
            else row[x] = (x - 9) % 14 == 0 || (y - 7) % 21 == 0 ? PhilipsWhite : PhilipsBackground;
        }
        // Outside the circle, on the same grid. Every other row goes the other way
        // around the color axis: phase inversion, for which PAL's mean of two lines is gray.
        if (y < 8 || y >= 280) continue;
        if ((y - 7) % 21) {
            memset(row + 10, y & 1 ? Philips270 : Philips90, 13);
            memset(row + 234, y & 1 ? Philips180 : Philips0, 13);
        }
        const bool upper = y >= 29 && y < 70, lower = y >= 218 && y < 259;
        if (y >= 29 && y < 259) {
            memset(row + 24, y < 144 ? Philips270 : Philips90, 13);
            memset(row + 219, y < 144 ? Philips180 : Philips0, 14);
        }
        if (upper || lower) {
            memset(row + 37, upper ? Philips326 : Philips146, 14);
            memset(row + 206, upper ? Philips326 : Philips146, 13);
        }
    }
    // Castellations: the border's black cells, between white ones.
    rectangle(0, 0, width, 7, PhilipsWhite);
    rectangle(0, 281, width, 7, PhilipsWhite);
    for (int cell = 0; cell < 9; ++cell) {
        rectangle(10 + cell * 28, 0, 13, 7, PhilipsBlack);
        rectangle(10 + cell * 28, 281, 13, 7, PhilipsBlack);
    }
    rectangle(0, 0, 9, palHeight, PhilipsWhite);
    rectangle(248, 0, 8, palHeight, PhilipsWhite);
    for (int cell = 0; cell < 7; ++cell) {
        rectangle(0, 8 + cell * 42, 9, 20, PhilipsBlack);
        rectangle(248, 8 + cell * 42, 8, 20, PhilipsBlack);
    }
    // Burst gate check: little luma, much chroma, going the other way on every other row.
    for (int y = 50; y < 70; ++y) memset(pixels + y * width, y & 1 ? PhilipsBurstB : PhilipsBurstA, 9);
    text(110, 31, "M5 VTX", 1, PhilipsWhite, 2);
    text(107, 221, "PAL 625", 1, PhilipsWhite, 2);
}

// The definition lines: sines from black to white, past what a column can hold.
uint16_t gratings[2][Esp8BitVideo::palBandSamples];
constexpr int gratingColumns = 27;
constexpr int gratingFirstRow = 154, gratingBlockEnd = 175, gratingLastRow = 196;
const Esp8BitVideo::SampleBand gratingBands[2] = {
    {gratingFirstRow, gratingBlockEnd, gratings[0]}, {gratingBlockEnd, gratingLastRow, gratings[1]}};

void buildGratings() {
    using namespace Esp8BitVideo;
    using namespace Esp8BitVideo::Detail;
    static const float megahertz[5] = {0.8f, 1.8f, 2.8f, 3.8f, 4.8f};
    const int origin = palColumnStart(palBandFirstColumn);
    for (int burst = 0; burst < 5; ++burst) {
        const int from = palColumnStart(palBandFirstColumn + burst * gratingColumns) - origin;
        const int to = palColumnStart(palBandFirstColumn + (burst + 1) * gratingColumns) - origin;
        for (int i = from; i < to; ++i) {
            const float level = .5f - .5f * cosf(6.2831853f * megahertz[burst] * 1e6f * (i - from) / float(palSampleRate));
            gratings[1][i] = static_cast<uint16_t>(lroundf(blankCode + (palWhiteCode - blankCode) * level) << 8);
        }
    }
    memcpy(gratings[0], gratings[1], sizeof(gratings[0]));
    // The center's black block with its white line reaches down into the first rows.
    const auto at = [origin](int x) { return palColumnStart(x) - origin; };
    for (int i = at(122); i < at(135); ++i) gratings[0][i] = blankCode << 8;
    for (int i = at(128); i < at(129); ++i) gratings[0][i] = palWhiteCode << 8;
}
}

uint8_t rgb(uint8_t red, uint8_t green, uint8_t blue) {
    return (red & 0xe0) | ((green >> 3) & 0x1c) | (blue >> 6);
}

bool begin() {
    if (Esp8BitVideo::running()) return true;
    for (auto& buffer : buffers) {
        if (!buffer) buffer = static_cast<uint8_t*>(heap_caps_malloc(width * palHeight, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        if (!buffer) {
            for (auto& allocated : buffers) { heap_caps_free(allocated); allocated = nullptr; }
            pixels = nullptr;
            return false;
        }
        memset(buffer, 0, width * palHeight);
    }
    buildGratings();
    pixels = buffers[0];
    showTestPattern();
    memcpy(buffers[1], buffers[0], width * height);
    if (Esp8BitVideo::begin(buffers[0], buffers[1], pin)) return true;
    for (auto& allocated : buffers) { heap_caps_free(allocated); allocated = nullptr; }
    pixels = nullptr;
    return false;
}

bool running() { return Esp8BitVideo::running(); }
uint32_t frames() { return Esp8BitVideo::frames(); }
uint8_t* framebuffer() { return running() ? Esp8BitVideo::framebuffer() : buffers[0]; }
uint8_t* backbuffer() { return Esp8BitVideo::backbuffer(); }
bool setBackPalette(const uint8_t* rgb888) { return Esp8BitVideo::setBackPalette(rgb888); }
bool present() { return Esp8BitVideo::present(); }
bool presentPending() { return Esp8BitVideo::presentPending(); }
uint32_t presentedFrames() { return Esp8BitVideo::presentedFrames(); }
bool isPal() { return Esp8BitVideo::standard() == Esp8BitVideo::Standard::Pal; }

void showTestPattern(uint8_t pattern) {
    pattern %= patternCount;
    if (running()) {
        // The caller first stops its background producer. Drain any SD frame
        // queued before stop, then replace it with the requested built-in card.
        if (!waitForPendingPresent()) return;
        const bool ready = pattern == palPattern
            ? Esp8BitVideo::setStandard(Esp8BitVideo::Standard::Pal, gratingBands, 2)
            : Esp8BitVideo::setStandard(Esp8BitVideo::Standard::Ntsc);
        if (!ready) return;
        pixels = Esp8BitVideo::backbuffer();
        if (!pixels) return;
        const bool colors = pattern == 3 ? loadSmpteColors()
                            : pattern == palPattern ? loadPhilipsColors()
                            : Esp8BitVideo::setBackPalette(nullptr);
        if (!colors) return;
    }
    if (!pixels) return;
    rows = pattern == palPattern ? palHeight : height;
    switch (pattern) {
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
        case 3: smpteCard(); break;
        case 4: philipsCard(); break;
        default: card(); break;
    }
    if (running() && Esp8BitVideo::present()) waitForPendingPresent();
}
}
