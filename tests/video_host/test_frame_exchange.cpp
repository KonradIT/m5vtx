#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "esp8bit_video.h"
using Esp8BitVideo::Detail::FrameExchange;
using Esp8BitVideo::Detail::PaletteBank;

uint32_t waveformCrc(const PaletteBank& palette) {
    uint32_t crc = 0xffffffff;
    for (int index = 0; index < 256; ++index)
        for (int sample = 0; sample < 4; ++sample)
            for (int shift = 0; shift < 16; shift += 8) {
                crc ^= uint8_t(palette.wave[index][sample] >> shift);
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ (0xedb88320u & uint32_t(-int32_t(crc & 1)));
            }
    return crc ^ 0xffffffff;
}

void testPaletteGenerationAndCache() {
    PaletteBank legacy, indexed;
    assert(legacy.assign(nullptr));
    // Fingerprint frozen from the complete v1 four-phase DAC table generator,
    // including its exact float arithmetic: changes with the DAC levels.
    assert(waveformCrc(legacy) == 0x02aad8d1u);
    assert(!legacy.assign(nullptr));
    uint8_t colors[768] = {};
    colors[17 * 3] = 255; // Arbitrary index17 now means pure red.
    colors[219 * 3] = colors[219 * 3 + 1] = colors[219 * 3 + 2] = 255;
    assert(indexed.assign(colors));
    assert(!indexed.assign(colors)); // Same clip palette must hit the cache.
    for (int sample = 0; sample < 4; ++sample) {
        assert(indexed.wave[17][sample] == legacy.wave[224][sample]);
        assert(indexed.wave[219][sample] == legacy.wave[255][sample]);
        assert(indexed.wave[224][sample] == legacy.wave[0][sample]);
    }
    colors[17 * 3] = 0;
    colors[17 * 3 + 2] = 255;
    assert(indexed.wave[17][0] == legacy.wave[224][0]); // Source bytes were copied.
    assert(indexed.assign(colors));
    for (int sample = 0; sample < 4; ++sample)
        assert(indexed.wave[17][sample] == legacy.wave[3][sample]);
    assert(indexed.assign(nullptr));
    assert(waveformCrc(indexed) == 0x02aad8d1u); // Built-in/v1 restoration is exact.
    assert(!indexed.assign(nullptr));
}

void testPaletteAndFrameHandoff() {
    uint8_t a = 17, b = 17;
    FrameExchange exchange;
    PaletteBank palettes[2];
    palettes[0].assign(nullptr);
    palettes[1].assign(nullptr);
    exchange.reset(&a, &b);
    uint8_t red[768] = {}, blue[768] = {};
    red[17 * 3] = 255;
    blue[17 * 3 + 2] = 255;
    // Preparing a new bank must not change the currently visible palette.
    palettes[exchange.writableBank()].assign(red);
    assert(waveformCrc(palettes[exchange.frontBank()]) == 0x02aad8d1u);
    assert(exchange.queue() && exchange.writableBank() == -1);
    for (int line = 0; line < 240; ++line) {
        assert(exchange.front() == &a && exchange.frontBank() == 0);
        assert(!exchange.onLineRendered(line));
    }
    assert(exchange.onLineRendered(240));
    assert(exchange.front() == &b && exchange.frontBank() == 1);
    assert(palettes[exchange.frontBank()].wave[*exchange.front()][0] == (32 << 8));
    palettes[exchange.writableBank()].assign(blue);
    assert(palettes[exchange.frontBank()].wave[17][0] == (32 << 8));
    assert(exchange.queue());
    for (int line = 0; line < 240; ++line) assert(!exchange.onLineRendered(line));
    assert(exchange.onLineRendered(240));
    assert(exchange.front() == &a && exchange.frontBank() == 0);
    assert(palettes[exchange.frontBank()].wave[*exchange.front()][0] == (74 << 8));
    // Returning to built-ins restores only the free bank; the blue image stays
    // blue until the same boundary switches both its pixels and its palette.
    palettes[exchange.writableBank()].assign(nullptr);
    assert(palettes[exchange.frontBank()].wave[17][0] == (74 << 8));
    assert(exchange.queue() && exchange.onLineRendered(240));
    assert(exchange.front() == &b && exchange.frontBank() == 1);
    assert(waveformCrc(palettes[exchange.frontBank()]) == 0x02aad8d1u);
}

int main() {
    uint8_t a[Esp8BitVideo::width * Esp8BitVideo::height] = {};
    uint8_t b[Esp8BitVideo::width * Esp8BitVideo::height] = {};
    FrameExchange empty;
    assert(!empty.queue() && !empty.pending() && empty.writable() == nullptr && empty.writableBank() == -1);
    for (int submit = 0; submit < Esp8BitVideo::linesPerFrame; ++submit) {
        FrameExchange exchange;
        exchange.reset(a, b);
        assert(exchange.front() == a && exchange.writable() == b);
        assert(exchange.frontBank() == 0 && exchange.writableBank() == 1);
        for (int raster = 0; raster < 3; ++raster) {
            auto* activeFrame = exchange.front();
            for (int line = 0; line < Esp8BitVideo::linesPerFrame; ++line) {
                if (raster == 0 && line == submit) {
                    assert(exchange.queue());
                    assert(exchange.pending() && exchange.writable() == nullptr && exchange.writableBank() == -1);
                    assert(!exchange.queue());
                }
                // A receiver must see one source buffer throughout each image.
                if (line < Esp8BitVideo::height) assert(exchange.front() == activeFrame);
                const bool swapped = exchange.onLineRendered(line);
                if (swapped) assert(line == Esp8BitVideo::height);
            }
        }
        assert(exchange.presented() == 1 && !exchange.pending());
        assert(exchange.front() == b && exchange.writable() == a);
        assert(exchange.frontBank() == 1 && exchange.writableBank() == 0);
        assert(exchange.queue());
        for (int line = 0; line < Esp8BitVideo::height; ++line)
            assert(!exchange.onLineRendered(line));
        assert(exchange.onLineRendered(Esp8BitVideo::height));
        assert(exchange.front() == a && exchange.writable() == b);
        assert(exchange.frontBank() == 0 && exchange.writableBank() == 1);
        assert(exchange.presented() == 2);
    }
    // A ready frame arriving after this field's blanking point must wait for
    // the next boundary; blanking does not mean it can flip at any line.
    FrameExchange late;
    late.reset(a, b);
    assert(!late.onLineRendered(240));
    assert(late.queue());
    for (int line = 241; line < 262; ++line) assert(!late.onLineRendered(line));
    assert(late.front() == a && late.pending());
    testPaletteGenerationAndCache();
    testPaletteAndFrameHandoff();
    std::puts("Frame/palette ownership tests passed: 525 submission positions, intact images, queue rejection, vblank coherence, palette cache and exact v1 restoration.");
}
