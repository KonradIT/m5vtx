// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

// The boards M5VTX runs on. Each platformio.ini environment selects one with
// M5VTX_BOARD and defines its features and wiring next to it:
//   M5VTX_HAS_SD               1 when the board has a microSD slot for movies
//   M5VTX_VIDEO_DAC_GPIO       25 (DAC1) or 26 (DAC2), the composite output
//   M5VTX_CONTROL_PINS         the GPIOs the VTX control wire may be on, e.g. 16,17
//   M5VTX_CONTROL_PIN_DEFAULT  the one used until another is stored in NVS
// A build without M5VTX_BOARD, such as the host tests, is the M5Stack Basic.
#define M5VTX_BOARD_BASIC 1
#define M5VTX_BOARD_STICKC_PLUS2 2

#ifndef M5VTX_BOARD
#define M5VTX_BOARD M5VTX_BOARD_BASIC
#define M5VTX_HAS_SD 1
#define M5VTX_VIDEO_DAC_GPIO 26
#define M5VTX_CONTROL_PINS 16, 17
#define M5VTX_CONTROL_PIN_DEFAULT 16
#endif

#if !defined(M5VTX_HAS_SD) || !defined(M5VTX_VIDEO_DAC_GPIO) || !defined(M5VTX_CONTROL_PINS) || \
    !defined(M5VTX_CONTROL_PIN_DEFAULT)
#error "platformio.ini defines M5VTX_HAS_SD, M5VTX_VIDEO_DAC_GPIO, M5VTX_CONTROL_PINS and M5VTX_CONTROL_PIN_DEFAULT with M5VTX_BOARD"
#endif

namespace Board {
// Screen geometry in landscape pixels. The two status rows, the field rows and
// the setup rows share rowHeight; the field rows scroll when they do not all fit.
// The notice and the key help sit at the bottom.
struct Layout {
  int16_t width, height;
  int16_t rowHeight;
  int16_t valueX;      // where a row's value starts, unless its label is wider
  uint8_t fieldRows;   // field rows on the screen at once
  int16_t noticeHeight, keyHeight;
  int16_t setupItemsY, setupNotesY;
};

#if M5VTX_BOARD == M5VTX_BOARD_BASIC
// M5Stack Basic v2.7: 320x240 LCD, buttons A, B and C under it, a microSD slot on
// the LCD's SPI bus, and the speaker on GPIO25, DAC1.
constexpr const char* name = "M5Stack Basic";
constexpr int speakerPin = 25;
constexpr const char* buttons = "A39/B38/C37";
constexpr const char* applyButton = "C";  // held to apply the selected row
constexpr Layout layout = {320, 240, 21, 132, 7, 18, 16, 24, 96};
constexpr const char* mainKeys[] = {"A: next    B: -    C: +", "Hold C: apply   Hold B: probe"};
constexpr const char* setupKeys[] = {"B: -    C: +", "Hold C: save protocol"};
constexpr const char* setupNotes[] = {"Bands and power: paste the vtxtable", "over USB serial, then save",
                                      "Protocol: B / C, then hold C"};
#elif M5VTX_BOARD == M5VTX_BOARD_STICKC_PLUS2
// M5StickC Plus2: an ESP32-PICO-V3-02, whose PSRAM takes GPIO16 and GPIO17, a
// 135x240 LCD turned to landscape, the front button A, the side button B and the
// power button, and no card slot. The passive buzzer on GPIO2 stays off.
constexpr const char* name = "M5StickC Plus2";
constexpr int speakerPin = -1;
constexpr const char* buttons = "A37/B39/PWR35";
constexpr const char* applyButton = "A";
constexpr Layout layout = {240, 135, 15, 92, 4, 15, 15, 15, 60};
constexpr const char* mainKeys[] = {"A: next   B: +   PWR: -", "Hold A: apply   Hold B: probe"};
constexpr const char* setupKeys[] = {"B: change    Hold A: save"};
constexpr const char* setupNotes[] = {"Paste the vtxtable over USB", "serial, then save",
                                      "Protocol: B, then hold A"};
#else
#error "Unknown M5VTX_BOARD"
#endif

constexpr bool hasSd = M5VTX_HAS_SD;
constexpr int videoPin = M5VTX_VIDEO_DAC_GPIO;
constexpr uint8_t controlPins[] = {M5VTX_CONTROL_PINS};
constexpr size_t controlPinCount = sizeof(controlPins) / sizeof(controlPins[0]);
constexpr uint8_t defaultControlPin = M5VTX_CONTROL_PIN_DEFAULT;
constexpr size_t mainKeyLines = sizeof(mainKeys) / sizeof(mainKeys[0]);
constexpr size_t setupKeyLines = sizeof(setupKeys) / sizeof(setupKeys[0]);
constexpr size_t setupNoteLines = sizeof(setupNotes) / sizeof(setupNotes[0]);

constexpr bool listed(const uint8_t* pins, size_t count, int pin) {
  return count && (pins[0] == pin || listed(pins + 1, count - 1, pin));
}

inline bool isControlPin(int pin) { return listed(controlPins, controlPinCount, pin); }

static_assert(videoPin == 25 || videoPin == 26, "Composite video needs DAC1 (GPIO25) or DAC2 (GPIO26)");
static_assert(videoPin != speakerPin, "The speaker's DAC cannot carry video");
static_assert(speakerPin < 0 || speakerPin == 25, "setup() turns the speaker off as DAC1");
static_assert(listed(controlPins, controlPinCount, defaultControlPin), "The default control pin must be listed");
static_assert(!listed(controlPins, controlPinCount, videoPin), "The control wire cannot share the video pin");
static_assert(!listed(controlPins, controlPinCount, speakerPin), "The control wire cannot share the speaker pin");
static_assert(mainKeyLines <= 2 && setupKeyLines <= 2, "Two lines of key help at most");
}  // namespace Board
