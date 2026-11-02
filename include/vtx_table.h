#pragma once

#include <stddef.h>
#include <stdint.h>

// Betaflight's vtxtable: the bands and channels a transmitter has, at which
// frequencies, and its power levels, entered with the same CLI lines a
// Betaflight dump holds. A FACTORY band is one the VTX knows by number, so
// SmartAudio selects it as band * 8 + channel; a CUSTOM band is set by its
// frequency. A frequency of 0 is a channel the VTX does not have. Power values
// are what goes on the wire: a SmartAudio v2.1 level is dBm, a v2 or v1 level
// the raw byte the VTX takes, a Tramp level milliwatts.
struct VtxTable {
  static constexpr uint8_t maxBands = 8;
  static constexpr uint8_t maxChannels = 8;
  static constexpr uint8_t maxPowerLevels = 8;
  static constexpr uint8_t nameLength = 8;
  static constexpr uint8_t labelLength = 3;
  static constexpr uint32_t storedMagic = 0x31585456;  // "VTX1", first in the stored bytes

  uint32_t magic = storedMagic;
  uint8_t bands = 0;
  uint8_t channels = 0;
  uint8_t powerLevels = 0;
  char names[maxBands][nameLength + 1] = {};
  char letters[maxBands] = {};
  bool factory[maxBands] = {};
  uint16_t frequencies[maxBands][maxChannels] = {};
  uint16_t powerValues[maxPowerLevels] = {};
  char powerLabels[maxPowerLevels][labelLength + 1] = {};

  // The usual five bands, A, B, E, F and Raceband, as FACTORY, and no power levels.
  void reset();
  bool valid() const;
  // Band and channel are 1-based here, as in Betaflight's settings. 0 means not available.
  uint16_t frequency(uint8_t band, uint8_t channel) const;
  // The first band and channel at a frequency.
  bool lookup(uint16_t megahertz, uint8_t& band, uint8_t& channel) const;
  // The 0-based level with this power value, or -1.
  int powerIndex(uint16_t value) const;
  // What follows "vtxtable " on one CLI line. Returns an error, or nullptr when applied.
  const char* command(const char* arguments);
  // Line n of the table as Betaflight prints it, from "vtxtable bands". False past the end.
  bool line(int n, char* out, size_t size) const;
};
