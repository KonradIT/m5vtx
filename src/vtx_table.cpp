// SPDX-License-Identifier: GPL-3.0-only
#include "vtx_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
const uint16_t standardFrequencies[5][8] = {
    {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725},
    {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866},
    {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945},
    {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880},
    {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917}};
const char* const standardNames[5] = {"BOSCAM_A", "BOSCAM_B", "BOSCAM_E", "FATSHARK", "RACEBAND"};
const char standardLetters[5] = {'A', 'B', 'E', 'F', 'R'};

// The next word of the line, as Betaflight's strtok does, or nullptr at its end.
const char* word(const char*& cursor, char* out, size_t size) {
  while (*cursor == ' ' || *cursor == '\t') ++cursor;
  if (!*cursor) return nullptr;
  size_t length = 0;
  while (*cursor && *cursor != ' ' && *cursor != '\t') {
    if (length + 1 < size) out[length++] = *cursor;
    ++cursor;
  }
  out[length] = 0;
  return out;
}

bool number(const char* text, long& value) {
  char* end;
  value = strtol(text, &end, 10);
  return end != text && !*end;
}

bool equalsIgnoringCase(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if ((*a | 32) != (*b | 32)) return false;
  return !*a && !*b;
}

// Betaflight cuts names and labels to their length and upper-cases them.
void copyUpper(char* out, size_t size, const char* text) {
  size_t i = 0;
  for (; text[i] && i + 1 < size; ++i) out[i] = text[i] >= 'a' && text[i] <= 'z' ? text[i] - 32 : text[i];
  out[i] = 0;
}

char errorText[64];
const char* error(const char* format, int value) {
  snprintf(errorText, sizeof(errorText), format, value);
  return errorText;
}
const char* error(const char* format, const char* text) {
  snprintf(errorText, sizeof(errorText), format, text);
  return errorText;
}
}

void VtxTable::reset() {
  *this = VtxTable();
  bands = 5;
  channels = 8;
  for (uint8_t band = 0; band < 5; ++band) {
    snprintf(names[band], sizeof(names[band]), "%s", standardNames[band]);
    letters[band] = standardLetters[band];
    factory[band] = true;
    memcpy(frequencies[band], standardFrequencies[band], sizeof(frequencies[band]));
  }
}

bool VtxTable::valid() const {
  return magic == storedMagic && bands <= maxBands && channels <= maxChannels && powerLevels <= maxPowerLevels;
}

uint16_t VtxTable::frequency(uint8_t band, uint8_t channel) const {
  if (band < 1 || band > bands || channel < 1 || channel > channels) return 0;
  return frequencies[band - 1][channel - 1];
}

bool VtxTable::lookup(uint16_t megahertz, uint8_t& band, uint8_t& channel) const {
  if (!megahertz) return false;
  for (uint8_t b = 0; b < bands; ++b)
    for (uint8_t c = 0; c < channels; ++c)
      if (frequencies[b][c] == megahertz) {
        band = b + 1;
        channel = c + 1;
        return true;
      }
  return false;
}

int VtxTable::powerIndex(uint16_t value) const {
  for (uint8_t level = 0; level < powerLevels; ++level)
    if (powerValues[level] == value) return level;
  return -1;
}

// Betaflight's cliVtxTable, with its messages. Counts, names, letters and labels
// are taken as it takes them; a frequency or power value that is not a number is
// refused here, where Betaflight would read it as 0.
const char* VtxTable::command(const char* arguments) {
  const char* cursor = arguments;
  char token[32];
  if (!word(cursor, token, sizeof(token))) return "MISSING SUBCOMMAND";
  long value;
  if (equalsIgnoringCase(token, "bands") || equalsIgnoringCase(token, "channels") ||
      equalsIgnoringCase(token, "powerlevels")) {
    const bool isBands = equalsIgnoringCase(token, "bands"), isChannels = equalsIgnoringCase(token, "channels");
    const uint8_t most = isBands ? maxBands : isChannels ? maxChannels : maxPowerLevels;
    if (!word(cursor, token, sizeof(token)) || !number(token, value) || value < 0 || value > most)
      return error(isBands ? "BAND COUNT NOT BETWEEN 0 AND %d" : isChannels ? "CHANNEL COUNT NOT BETWEEN 0 AND %d"
                                                                            : "POWER LEVEL COUNT NOT BETWEEN 0 AND %d", most);
    if (isBands) {
      bands = uint8_t(value);
      for (uint8_t band = bands; band < maxBands; ++band) {
        memset(names[band], 0, sizeof(names[band]));
        letters[band] = 0;
        factory[band] = false;
        memset(frequencies[band], 0, sizeof(frequencies[band]));
      }
    } else if (isChannels) {
      channels = uint8_t(value);
      for (uint8_t band = 0; band < maxBands; ++band)
        for (uint8_t channel = channels; channel < maxChannels; ++channel) frequencies[band][channel] = 0;
    } else {
      powerLevels = uint8_t(value);
      for (uint8_t level = powerLevels; level < maxPowerLevels; ++level) {
        powerValues[level] = 0;
        memset(powerLabels[level], 0, sizeof(powerLabels[level]));
      }
    }
    return nullptr;
  }
  if (equalsIgnoringCase(token, "powervalues") || equalsIgnoringCase(token, "powerlabels")) {
    const bool values = equalsIgnoringCase(token, "powervalues");
    uint16_t newValues[maxPowerLevels] = {};
    char newLabels[maxPowerLevels][labelLength + 1] = {};
    uint8_t count = 0;
    while (word(cursor, token, sizeof(token))) {
      if (count == powerLevels) return error(values ? "TOO MANY VALUES (EXPECTED %d)" : "TOO MANY LABELS (EXPECTED %d)", powerLevels);
      if (values) {
        if (!number(token, value) || value < 0 || value > 65535) return error("INVALID POWER VALUE %s", token);
        newValues[count] = uint16_t(value);
      } else copyUpper(newLabels[count], sizeof(newLabels[count]), token);
      ++count;
    }
    if (count != powerLevels) return error(values ? "NOT ENOUGH VALUES (EXPECTED %d)" : "NOT ENOUGH LABELS (EXPECTED %d)", powerLevels);
    if (values) memcpy(powerValues, newValues, sizeof(powerValues));
    else memcpy(powerLabels, newLabels, sizeof(powerLabels));
    return nullptr;
  }
  if (equalsIgnoringCase(token, "band")) {
    if (!word(cursor, token, sizeof(token)) || !number(token, value) || value < 1 || value > bands)
      return error("BAND NUMBER NOT BETWEEN 1 AND %d", bands);
    const uint8_t band = uint8_t(value - 1);
    char name[nameLength + 1];
    if (!word(cursor, token, sizeof(token))) return "MISSING BAND NAME";
    copyUpper(name, sizeof(name), token);
    if (!word(cursor, token, sizeof(token))) return "MISSING BAND LETTER";
    const char letter = token[0] >= 'a' && token[0] <= 'z' ? token[0] - 32 : token[0];
    // The flag is optional, as in Betaflight, and a band without it is CUSTOM.
    bool isFactory = false;
    const char* resume = cursor;
    if (word(cursor, token, sizeof(token)) && (token[0] < '0' || token[0] > '9')) {
      if (equalsIgnoringCase(token, "factory")) isFactory = true;
      else if (!equalsIgnoringCase(token, "custom")) return error("INVALID FACTORY FLAG %s (EXPECTED FACTORY OR CUSTOM)", token);
    } else cursor = resume;
    uint16_t newFrequencies[maxChannels] = {};
    uint8_t count = 0;
    while (word(cursor, token, sizeof(token))) {
      if (count == channels) return error("TOO MANY FREQUENCIES (EXPECTED %d)", channels);
      if (!number(token, value) || value < 0 || value > 65535) return error("INVALID FREQUENCY %s", token);
      newFrequencies[count++] = uint16_t(value);
    }
    if (count != channels) return error("NOT ENOUGH FREQUENCIES (EXPECTED %d)", channels);
    memcpy(names[band], name, sizeof(names[band]));
    letters[band] = letter;
    factory[band] = isFactory;
    memcpy(frequencies[band], newFrequencies, sizeof(frequencies[band]));
    return nullptr;
  }
  return error("INVALID SUBCOMMAND %s", token);
}

bool VtxTable::line(int n, char* out, size_t size) const {
  if (n == 0) { snprintf(out, size, "vtxtable bands %u", bands); return true; }
  if (n == 1) { snprintf(out, size, "vtxtable channels %u", channels); return true; }
  n -= 2;
  if (n < bands) {
    int written = snprintf(out, size, "vtxtable band %d %-8s %c %-7s", n + 1, names[n],
                           letters[n] ? letters[n] : '?', factory[n] ? "FACTORY" : "CUSTOM");
    for (uint8_t channel = 0; channel < channels && written >= 0 && size_t(written) < size; ++channel)
      written += snprintf(out + written, size - written, " %4u", frequencies[n][channel]);
    return true;
  }
  n -= bands;
  if (n == 0) { snprintf(out, size, "vtxtable powerlevels %u", powerLevels); return true; }
  if (n > 2) return false;
  int written = snprintf(out, size, n == 1 ? "vtxtable powervalues" : "vtxtable powerlabels");
  for (uint8_t level = 0; level < powerLevels && written >= 0 && size_t(written) < size; ++level) {
    if (n == 1) written += snprintf(out + written, size - written, " %u", powerValues[level]);
    else written += snprintf(out + written, size - written, " %s", powerLabels[level]);
  }
  return true;
}
