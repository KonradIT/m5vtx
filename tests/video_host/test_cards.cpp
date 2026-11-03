// The real src/composite_video.cpp and PAL line renderer, over a stand-in for the
// engine's hardware half. The DMA ring is followed buffer by buffer, as the ISR does.
//
//   test_cards [DIRECTORY]
//
// With a directory, also writes what tools/reference's firmware test compares with
// its own rendering: each card's pixels and DAC table, and four PAL frames, after
// which the subcarrier and the V switch are back where they started.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "composite_video.h"
#include "esp8bit_video.h"

using namespace Esp8BitVideo;
using Detail::PaletteBank;

namespace Esp8BitVideo {
namespace {
Detail::FrameExchange exchange;
PaletteBank palettes[2];
SampleBand bands[2] = {};
bool started = false;
bool pal = false;
int restarts = 0;
}

bool begin(uint8_t* front, uint8_t* back, int) {
    exchange.reset(front, back);
    palettes[0].assign(nullptr);
    palettes[1].assign(nullptr);
    started = true;
    return true;
}
bool running() { return started; }
uint32_t frames() { return 0; }
bool setStandard(Standard next, const SampleBand* given, int count) {
    const bool wanted = next == Standard::Pal;
    if (wanted == pal) return true;
    assert(!exchange.pending());
    pal = wanted;
    ++restarts;
    for (int i = 0; i < 2; ++i) bands[i] = wanted && i < count ? given[i] : SampleBand{0, 0, nullptr};
    palettes[0].setPal(pal);
    palettes[1].setPal(pal);
    return true;
}
Standard standard() { return pal ? Standard::Pal : Standard::Ntsc; }
uint8_t* framebuffer() { return exchange.front(); }
uint8_t* backbuffer() { return exchange.writable(); }
bool setBackPalette(const uint8_t* rgb888) {
    palettes[exchange.writableBank()].assign(rgb888);
    return true;
}
bool setBackPaletteSignal(uint8_t index, float y, float u, float v) {
    palettes[exchange.writableBank()].setSignal(index, y, u, v);
    return true;
}
// The field boundary comes at once here: nothing else is scanning.
bool present() { return exchange.queue() && exchange.onLineRendered(height); }
bool presentPending() { return exchange.pending(); }
uint32_t presentedFrames() { return exchange.presented(); }
}

namespace {
const PaletteBank& frontPalette() { return palettes[exchange.frontBank()]; }

// Consecutive samples through the ring: buffers of 1136 and 1134, as the DMA reads them.
struct PalRaster {
    std::vector<uint8_t> stream;
    int nextBuffer = 0;
    uint32_t phase = 0;
    bool secondFrame = false;

    void frame() {
        for (int line = 1; line <= palLinesPerFrame; ++line) {
            alignas(4) uint16_t buffer[palSamplesPerLine + 1];
            const int lead = nextBuffer & 1;
            const int samples = palSamplesPerLine + (lead ? -1 : 1);
            std::memset(buffer, 0xff, sizeof(buffer));
            Detail::renderPalLine(buffer, line, secondFrame, phase, lead, exchange.front(), frontPalette().wave, bands);
            for (int i = 0; i < samples; ++i) {
                const uint16_t value = buffer[i ^ 1];
                assert((value & 0xff) == 0);    // every sample written, and only in the DAC's byte
                stream.push_back(uint8_t(value >> 8));
            }
            phase = (phase + palSamplesPerLine) & 3;
            nextBuffer = (nextBuffer + 1) & 3;
        }
        secondFrame = !secondFrame;
    }
};

void write(const std::string& path, const void* data, size_t bytes) {
    FILE* file = std::fopen(path.c_str(), "wb");
    assert(file);
    assert(std::fwrite(data, 1, bytes, file) == bytes);
    std::fclose(file);
}

void writeTable(const std::string& path) {
    uint8_t codes[256 * 4];
    for (int color = 0; color < 256; ++color)
        for (int i = 0; i < 4; ++i) codes[color * 4 + i] = uint8_t(frontPalette().wave[color][i] >> 8);
    write(path, codes, sizeof(codes));
}

// Each line's sync pulses as the receiver finds them, from the stream alone.
std::vector<std::pair<int, int>> pulses(const uint8_t* line, int length) {
    std::vector<std::pair<int, int>> found;
    for (int i = 0; i < length; ++i) {
        if (line[i] != Detail::syncCode) continue;
        int end = i;
        while (end < length && line[end] == Detail::syncCode) ++end;
        found.push_back({i, end - i});
        i = end;
    }
    return found;
}

void testPalTiming(const std::vector<uint8_t>& stream) {
    using namespace Detail;
    assert(stream.size() == size_t(4 * palLinesPerFrame * palSamplesPerLine));
    const auto eq = [](int at) { return std::make_pair(at, palEqualizingSamples); };
    const auto broad = [](int at) { return std::make_pair(at, palBroadSamples); };
    using Pulses = std::vector<std::pair<int, int>>;
    const Pulses ee{eq(0), eq(palHalfLine)}, bb{broad(0), broad(palHalfLine)};
    const auto at = [&](int line) { return pulses(stream.data() + (line - 1) * palSamplesPerLine, palSamplesPerLine); };
    for (int line : {1, 2, 314, 315}) assert(at(line) == bb);
    assert((at(3) == Pulses{broad(0), eq(palHalfLine)}));
    for (int line : {4, 5, 311, 312, 316, 317, 624, 625}) assert(at(line) == ee);
    assert((at(313) == Pulses{eq(0), broad(palHalfLine)}));
    assert((at(318) == Pulses{eq(0)}));
    assert((at(623) == Pulses{{0, palSyncSamples}, eq(palHalfLine)}));
    for (int line = 6; line <= 625; ++line)
        if (line < 311 || (line > 318 && line < 623)) assert((at(line) == Pulses{{0, palSyncSamples}}));
}

void testStandardFollowsPattern() {
    using CompositeVideo::showTestPattern;
    for (int pattern : {0, 1, 2, 3}) {
        showTestPattern(pattern);
        assert(!CompositeVideo::isPal());
    }
    const int before = restarts;
    showTestPattern(CompositeVideo::palPattern);
    assert(CompositeVideo::isPal() && restarts == before + 1);
    assert(bands[0].samples && bands[1].samples && bands[0].last == bands[1].first);
    showTestPattern(CompositeVideo::palPattern);        // the same again needs no restart
    assert(restarts == before + 1);
    showTestPattern(0);
    assert(!CompositeVideo::isPal() && restarts == before + 2 && !bands[0].samples);
    // The built-ins' palette is back exactly as it was, after PAL's other levels.
    PaletteBank legacy;
    legacy.assign(nullptr);
    assert(!std::memcmp(frontPalette().wave, legacy.wave, sizeof(legacy.wave)));
}

void testSignalColorsDoNotOutliveTheirCard() {
    using CompositeVideo::showTestPattern;
    showTestPattern(3);
    PaletteBank smpte = frontPalette();
    showTestPattern(0);
    showTestPattern(3);
    assert(!std::memcmp(frontPalette().wave, smpte.wave, sizeof(smpte.wave)));
    showTestPattern(0);
    showTestPattern(0);     // through both banks
    PaletteBank legacy;
    legacy.assign(nullptr);
    assert(!std::memcmp(frontPalette().wave, legacy.wave, sizeof(legacy.wave)));
}
}

int main(int argc, char** argv) {
    assert(CompositeVideo::begin());
    testStandardFollowsPattern();
    testSignalColorsDoNotOutliveTheirCard();

    const std::string directory = argc > 1 ? std::string(argv[1]) + "/" : "";
    if (argc > 1) {
        for (int pattern : {3, 4}) {
            CompositeVideo::showTestPattern(pattern);
            const int rows = pattern == CompositeVideo::palPattern ? palHeight : height;
            write(directory + "pattern" + std::to_string(pattern) + ".pixels", exchange.front(), width * rows);
            writeTable(directory + "pattern" + std::to_string(pattern) + ".dac");
        }
    }
    CompositeVideo::showTestPattern(CompositeVideo::palPattern);
    PalRaster raster;
    for (int frame = 0; frame < 4; ++frame) raster.frame();
    assert(raster.phase == 0 && !raster.secondFrame && raster.nextBuffer % 2 == 0);
    testPalTiming(raster.stream);
    if (argc > 1) write(directory + "pal.frames", raster.stream.data(), raster.stream.size());
    std::puts("Card tests passed: the raster follows the pattern, built-ins restored exactly, PAL pulses on every line, every DMA sample written, four frames back to their start.");
}
