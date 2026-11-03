#pragma once

#include <stdint.h>

#include "board.h"

// RGB332 framebuffer (RRRGGGBB), scanned continuously through the board's video DAC.
// I2S0 and the audio PLL are exclusively owned by this output engine.
namespace CompositeVideo {
constexpr int width = 256;
constexpr int height = 240;
constexpr int palHeight = 288;
constexpr int pin = Board::videoPin;  // 25 (DAC1) or 26 (DAC2)
constexpr uint8_t patternCount = 5;
constexpr uint8_t palPattern = 4;

bool begin();
bool running();
uint32_t frames();
// Current scanout image. Treat it as read-only once output is running; its
// storage becomes writable again after a later presentation swaps it out.
uint8_t* framebuffer();
// One producer may write this internal-RAM image, then call present(). It is
// unavailable while a previous image is queued. The pointer may change after
// a successful present, so obtain it again before writing the next frame.
uint8_t* backbuffer();
// Attach 256 RGB888 colors (768 bytes) to this writable frame, or restore the
// fixed RGB332 palette with nullptr. Finish this before calling present().
// Palette data is copied; the caller retains ownership of the source bytes.
bool setBackPalette(const uint8_t* rgb888);
bool present();
bool presentPending();
uint32_t presentedFrames();
// 0: color test card, 1: all 256 palette entries, 2: gray scale, 3: SMPTE color
// bars. 4: Philips PM5544, the one image sent as PAL: the raster follows the image,
// and everything but this pattern is NTSC.
// Stop any background frame producer before calling. Drains its queued frame,
// renders into the free buffer and waits for the built-in image to be applied.
void showTestPattern(uint8_t pattern = 0);
bool isPal();
uint8_t rgb(uint8_t red, uint8_t green, uint8_t blue);
}
