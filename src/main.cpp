// SPDX-License-Identifier: GPL-3.0-only
#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <driver/dac.h>

#include "board.h"
#include "composite_video.h"
#if M5VTX_HAS_SD
#include "sd_video_player.h"
#endif
#include "vtx_manager.h"
#include "vtx_table.h"

namespace {
VtxManager vtx;
VtxTable table;
#if M5VTX_HAS_SD
SdVideoPlayer sdVideo;
bool mediaInitialized = false;
uint32_t lastMediaCheck = 0, lastMediaFrames = 0;
char lastMediaMessage[96] = {};
#endif
SemaphoreHandle_t spiBusMutex = nullptr;  // the LCD's SPI bus, and the card's where there is one
constexpr Board::Layout layout = Board::layout;
M5Canvas displayLine(&M5.Display);
bool displayBuffered = false;
bool displayDeferred = false;
uint32_t displayTransfers = 0;
enum Field : uint8_t { Band, Channel, Power, Pit, Protocol, Image, Probe, FieldCount };
Field selected = Channel;
uint8_t firstShownField = 0;  // the field rows scroll where they do not all fit
Field pendingField = FieldCount;
uint32_t pendingSince = 0;
uint8_t draftBand = 0, draftChannel = 0, draftPower = 0, pattern = 0;  // 0-based into the vtxtable
uint8_t controlPin = Board::defaultControlPin, draftControlPin = Board::defaultControlPin;
VtxProtocol controlProtocol = VtxProtocol::SmartAudio;
VtxProtocol draftProtocol = VtxProtocol::SmartAudio;
bool draftPit = false;
uint8_t edited = 0;
uint8_t applyQueue = 0;  // fields `save` still has to send, one readback at a time
bool inBatch = false;    // between `batch start` and `batch end`, unknown lines are a dump's
// First-time setup: a vtxtable with bands and power levels has to be entered over
// serial and saved, and the protocol chosen, before the main screen is shown.
bool tableSaved = false;     // a vtxtable is in NVS
bool protocolSaved = false;  // the protocol was chosen and is in NVS
bool setupShown = false;     // the setup list is what the LCD shows now
bool screenDirty = true;
uint32_t lastScreen = 0, lastReport = 0, lastResponses = 0;
char notice[64] = "Queries only during discovery";
char commandLine[192] = {};
size_t commandLength = 0;
// Without power levels in the vtxtable, Tramp offers these up to its reported
// maximum, and SmartAudio v2, which advertises nothing, the specification's four
// indices. SmartAudio v2.1 advertises its own dBm table.
constexpr uint16_t trampPowerChoices[] = {25, 100, 200, 400, 600, 800, 1000, 1600};
constexpr uint8_t trampPowerChoiceCount = sizeof(trampPowerChoices) / sizeof(trampPowerChoices[0]);
constexpr uint8_t sa2DefaultPowerLevels = 4;
// `sa2-offset 1` is for a SmartAudio v2 VTX whose GET reports one more than its SET
// takes, as measured on a DarwinFPV (SET 0..2, GET 1..3). Kept in NVS.
uint8_t sa2PowerOffset = 0;
constexpr const char* fieldNames[] = {"Band", "Channel", "Power", "Pit mode", "Protocol", "Image", "Control pin"};
constexpr const char* patternNames[] = {"Test card", "256 colors", "Gray ramp", "SMPTE bars NTSC", "Philips PM5544 PAL"};
constexpr size_t builtInImages = CompositeVideo::patternCount;
static_assert(sizeof(patternNames) / sizeof(patternNames[0]) == builtInImages, "Each built-in image needs a name");
constexpr uint8_t channelEditMask = (1 << Band) | (1 << Channel);

bool isChannelField(Field field) { return field == Band || field == Channel; }
uint8_t fieldMask(Field field) { return isChannelField(field) ? channelEditMask : uint8_t(1 << field); }

bool setupBandsDone() { return tableSaved && table.bands && table.channels; }
bool setupPowerDone() { return tableSaved && table.powerLevels; }
bool setupComplete() { return setupBandsDone() && setupPowerDone() && protocolSaved; }

const char* protocolChoiceName(VtxProtocol protocol) {
  return protocol == VtxProtocol::Unknown ? "Auto" : VtxManager::protocolName(protocol);
}

void notify(const char* text) {
  snprintf(notice, sizeof(notice), "%s", text);
  Serial.printf("UI %s\n", notice);
  screenDirty = true;
}

void savePreference(const char* key, uint8_t value) {
  Preferences preferences;
  if (preferences.begin("m5vtx", false)) {
    preferences.putUChar(key, value);
    preferences.end();
  }
}

// The vtxtable: what the band, channel and power rows mean on the wire.

uint16_t draftFrequency() { return table.frequency(draftBand + 1, draftChannel + 1); }

void clampDrafts() {
  if (draftBand >= table.bands) draftBand = 0;
  if (draftChannel >= table.channels) draftChannel = 0;
  if (table.powerLevels && draftPower >= table.powerLevels) draftPower = 0;
}

uint8_t powerLevelCount(const VtxStatus& s) {
  if (table.powerLevels) return table.powerLevels;
  if (s.protocol == VtxProtocol::SmartAudio) return s.powerCount;
  uint8_t count = 0;
  if (s.maxPowerMilliwatts)
    while (count < trampPowerChoiceCount && trampPowerChoices[count] <= s.maxPowerMilliwatts) ++count;
  return count;
}

void powerText(const VtxStatus& s, uint8_t level, char* out, size_t size) {
  if (table.powerLevels) {
    const char* unit = s.protocol == VtxProtocol::Tramp ? " mW" : s.saVersion == 21 ? " dBm" : "";
    snprintf(out, size, "%s (%s%u%s)", table.powerLabels[level], *unit ? "" : "#", table.powerValues[level], unit);
  } else if (s.protocol == VtxProtocol::SmartAudio) {
    if (s.saVersion == 21 && level < s.powerCount) snprintf(out, size, "#%u (%u dBm)", level, s.powerLevelsDbm[level]);
    else snprintf(out, size, "Level #%u", level);
  } else snprintf(out, size, "%u mW", trampPowerChoices[level % trampPowerChoiceCount]);
}

bool sendPower(const VtxStatus& s, uint8_t level) {
  if (table.powerLevels) return vtx.setPowerValue(table.powerValues[level]);
  if (s.protocol == VtxProtocol::SmartAudio) return vtx.setPowerIndex(level);
  return vtx.setPower(trampPowerChoices[level % trampPowerChoiceCount]);
}

bool powerConfirmed(const VtxStatus& s, uint8_t level) {
  if (table.powerLevels) return s.powerValue == table.powerValues[level];
  if (s.protocol == VtxProtocol::SmartAudio) return s.powerIndex == level;
  return s.powerMilliwatts == trampPowerChoices[level % trampPowerChoiceCount];
}

// The level the VTX reports, or -1.
int reportedPower(const VtxStatus& s) {
  if (table.powerLevels) return s.powerValue < 0 ? -1 : table.powerIndex(uint16_t(s.powerValue));
  if (s.protocol == VtxProtocol::SmartAudio) return s.powerIndex;
  for (uint8_t i = 0; i < trampPowerChoiceCount; ++i)
    if (s.powerMilliwatts == trampPowerChoices[i]) return i;
  return -1;
}

// SmartAudio selects a FACTORY band's channel by number; everything else is a frequency.
bool factoryChannel(const VtxStatus& s) {
  return s.protocol == VtxProtocol::SmartAudio && draftBand < table.bands && table.factory[draftBand];
}

bool sendChannel(const VtxStatus& s) {
  const uint16_t megahertz = draftFrequency();
  if (!megahertz) { notify("Channel not available in the vtxtable"); return false; }
  if (factoryChannel(s)) return vtx.setChannel(draftBand * 8 + draftChannel);
  return vtx.setFrequency(megahertz);
}

bool channelConfirmed(const VtxStatus& s) {
  return s.frequencyMHz == draftFrequency() &&
         (!factoryChannel(s) || s.channelIndex == draftBand * 8 + draftChannel);
}

// The band and channel the VTX is on, 1-based: the selection when it is an alias of
// the reported frequency (R7 and F8 are both 5880 MHz), else SmartAudio's channel
// number on a FACTORY band, else the first entry at that frequency.
bool reportedChannel(const VtxStatus& s, uint8_t& band, uint8_t& channel) {
  if (!s.frequencyMHz) return false;
  if (draftFrequency() == s.frequencyMHz) { band = draftBand + 1; channel = draftChannel + 1; return true; }
  if (s.protocol == VtxProtocol::SmartAudio && s.channelIndex != 255) {
    const uint8_t b = s.channelIndex / 8, c = s.channelIndex % 8;
    if (b < table.bands && c < table.channels && table.factory[b] && table.frequencies[b][c] == s.frequencyMHz) {
      band = b + 1;
      channel = c + 1;
      return true;
    }
  }
  return table.lookup(s.frequencyMHz, band, channel);
}

void followChannel(const VtxStatus& s) {
  uint8_t band, channel;
  if (reportedChannel(s, band, channel)) { draftBand = band - 1; draftChannel = channel - 1; }
}

// The built-in images, then on a board with a card slot its movies and Rescan SD.
#if M5VTX_HAS_SD
size_t imageOptionCount() { return builtInImages + sdVideo.count() + 1; }

void rescanVideos() {
  sdVideo.stop();
  pattern = 0;
  CompositeVideo::showTestPattern(pattern);
  notify(mediaInitialized && sdVideo.rescan() ? "Scanning SD card" : "SD player unavailable");
}
#else
size_t imageOptionCount() { return builtInImages; }
#endif

bool selectImage(uint8_t choice, bool activateRescan = false) {
  if (choice < builtInImages) {
#if M5VTX_HAS_SD
    sdVideo.stop();
#endif
    pattern = choice;
    CompositeVideo::showTestPattern(pattern);
    notify("Image displayed on video output");
    return true;
  }
#if !M5VTX_HAS_SD
  (void)activateRescan;
  return false;
#else
  const size_t videoCount = sdVideo.count();
  if (choice > builtInImages + videoCount) return false;
  if (choice == builtInImages + videoCount) {
    pattern = choice;
    if (activateRescan) rescanVideos();
    else {
      char text[64];
      snprintf(text, sizeof(text), "Hold %s to rescan SD card", Board::applyButton);
      notify(text);
    }
    return true;
  }
  // Movies are NTSC. Nothing is producing frames while the PAL card is up.
  const bool wasPal = CompositeVideo::isPal();
  if (wasPal) CompositeVideo::showTestPattern(0);
  if (!sdVideo.play(choice - builtInImages)) {
    if (wasPal) CompositeVideo::showTestPattern(CompositeVideo::palPattern);
    notify("Video unavailable; rescan SD card");
    return false;
  }
  pattern = choice;
  notify("Loading SD video");
  return true;
#endif
}

#if M5VTX_HAS_SD
void reportMedia(bool listFiles = false) {
  const auto media = sdVideo.status();
  Serial.printf("MEDIA mounted=%u scanning=%u playing=%u loading=%u files=%u selected=%d frame=%lu/%lu fps=%u/%u presented=%lu underruns=%lu errors=%lu rejected=%lu read_ms=%lu max_read_us=%lu image=%u flips=%lu format=%s header_bytes=%u\n",
                media.mounted, media.scanning, media.playing, media.loading, unsigned(sdVideo.count()),
                media.selectedIndex, static_cast<unsigned long>(media.frameIndex),
                static_cast<unsigned long>(media.totalFrames), media.fpsNum, media.fpsDen,
                static_cast<unsigned long>(media.framesPresented), static_cast<unsigned long>(media.underruns),
                static_cast<unsigned long>(media.sdErrors), static_cast<unsigned long>(media.rejectedFiles),
                static_cast<unsigned long>(media.lastFrameReadMs), static_cast<unsigned long>(media.maxReadUs),
                pattern, static_cast<unsigned long>(CompositeVideo::presentedFrames()),
                media.version == 2 ? "Indexed256" : media.version == 1 ? "RGB332" : "none", media.headerSize);
  Serial.printf("MEDIA_STATE %s\n", media.message);
  if (listFiles) {
    for (size_t i = 0; i < sdVideo.count(); ++i) {
      const auto entry = sdVideo.entry(i);
      Serial.printf("MEDIA_FILE index=%u image=%u name=%s frames=%lu fps=%u/%u format=%s header_bytes=%u palette_crc=%08lX title=%s\n",
                    unsigned(i), unsigned(i + builtInImages), entry.name, static_cast<unsigned long>(entry.frameCount),
                    entry.fpsNum, entry.fpsDen, entry.version == 2 ? "Indexed256" : "RGB332", entry.headerSize,
                    static_cast<unsigned long>(entry.paletteCrc), entry.title);
    }
  }
}

void syncMedia() {
  if (millis() - lastMediaCheck < 200) return;
  lastMediaCheck = millis();
  const auto media = sdVideo.status();
  if (strcmp(lastMediaMessage, media.message)) {
    snprintf(lastMediaMessage, sizeof(lastMediaMessage), "%s", media.message);
    notify(media.message);
  }
  if (media.framesPresented != lastMediaFrames) {
    lastMediaFrames = media.framesPresented;
    screenDirty = true;
  }
}

// The second row while a movie plays or loads.
bool movieProgress(char* text, size_t size) {
  const auto media = sdVideo.status();
  if (media.playing && media.fpsNum) {
    const uint32_t seconds = uint64_t(media.frameIndex) * media.fpsDen / media.fpsNum;
    const uint32_t total = uint64_t(media.totalFrames) * media.fpsDen / media.fpsNum;
    snprintf(text, size, "Play %02lu:%02lu / %02lu:%02lu  %u fps",
             static_cast<unsigned long>(seconds / 60), static_cast<unsigned long>(seconds % 60),
             static_cast<unsigned long>(total / 60), static_cast<unsigned long>(total % 60),
             unsigned((media.fpsNum + media.fpsDen / 2) / media.fpsDen));
    return true;
  }
  if (media.loading) {
    snprintf(text, size, "Loading SD video...");
    return true;
  }
  return false;
}
#else
// No card slot. The report keeps its shape for the tools that read it.
void reportMedia(bool = false) {
  Serial.printf("MEDIA mounted=0 scanning=0 playing=0 loading=0 files=0 selected=-1 image=%u flips=%lu format=none\n",
                pattern, static_cast<unsigned long>(CompositeVideo::presentedFrames()));
  Serial.printf("MEDIA_STATE The %s has no SD card slot\n", Board::name);
}

bool movieProgress(char*, size_t) { return false; }
#endif

void report() {
  const auto& s = vtx.status();
  uint8_t band = 0, channel = 0;
  reportedChannel(s, band, channel);
  Serial.printf("STATUS protocol=%s connected=%u probing=%u baud=%lu inverted=%u "
                "frequency=%u band=%u channel=%u channel_raw=%u power_mw=%u power_index=%d power_raw=%d power_value=%d "
                "power_levels=%u sa2_offset=%u pit=%u queries=%lu rx=%lu valid=%lu crc_errors=%lu echoes=%lu "
                "video=%u standard=%s frames=%lu heap=%u pattern=%u\n",
                VtxManager::protocolName(s.protocol), s.connected, s.probing,
                static_cast<unsigned long>(s.baud), s.inverted, s.frequencyMHz, band, channel, s.channelIndex,
                s.powerMilliwatts, s.powerIndex, s.rawPowerIndex, s.powerValue,
                powerLevelCount(s), sa2PowerOffset, s.pitMode,
                static_cast<unsigned long>(s.queries), static_cast<unsigned long>(s.rxBytes),
                static_cast<unsigned long>(s.validResponses), static_cast<unsigned long>(s.crcErrors),
                static_cast<unsigned long>(s.echoFrames), CompositeVideo::running(),
                CompositeVideo::isPal() ? "PAL" : "NTSC",
                static_cast<unsigned long>(CompositeVideo::frames()), ESP.getFreeHeap(), pattern);
  Serial.printf("STATE %s\n", s.stateText);
  Serial.printf("LINE pin=%u settled=%d rx_edges=%lu pullup=%d pulldown=%d floating=%d\n",
                controlPin, s.settledLevel, static_cast<unsigned long>(s.rxEdges),
                s.linePullUp, s.linePullDown, s.lineFloating);
  Serial.printf("UI_STATE selected=%s vtx_band=%u vtx_channel=%u draft_mhz=%u vtx_power=%u draft_pit=%u edited=%u pending=%u queued=%u control_pin=%u draft_pin=%u protocol_mode=%s draft_protocol=%s setup_bands=%u setup_power=%u setup_protocol=%u setup_done=%u\n",
                fieldNames[selected], draftBand + 1, draftChannel + 1, draftFrequency(), draftPower + 1, draftPit,
                edited, pendingField < FieldCount, applyQueue,
                controlPin, draftControlPin, protocolChoiceName(controlProtocol), protocolChoiceName(draftProtocol),
                setupBandsDone(), setupPowerDone(), protocolSaved, setupComplete());
  Serial.printf("UI_RENDER buffered=%u row_transfers=%lu\n", displayBuffered,
                static_cast<unsigned long>(displayTransfers));
  reportMedia();
}

void selectControlPin(uint8_t pin) {
  controlPin = draftControlPin = pin;
  draftProtocol = controlProtocol;
  savePreference("control-pin", pin);
  pendingField = FieldCount;
  edited = 0;
  lastResponses = 0;
  vtx.begin(pin);
  char text[64];
  snprintf(text, sizeof(text), "Discovering VTX on GPIO%u", pin);
  notify(text);
}

void selectProtocol(VtxProtocol protocol) {
  if (!vtx.setProtocol(protocol)) { notify("Unsupported control protocol"); return; }
  controlProtocol = draftProtocol = protocol;
  draftControlPin = controlPin;
  savePreference("protocol", static_cast<uint8_t>(protocol));
  protocolSaved = true;
  // Nothing is sent on the wire until a protocol has been chosen.
  if (!vtx.started()) vtx.begin(controlPin);
  pendingField = FieldCount;
  edited = 0;
  lastResponses = 0;
  draftPower = 0;
  char text[64];
  snprintf(text, sizeof(text), "Probing %s on GPIO%u", protocolChoiceName(protocol), controlPin);
  notify(text);
}

void setSa2Offset(uint8_t offset) {
  if (!vtx.setSa2PowerOffset(offset)) { notify("Busy; retry after readback"); return; }
  sa2PowerOffset = offset;
  savePreference("sa2-offset", offset);
  vtx.setKnownPowerCount(sa2DefaultPowerLevels);
  edited &= ~(1 << Power);
  notify(offset ? "SA2 GET power is one above SET" : "SA2 GET power equals SET");
}

// Sends one drafted field and waits for its readback.
bool send(Field field) {
  const auto& s = vtx.status();
  bool accepted = false;
  switch (field) {
    case Band:
    case Channel: accepted = sendChannel(s); break;
    case Power: accepted = sendPower(s, draftPower); break;
    case Pit: accepted = vtx.setPit(draftPit); break;
    default: return false;
  }
  if (accepted) {
    edited |= fieldMask(field);
    pendingField = field;
    pendingSince = millis();
    notify("Sent; check reported settings above");
  } else notify("Unavailable; see VTX status");
  return accepted;
}

// `save` sends the drafts one at a time, each after the previous readback.
void sendQueued() {
  for (Field field : {Channel, Power, Pit}) {
    if (!(applyQueue & fieldMask(field))) continue;
    applyQueue &= ~fieldMask(field);
    send(field);
    return;
  }
}

void syncDrafts() {
  const auto& s = vtx.status();
  if (pendingField < FieldCount && millis() - pendingSince > 6500) {
    edited &= ~fieldMask(pendingField);
    pendingField = FieldCount;
    lastResponses = 0;
    notify("Setting not confirmed; showing readback");
  }
  // The previous draft may be confirmed by a reply from before its command went
  // out, when the VTX was there already: wait until that command is through.
  if (pendingField == FieldCount && applyQueue && s.connected && vtx.canSet()) sendQueued();
  if (!s.connected || s.validResponses == lastResponses) return;
  lastResponses = s.validResponses;
  if (pendingField < FieldCount) {
    bool confirmed = false;
    switch (pendingField) {
      case Band:
      case Channel: confirmed = channelConfirmed(s); break;
      case Power: confirmed = powerConfirmed(s, draftPower); break;
      case Pit: confirmed = s.pitMode == draftPit; break;
      default: break;
    }
    if (confirmed) {
      edited &= ~fieldMask(pendingField);
      pendingField = FieldCount;
      notify("Setting confirmed by VTX readback");
    }
  }
  if (!(edited & channelEditMask)) followChannel(s);
  if (!(edited & (1 << Power))) {
    const int level = reportedPower(s);
    if (level >= 0) draftPower = level;
  }
  if (!(edited & (1 << Pit))) draftPit = s.pitMode;
  screenDirty = true;
}

void apply() {
  if (pendingField < FieldCount) { notify("Waiting for setting readback"); return; }
  switch (selected) {
    case Protocol: selectProtocol(draftProtocol); return;
    case Image: selectImage(pattern, true); return;
    case Probe: selectControlPin(draftControlPin); return;
    default: send(selected);
  }
}

void change(int direction) {
  if (pendingField == selected || (isChannelField(pendingField) && isChannelField(selected))) {
    notify("Waiting for setting readback"); return;
  }
  const auto& s = vtx.status();
  switch (selected) {
    case Band:
      if (!table.bands) { notify("No vtxtable; enter one on the serial console"); return; }
      draftBand = (draftBand + table.bands + direction) % table.bands;
      break;
    case Channel:
      if (!table.channels) { notify("No vtxtable; enter one on the serial console"); return; }
      draftChannel = (draftChannel + table.channels + direction) % table.channels;
      break;
    case Power: {
      const uint8_t count = powerLevelCount(s);
      if (!count) {
        notify(s.protocol == VtxProtocol::SmartAudio ? "Power table unavailable for this VTX" : "No supported power levels reported");
        return;
      }
      draftPower = (draftPower + count + direction) % count;
      break;
    }
    case Pit: draftPit = !draftPit; break;
    case Protocol:
      draftProtocol = draftProtocol == VtxProtocol::Tramp ? VtxProtocol::SmartAudio : VtxProtocol::Tramp;
      break;
    case Image:
      selectImage((pattern + imageOptionCount() + direction) % imageOptionCount());
      return;
    case Probe: {
      size_t index = 0;
      while (index < Board::controlPinCount && Board::controlPins[index] != draftControlPin) ++index;
      draftControlPin = Board::controlPins[(index + Board::controlPinCount + direction) % Board::controlPinCount];
      screenDirty = true;
      char text[64];
      snprintf(text, sizeof(text), "Hold %s to select pin and probe", Board::applyButton);
      notify(text);
      return;
    }
    default: return;
  }
  edited |= (1 << selected);
  screenDirty = true;
}

// The setup list: only the protocol can be chosen on the board, so B and C change
// it and holding C saves it. The vtxtable comes over serial.
void setupButton(const char* key) {
  if (!strcmp(key, "b") || !strcmp(key, "c")) {
    draftProtocol = draftProtocol == VtxProtocol::Tramp ? VtxProtocol::SmartAudio : VtxProtocol::Tramp;
    screenDirty = true;
  } else if (!strcmp(key, "holdc")) selectProtocol(draftProtocol);
  else if (!strcmp(key, "a") || !strcmp(key, "holdb")) notify("Enter the vtxtable over USB serial");
}

void button(const char* key) {
  if (!setupComplete()) { setupButton(key); return; }
  if (!strcmp(key, "a")) { selected = static_cast<Field>((selected + 1) % FieldCount); screenDirty = true; }
  else if (!strcmp(key, "b")) change(-1);
  else if (!strcmp(key, "c")) change(1);
  else if (!strcmp(key, "holdc")) apply();
  else if (!strcmp(key, "holdb")) {
    vtx.reprobe(); edited = 0; pendingField = FieldCount;
    draftControlPin = controlPin; draftProtocol = controlProtocol;
    notify("Probing selected protocol and pin");
  }
}

// Render each changed strip off-screen, then transfer the finished pixels.
// A single row-high RGB565 canvas, 14 KB on the Basic's 320x22, leaves RAM for
// video and UART.
struct CachedLine {
  char left[64] = {};
  char right[48] = {};
  uint16_t color = 0, background = 0;
  bool valid = false;
};
CachedLine lineCache[FieldCount + 5];
static_assert(layout.fieldRows > 0 && layout.fieldRows <= FieldCount, "Field rows are slots 2 onwards");
static_assert(4 + Board::setupNoteLines <= FieldCount + 2, "Setup notes are slots 4 onwards");

// Between the setup list and the main screen: start from black, redraw every row.
void clearDisplay() {
  for (auto& line : lineCache) line.valid = false;
  if (spiBusMutex) xSemaphoreTake(spiBusMutex, portMAX_DELAY);
  M5.Display.fillScreen(TFT_BLACK);
  if (spiBusMutex) xSemaphoreGive(spiBusMutex);
}

void drawDisplayLine(uint8_t slot, int y, int height, const char* left,
                     const char* right, uint16_t color, uint16_t background = TFT_BLACK) {
  auto& previous = lineCache[slot];
  if (previous.valid && previous.color == color && previous.background == background &&
      !strcmp(previous.left, left) && !strcmp(previous.right, right)) return;

  // SD and LCD share this physical bus. Never block the button/UART loop on a
  // card initialization or slow read; leave this row dirty and try again.
  if (spiBusMutex && xSemaphoreTake(spiBusMutex, 0) != pdTRUE) {
    displayDeferred = true;
    return;
  }

  auto& lcd = M5.Display;
  auto& surface = displayBuffered ? static_cast<lgfx::LovyanGFX&>(displayLine)
                                  : static_cast<lgfx::LovyanGFX&>(lcd);
  const int top = displayBuffered ? 0 : y;
  surface.setFont(&fonts::Font2);
  surface.setTextSize(1);
  surface.setTextWrap(false);
  surface.fillRect(0, top, layout.width, height, background);
  surface.setTextColor(color, background);
  const int textY = top + (height > 16 ? 1 : 0);
  surface.setCursor(6, textY);
  surface.print(left);
  if (*right) {
    // A label wider than the value column pushes its value along.
    int rightX = 14 + surface.textWidth(left);
    if (rightX < layout.valueX) rightX = layout.valueX;
    surface.setCursor(rightX, textY);
    surface.print(right);
  }
  if (displayBuffered) {
    // The reusable sprite is taller than the footer; clip to this strip so a
    // changed row cannot overwrite the neighboring row's cached pixels.
    lcd.setClipRect(0, y, layout.width, height);
    displayLine.pushSprite(0, y);
    lcd.clearClipRect();
  }
  if (spiBusMutex) xSemaphoreGive(spiBusMutex);
  snprintf(previous.left, sizeof(previous.left), "%s", left);
  snprintf(previous.right, sizeof(previous.right), "%s", right);
  previous.color = color;
  previous.background = background;
  previous.valid = true;
  ++displayTransfers;
}

// The notice, then the key help at the bottom of the screen.
void drawFooter(const char* const* keys, size_t keyLines) {
  const int keysTop = layout.height - int(keyLines) * layout.keyHeight;
  char text[64];
  snprintf(text, sizeof(text), "%.43s", notice);
  drawDisplayLine(FieldCount + 2, keysTop - layout.noticeHeight, layout.noticeHeight, text, "", TFT_YELLOW);
  for (size_t i = 0; i < keyLines; ++i)
    drawDisplayLine(FieldCount + 3 + i, keysTop + int(i) * layout.keyHeight, layout.keyHeight, keys[i], "", TFT_LIGHTGREY);
}

void drawSetupScreen() {
  const auto tick = [](bool done) { return done ? "[x]" : "[ ]"; };
  const int rows = layout.rowHeight, items = layout.setupItemsY;
  char text[64];
  drawDisplayLine(0, 0, rows, "SETUP", "", TFT_YELLOW);
  snprintf(text, sizeof(text), "%s VTX bands / channels", tick(setupBandsDone()));
  drawDisplayLine(1, items, rows, text, "", TFT_WHITE);
  snprintf(text, sizeof(text), "%s VTX power levels", tick(setupPowerDone()));
  drawDisplayLine(2, items + rows, rows, text, "", TFT_WHITE);
  snprintf(text, sizeof(text), "%s VTX protocol", tick(protocolSaved));
  drawDisplayLine(3, items + 2 * rows, rows, text, protocolChoiceName(draftProtocol), TFT_WHITE, protocolSaved ? TFT_BLACK : 0x1928);
  for (size_t i = 0; i < Board::setupNoteLines; ++i)
    drawDisplayLine(4 + i, layout.setupNotesY + int(i) * rows, rows, Board::setupNotes[i], "", TFT_LIGHTGREY);
  drawFooter(Board::setupKeys, Board::setupKeyLines);
}

void drawScreen() {
  const auto& s = vtx.status();
  displayDeferred = false;
  const bool setup = !setupComplete();
  if (setup != setupShown) {
    clearDisplay();
    setupShown = setup;
    if (!setup) notify("Setup complete");
  }
  if (setup) {
    drawSetupScreen();
    screenDirty = displayDeferred;
    return;
  }
  const int rows = layout.rowHeight;
  char text[64], value[48];
  if (s.connected) {
    if (s.powerMilliwatts) snprintf(text, sizeof(text), "%s %u MHz %u mW", VtxManager::protocolName(s.protocol), s.frequencyMHz, s.powerMilliwatts);
    else snprintf(text, sizeof(text), "%s %u MHz power #%d", VtxManager::protocolName(s.protocol), s.frequencyMHz, s.powerIndex);
  } else if (s.probing) snprintf(text, sizeof(text), "%s: probing %u/%u  %lu", protocolChoiceName(controlProtocol), s.probeIndex + 1, s.probeCount, static_cast<unsigned long>(s.baud));
  else snprintf(text, sizeof(text), "No %s reply - hold B to retry", protocolChoiceName(controlProtocol));
  drawDisplayLine(0, 0, rows, text, "", s.connected ? TFT_GREEN : TFT_YELLOW);
  if (!movieProgress(text, sizeof(text))) {
    if (s.connected) snprintf(text, sizeof(text), "Pit:%s  G%u:%lu%s", s.pitMode ? "ON" : "OFF", controlPin, static_cast<unsigned long>(s.baud), s.inverted ? " inv" : "");
    else snprintf(text, sizeof(text), "%s", s.stateText);
  }
  drawDisplayLine(1, rows, rows, text, "", TFT_LIGHTGREY);
  // The field rows that fit, scrolled to keep the selection on the screen.
  if (selected < firstShownField) firstShownField = selected;
  else if (selected >= firstShownField + layout.fieldRows) firstShownField = selected - layout.fieldRows + 1;
  for (uint8_t line = 0; line < layout.fieldRows; ++line) {
    const uint8_t row = firstShownField + line;
    snprintf(text, sizeof(text), "%s%s", fieldNames[row], (edited & (1 << row)) ? " *" : "");
    switch (row) {
      case Band:
        if (draftBand < table.bands) snprintf(value, sizeof(value), "%s (%c)", table.names[draftBand], table.letters[draftBand]);
        else snprintf(value, sizeof(value), "No vtxtable");
        break;
      case Channel:
        if (draftFrequency()) snprintf(value, sizeof(value), "%u / %u  %u MHz", draftChannel + 1, table.channels, draftFrequency());
        else snprintf(value, sizeof(value), "%u / %u  not available", draftChannel + 1, table.channels);
        break;
      case Power: powerText(s, draftPower, value, sizeof(value)); break;
      case Pit: snprintf(value, sizeof(value), "%s", draftPit ? "ON" : "OFF"); break;
      case Protocol: snprintf(value, sizeof(value), "%s", protocolChoiceName(draftProtocol)); break;
      case Image:
        if (pattern < builtInImages) snprintf(value, sizeof(value), "%s", patternNames[pattern]);
#if M5VTX_HAS_SD
        else if (pattern < builtInImages + sdVideo.count()) {
          const auto entry = sdVideo.entry(pattern - builtInImages);
          snprintf(value, sizeof(value), "%.24s", *entry.title ? entry.title : entry.name);
        } else snprintf(value, sizeof(value), "Rescan SD (hold %s)", Board::applyButton);
#endif
        break;
      case Probe: snprintf(value, sizeof(value), "GPIO%u - hold %s", draftControlPin, Board::applyButton); break;
    }
    drawDisplayLine(2 + line, (2 + line) * rows, rows, text, value, TFT_WHITE,
                    selected == row ? 0x1928 : TFT_BLACK);
  }
  drawFooter(Board::mainKeys, Board::mainKeyLines);
  screenDirty = displayDeferred;
}

// The Betaflight CLI subset: vtxtable, set vtx_*, get, save, diff and dump, so
// that a transmitter's vtxtable file can be pasted as it is.

void printTable() {
  char text[160];
  for (int n = 0; table.line(n, text, sizeof(text)); ++n) Serial.println(text);
}

void printSettings() {
  Serial.printf("set vtx_band = %u\nset vtx_channel = %u\nset vtx_power = %u\nset vtx_freq = %u\n",
                draftBand + 1, draftChannel + 1, draftPower + 1, draftFrequency());
}

bool saveTable() {
  Preferences preferences;
  if (!preferences.begin("m5vtx", false)) return false;
  const bool saved = preferences.putBytes("vtxtable", &table, sizeof(table)) == sizeof(table);
  preferences.end();
  if (saved) tableSaved = true;
  return saved;
}

// A board without a saved table gets the usual five bands, not an empty table,
// for the serial commands; the setup list still asks for one.
void loadTable() {
  Preferences preferences;
  if (preferences.begin("m5vtx", true)) {
    if (preferences.getBytesLength("vtxtable") == sizeof(table))
      tableSaved = preferences.getBytes("vtxtable", &table, sizeof(table)) == sizeof(table) && table.valid();
    preferences.end();
  }
  if (!tableSaved) table.reset();
  clampDrafts();
}

// Betaflight's `defaults`: the VTX configuration goes, the board restarts into setup.
void clearVtxConfiguration() {
  Preferences preferences;
  if (preferences.begin("m5vtx", false)) {
    preferences.remove("vtxtable");
    preferences.remove("protocol");
    preferences.remove("sa2-offset");
    preferences.end();
  }
  Serial.println("VTX configuration cleared; restarting into setup");
  Serial.flush();
  delay(100);
  ESP.restart();
}

// `set name = value`. The vtx_* settings are drafted, as on the screen, and sent by
// `save`; a dump's other settings are a flight controller's and pass in silence.
void setVariable(char* arguments) {
  char* name = arguments;
  while (*name == ' ') ++name;
  char* cursor = name;
  while (*cursor && *cursor != ' ' && *cursor != '=') ++cursor;
  if (*cursor) *cursor++ = 0;
  while (*cursor == ' ' || *cursor == '=') ++cursor;
  if (strncmp(name, "vtx_", 4)) return;
  if (!strcmp(name, "vtx_freq")) {
    Serial.println("# vtx_freq follows vtx_band and vtx_channel here; `freq MHz` sets a frequency directly");
    return;
  }
  const bool isBand = !strcmp(name, "vtx_band"), isChannel = !strcmp(name, "vtx_channel"), isPower = !strcmp(name, "vtx_power");
  if (!isBand && !isChannel && !isPower) return;
  int value;
  if (sscanf(cursor, "%d", &value) != 1) { Serial.printf("###ERROR: INVALID VALUE FOR %s\n", name); return; }
  const int most = isBand ? table.bands : isChannel ? table.channels : powerLevelCount(vtx.status());
  if (value < 1 || value > most) {
    if (isBand && value == 0) Serial.println("###ERROR: vtx_band 0 (direct frequency) is `freq MHz` here");
    else Serial.printf("###ERROR: %s MUST BE 1..%d\n", name, most);
    return;
  }
  if (isBand) draftBand = value - 1;
  else if (isChannel) draftChannel = value - 1;
  else draftPower = value - 1;
  edited |= isPower ? uint8_t(1 << Power) : channelEditMask;
  screenDirty = true;
  Serial.printf("%s = %d\n", name, value);
}

void execute(char* line) {
  int value;
  bool queued = false;
  while (*line == ' ') ++line;
  if (!*line || *line == '#') return;
  if (!strcmp(line, "status")) { report(); return; }
  if (!strcmp(line, "media")) { reportMedia(true); return; }
#if M5VTX_HAS_SD
  if (!strcmp(line, "sdscan")) { rescanVideos(); return; }
#else
  if (!strcmp(line, "sdscan") || !strncmp(line, "play ", 5)) {
    Serial.printf("###ERROR: the %s has no SD card slot\n", Board::name); return;
  }
#endif
  if (!strcmp(line, "probe")) {
    vtx.reprobe(); edited = 0; pendingField = FieldCount;
    draftControlPin = controlPin; draftProtocol = controlProtocol; return;
  }
  if (!strcmp(line, "query")) { vtx.requestStatus(); return; }
  if (!strcmp(line, "protocol smartaudio")) { selectProtocol(VtxProtocol::SmartAudio); return; }
  if (!strcmp(line, "protocol tramp")) { selectProtocol(VtxProtocol::Tramp); return; }
  if (!strcmp(line, "protocol auto")) { selectProtocol(VtxProtocol::Unknown); return; }
  if (!strncmp(line, "button ", 7)) { button(line + 7); return; }
  if (!strcmp(line, "vtxtable")) { printTable(); return; }
  if (!strncmp(line, "vtxtable ", 9)) {
    const char* error = table.command(line + 9);
    if (error) Serial.printf("###ERROR: %s\n", error);
    clampDrafts();
    screenDirty = true;
    return;
  }
  if (!strncmp(line, "set ", 4)) { setVariable(line + 4); return; }
  if (!strcmp(line, "get vtx") || !strncmp(line, "get vtx_", 8)) { printSettings(); return; }
  if (!strcmp(line, "diff") || !strcmp(line, "dump") || !strncmp(line, "diff ", 5) || !strncmp(line, "dump ", 5)) {
    printTable();
    printSettings();
    return;
  }
  if (!strcmp(line, "save")) {
    applyQueue |= edited & (channelEditMask | (1 << Power));
    if (!saveTable()) notify("vtxtable not saved");
    else if (applyQueue && !vtx.status().connected) notify("Saved; drafts are sent once connected");
    else notify(applyQueue ? "Saved; sending the drafts" : "vtxtable saved");
    return;
  }
  if (!strcmp(line, "batch start")) { inBatch = true; return; }
  if (!strcmp(line, "batch end")) { inBatch = false; return; }
  if (!strcmp(line, "defaults")) { clearVtxConfiguration(); return; }
  if (!strncmp(line, "vtx ", 4)) return;  // a dump's aux-channel ranges
#if M5VTX_HAS_SD
  if (sscanf(line, "play %d", &value) == 1 && value >= 0 && value < int(sdVideo.count())) {
    selectImage(value + builtInImages); return;
  }
#endif
  if (sscanf(line, "image %d", &value) == 1 && value >= 0 && value < int(imageOptionCount())) {
    selectImage(value, true); return;
  }
  if (sscanf(line, "pin %d", &value) == 1 && Board::isControlPin(value)) { selectControlPin(value); return; }
  if (sscanf(line, "sa2-offset %d", &value) == 1 && (value == 0 || value == 1)) { setSa2Offset(value); return; }
  if (sscanf(line, "freq %d", &value) == 1 && value >= 0 && value <= 65535) queued = vtx.setFrequency(value);
  else if (sscanf(line, "power-index %d", &value) == 1 && value >= 0 && value < 8) queued = vtx.setPowerIndex(value);
  else if (sscanf(line, "power %d", &value) == 1 && value > 0 && value <= 65535) queued = vtx.setPower(value);
  else if (sscanf(line, "pit %d", &value) == 1 && (value == 0 || value == 1)) queued = vtx.setPit(value);
  else if (sscanf(line, "pattern %d", &value) == 1 && value >= 0 && value < int(builtInImages)) {
    selectImage(value); return;
  } else {
    if (inBatch) return;
    char pins[24] = {};
    for (size_t i = 0; i < Board::controlPinCount; ++i) {
      const size_t used = strlen(pins);
      snprintf(pins + used, sizeof(pins) - used, i ? "/%u" : "%u", Board::controlPins[i]);
    }
    Serial.printf("Commands: status | media | %simage N | pattern 0..4 | probe | query | protocol smartaudio/tramp/auto | pin %s | sa2-offset 0/1 | freq MHz | power mW | power-index N | pit 0/1 | button a/b/c/holdc/holdb\n",
                  Board::hasSd ? "sdscan | play N | " : "", pins);
    Serial.println("Betaflight: vtxtable [bands N | channels N | band N NAME L FACTORY/CUSTOM F1..FN | powerlevels N | powervalues ... | powerlabels ...] | set vtx_band/vtx_channel/vtx_power = N | get vtx | save | diff | defaults");
    return;
  }
  Serial.printf("COMMAND %s %s\n", line, queued ? "queued (requires readback)" : "rejected");
}

void readConsole() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      commandLine[commandLength] = 0;
      if (commandLength) execute(commandLine);
      commandLength = 0;
    } else if (commandLength + 1 < sizeof(commandLine)) commandLine[commandLength++] = c;
    else commandLength = 0;
  }
}

// The M5Unified side of the board: what to assume if detection fails, and the
// buttons that stand for A, B and C.
#if M5VTX_BOARD == M5VTX_BOARD_STICKC_PLUS2
constexpr m5::board_t m5Board = m5::board_t::board_M5StickCPlus2;

// Front A, side B and the power button. Holding A applies, as holding C does on
// the Basic. The power button is only clicked: holding it turns the stick off.
void beginButtons() {
  M5.BtnA.setHoldThresh(800);
  M5.BtnB.setHoldThresh(800);
}

void readButtons() {
  if (M5.BtnA.wasClicked()) button("a");
  if (M5.BtnPWR.wasClicked()) button("b");
  if (M5.BtnB.wasClicked()) button("c");
  if (M5.BtnB.wasHold()) button("holdb");
  if (M5.BtnA.wasHold()) button("holdc");
}
#else
constexpr m5::board_t m5Board = m5::board_t::board_M5Stack;

void beginButtons() {
  M5.BtnB.setHoldThresh(800);
  M5.BtnC.setHoldThresh(800);
}

void readButtons() {
  if (M5.BtnA.wasClicked()) button("a");
  if (M5.BtnB.wasClicked()) button("b");
  if (M5.BtnC.wasClicked()) button("c");
  if (M5.BtnB.wasHold()) button("holdb");
  if (M5.BtnC.wasHold()) button("holdc");
}
#endif
}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;  // the StickC Plus2's microphone clock is G0, a control pin there
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.external_display_value = 0;
  cfg.fallback_board = m5Board;
  cfg.output_power = true;
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(130);
  M5.Display.fillScreen(TFT_BLACK);  // Clear once at startup, never while refreshing.
  displayLine.setColorDepth(16);
  displayLine.setPsram(false);
  displayBuffered = displayLine.createSprite(layout.width, layout.rowHeight + 1) != nullptr;
  if (!displayBuffered) Serial.println("UI line buffer unavailable; updating changed rows directly");
  beginButtons();
  if (Board::speakerPin >= 0) {
    // The speaker shares GPIO25/DAC1. Video exclusively owns DAC2 on GPIO26.
    dac_output_disable(DAC_CHANNEL_1);
    pinMode(Board::speakerPin, OUTPUT);
    digitalWrite(Board::speakerPin, LOW);
  }
  Serial.printf("\nM5VTX boot: flash=%u XTAL=%u MHz heap=%u\n", ESP.getFlashChipSize(), getXtalFrequencyMhz(), ESP.getFreeHeap());
  Serial.printf("M5 board=%u LCD=%dx%d buttons=%s speaker=disabled sd=%s firmware=\"%s\"\n", unsigned(M5.getBoard()),
                M5.Display.width(), M5.Display.height(), Board::buttons, Board::hasSd ? "slot" : "none", Board::name);
  if (CompositeVideo::begin()) {
    CompositeVideo::showTestPattern(pattern);
    Serial.printf("VIDEO NTSC GPIO%d DAC%d RGB332 256x240\n", CompositeVideo::pin, CompositeVideo::pin == 25 ? 1 : 2);
  } else notify("Video allocation/setup FAILED");
  Preferences preferences;
  if (preferences.begin("m5vtx", true)) {
    controlPin = preferences.getUChar("control-pin", Board::defaultControlPin);
    sa2PowerOffset = preferences.getUChar("sa2-offset", 0);
    protocolSaved = preferences.isKey("protocol");
    const uint8_t savedProtocol = preferences.getUChar("protocol", static_cast<uint8_t>(VtxProtocol::SmartAudio));
    if (savedProtocol <= static_cast<uint8_t>(VtxProtocol::Tramp)) controlProtocol = static_cast<VtxProtocol>(savedProtocol);
    preferences.end();
  }
  loadTable();
  if (!Board::isControlPin(controlPin)) controlPin = Board::defaultControlPin;
  draftControlPin = controlPin;
  draftProtocol = controlProtocol;
  if (sa2PowerOffset > 1) sa2PowerOffset = 0;
  vtx.setSa2PowerOffset(sa2PowerOffset);
  vtx.setKnownPowerCount(sa2DefaultPowerLevels);
  vtx.setProtocol(controlProtocol);
  if (protocolSaved) vtx.begin(controlPin);
  else notify("First-time setup; see README");
  spiBusMutex = xSemaphoreCreateMutex();
#if M5VTX_HAS_SD
  if (spiBusMutex && CompositeVideo::running()) mediaInitialized = sdVideo.begin(spiBusMutex);
  if (!mediaInitialized) notify("SD player unavailable");
#endif
  if (M5.getBoard() != m5Board) {
    char text[64];
    snprintf(text, sizeof(text), "Wrong firmware: this is for the %s", Board::name);
    notify(text);
  }
  drawScreen();
}

void loop() {
  M5.update();
  vtx.update();
  readConsole();
  readButtons();
  syncDrafts();
#if M5VTX_HAS_SD
  syncMedia();
#endif
  const uint32_t now = millis();
  if ((screenDirty && now - lastScreen > 100) || now - lastScreen > 1000) {
    drawScreen(); lastScreen = now;
  }
  if (now - lastReport > 5000) { report(); lastReport = now; }
  delay(1);
}
