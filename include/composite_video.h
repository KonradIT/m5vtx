#pragma once

#include <stdint.h>

// RGB332 framebuffer (RRRGGGBB), scanned continuously through DAC2/GPIO26.
// I2S0 and the audio PLL are exclusively owned by this output engine.
namespace CompositeVideo {
constexpr int width = 256;
constexpr int height = 240;
constexpr int pin = 26;

bool begin();
bool running();
uint32_t frames();
uint8_t* framebuffer();
// 0: color test card, 1: all 256 palette entries, 2: gray scale.
void showTestPattern(uint8_t pattern = 0);
uint8_t rgb(uint8_t red, uint8_t green, uint8_t blue);
}
