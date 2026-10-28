// SPDX-License-Identifier: GPL-3.0-only
#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <driver/dac.h>

#include "composite_video.h"
#include "vtx_manager.h"

namespace {
VtxManager vtx;
M5Canvas displayLine(&M5.Display);
bool displayBuffered = false;
uint32_t displayTransfers = 0;
enum Field : uint8_t { Band, Channel, Power, Pit, Protocol, Image, Probe, FieldCount };
Field selected = Channel;
Field pendingField = FieldCount;
uint32_t pendingSince = 0;
uint8_t draftChannel = 0, draftPower = 0, pattern = 0;
uint8_t controlPin = 16, draftControlPin = 16;
VtxProtocol controlProtocol = VtxProtocol::SmartAudio;
VtxProtocol draftProtocol = VtxProtocol::SmartAudio;
uint8_t knownPowerCount = 0;
bool draftPit = false;
uint8_t edited = 0;
bool screenDirty = true;
uint32_t lastScreen = 0, lastReport = 0, lastResponses = 0;
char notice[64] = "Queries only during discovery";
char commandLine[96] = {};
size_t commandLength = 0;
constexpr uint16_t powerChoices[] = {25, 100, 200, 400, 600, 800, 1000, 1600};
// Measured on this DarwinFPV VTX: SET uses 0..2, GET reports 1..3.
// Set the offset to 0 for a standard SmartAudio v2 device.
constexpr uint8_t sa2GetPowerOffset = 1;
constexpr uint8_t sa2VerifiedPowerCount = 3;
constexpr const char* fieldNames[] = {"Band", "Channel", "Power", "Pit mode", "Protocol", "Image", "Control pin"};
constexpr const char* bandNames[] = {"A", "B", "E", "F", "Raceband (R)"};
constexpr const char* patternNames[] = {"Test card", "256 colors", "Gray ramp"};
constexpr uint8_t channelEditMask = (1 << Band) | (1 << Channel);

bool isChannelField(Field field) { return field == Band || field == Channel; }

const char* protocolChoiceName(VtxProtocol protocol) {
  return protocol == VtxProtocol::Unknown ? "Auto" : VtxManager::protocolName(protocol);
}

void notify(const char* text) {
  snprintf(notice, sizeof(notice), "%s", text);
  Serial.printf("UI %s\n", notice);
  screenDirty = true;
}

void report() {
  const auto& s = vtx.status();
  Serial.printf("STATUS protocol=%s connected=%u probing=%u baud=%lu inverted=%u "
                "frequency=%u power_mw=%u power_index=%d power_raw=%d pit=%u channel=%u "
                "queries=%lu rx=%lu valid=%lu crc_errors=%lu echoes=%lu "
                "video=%u frames=%lu heap=%u pattern=%u\n",
                VtxManager::protocolName(s.protocol), s.connected, s.probing,
                static_cast<unsigned long>(s.baud), s.inverted, s.frequencyMHz,
                s.powerMilliwatts, s.powerIndex, s.rawPowerIndex, s.pitMode, s.channelIndex,
                static_cast<unsigned long>(s.queries), static_cast<unsigned long>(s.rxBytes),
                static_cast<unsigned long>(s.validResponses), static_cast<unsigned long>(s.crcErrors),
                static_cast<unsigned long>(s.echoFrames), CompositeVideo::running(),
                static_cast<unsigned long>(CompositeVideo::frames()), ESP.getFreeHeap(), pattern);
  Serial.printf("STATE %s\n", s.stateText);
  Serial.printf("LINE pin=%u settled=%d rx_edges=%lu pullup=%d pulldown=%d floating=%d\n",
                controlPin, s.settledLevel, static_cast<unsigned long>(s.rxEdges),
                s.linePullUp, s.linePullDown, s.lineFloating);
  Serial.printf("UI_STATE selected=%s draft_band=%c draft_channel=%u draft_index=%u draft_power=%u draft_pit=%u edited=%u pending=%u control_pin=%u draft_pin=%u protocol_mode=%s draft_protocol=%s\n",
                fieldNames[selected], "ABEFR"[draftChannel / 8], draftChannel % 8 + 1,
                draftChannel, draftPower, draftPit, edited, pendingField < FieldCount,
                controlPin, draftControlPin, protocolChoiceName(controlProtocol), protocolChoiceName(draftProtocol));
  Serial.printf("UI_RENDER buffered=%u row_transfers=%lu\n", displayBuffered,
                static_cast<unsigned long>(displayTransfers));
}

void selectControlPin(uint8_t pin) {
  controlPin = draftControlPin = pin;
  draftProtocol = controlProtocol;
  Preferences preferences;
  if (preferences.begin("m5vtx", false)) {
    preferences.putUChar("control-pin", pin);
    preferences.end();
  }
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
  Preferences preferences;
  if (preferences.begin("m5vtx", false)) {
    preferences.putUChar("protocol", static_cast<uint8_t>(protocol));
    preferences.end();
  }
  pendingField = FieldCount;
  edited = 0;
  lastResponses = 0;
  draftPower = 0;
  char text[64];
  snprintf(text, sizeof(text), "Probing %s on GPIO%u", protocolChoiceName(protocol), controlPin);
  notify(text);
}

void syncDrafts() {
  const auto& s = vtx.status();
  if (pendingField < FieldCount && millis() - pendingSince > 6500) {
    edited &= ~(isChannelField(pendingField) ? channelEditMask : (1 << pendingField));
    pendingField = FieldCount;
    lastResponses = 0;
    notify("Setting not confirmed; showing readback");
  }
  if (!s.connected || s.validResponses == lastResponses) return;
  lastResponses = s.validResponses;
  if (s.saVersion == 2 && s.powerCount > knownPowerCount && s.powerCount <= 8) {
    knownPowerCount = s.powerCount;
    Preferences preferences;
    if (preferences.begin("m5vtx", false)) {
      preferences.putUChar("sa2-count-v2", knownPowerCount);
      preferences.end();
    }
  }
  if (pendingField < FieldCount) {
    bool confirmed = false;
    switch (pendingField) {
      case Band:
      case Channel:
        confirmed = s.frequencyMHz == VtxManager::channelFrequency(draftChannel) &&
                    (s.protocol != VtxProtocol::SmartAudio || s.channelIndex == draftChannel);
        break;
      case Power:
        confirmed = s.protocol == VtxProtocol::SmartAudio ? s.powerIndex == draftPower
                                                        : s.powerMilliwatts == powerChoices[draftPower % 8];
        break;
      case Pit: confirmed = s.pitMode == draftPit; break;
      default: break;
    }
    if (confirmed) {
      edited &= ~(isChannelField(pendingField) ? channelEditMask : (1 << pendingField));
      pendingField = FieldCount;
      notify("Setting confirmed by VTX readback");
    }
  }
  if (!(edited & channelEditMask) && s.channelIndex < 40) {
    // Tramp reports frequency only. Keep the selected band/channel when it is
    // an alias for that frequency (e.g. R7 and F8 both mean 5880 MHz).
    if (s.protocol != VtxProtocol::Tramp ||
        VtxManager::channelFrequency(draftChannel) != s.frequencyMHz) draftChannel = s.channelIndex;
  }
  if (!(edited & (1 << Power))) {
    if (s.protocol == VtxProtocol::SmartAudio && s.powerIndex >= 0) draftPower = s.powerIndex;
    else {
      for (uint8_t i = 0; i < 8; ++i) if (s.powerMilliwatts == powerChoices[i]) draftPower = i;
    }
  }
  if (!(edited & (1 << Pit))) draftPit = s.pitMode;
  screenDirty = true;
}

void apply() {
  if (pendingField < FieldCount) { notify("Waiting for setting readback"); return; }
  bool accepted = false;
  const auto& s = vtx.status();
  switch (selected) {
    case Band:
    case Channel: accepted = vtx.setChannel(draftChannel); break;
    case Power:
      accepted = s.protocol == VtxProtocol::SmartAudio ? vtx.setPowerIndex(draftPower)
                                                     : vtx.setPower(powerChoices[draftPower % 8]);
      break;
    case Pit: accepted = vtx.setPit(draftPit); break;
    case Protocol: selectProtocol(draftProtocol); return;
    case Image: CompositeVideo::showTestPattern(pattern); notify("Image displayed on video output"); return;
    case Probe: selectControlPin(draftControlPin); return;
    default: return;
  }
  if (accepted) {
    edited |= (1 << selected);
    pendingField = selected;
    pendingSince = millis();
    notify("Sent; check reported settings above");
  } else notify("Unavailable; see VTX status");
}

void change(int direction) {
  if (pendingField == selected || (isChannelField(pendingField) && isChannelField(selected))) {
    notify("Waiting for setting readback"); return;
  }
  const auto& s = vtx.status();
  switch (selected) {
    case Band: draftChannel = ((draftChannel / 8 + 5 + direction) % 5) * 8 + draftChannel % 8; break;
    case Channel: draftChannel = (draftChannel / 8) * 8 + (draftChannel % 8 + 8 + direction) % 8; break;
    case Power: {
      uint8_t count = 8;
      if (s.protocol == VtxProtocol::SmartAudio) {
        count = s.powerCount;
        if (!count) { notify("Power table unavailable for this VTX"); return; }
      } else if (s.maxPowerMilliwatts) {
        count = 0;
        while (count < 8 && powerChoices[count] <= s.maxPowerMilliwatts) ++count;
      }
      if (!count) { notify("No supported power levels reported"); return; }
      draftPower = (draftPower + count + direction) % count;
      break;
    }
    case Pit: draftPit = !draftPit; break;
    case Protocol:
      draftProtocol = draftProtocol == VtxProtocol::Tramp ? VtxProtocol::SmartAudio : VtxProtocol::Tramp;
      break;
    case Image:
      pattern = (pattern + 3 + direction) % 3;
      CompositeVideo::showTestPattern(pattern);
      screenDirty = true;
      return;
    case Probe:
      draftControlPin = draftControlPin == 17 ? 16 : 17;
      screenDirty = true;
      notify("Hold C to select pin and probe");
      return;
    default: return;
  }
  edited |= (1 << selected);
  screenDirty = true;
}

void button(const char* key) {
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
// A single 320x22 RGB565 canvas uses 14 KB, leaving RAM for video and UART.
void drawDisplayLine(uint8_t slot, int y, int height, const char* left,
                     const char* right, uint16_t color, uint16_t background = TFT_BLACK) {
  struct CachedLine {
    char left[64] = {};
    char right[48] = {};
    uint16_t color = 0, background = 0;
    bool valid = false;
  };
  static CachedLine cache[FieldCount + 5];
  auto& previous = cache[slot];
  if (previous.valid && previous.color == color && previous.background == background &&
      !strcmp(previous.left, left) && !strcmp(previous.right, right)) return;

  auto& lcd = M5.Display;
  auto& surface = displayBuffered ? static_cast<lgfx::LovyanGFX&>(displayLine)
                                  : static_cast<lgfx::LovyanGFX&>(lcd);
  const int top = displayBuffered ? 0 : y;
  surface.setFont(&fonts::Font2);
  surface.setTextSize(1);
  surface.setTextWrap(false);
  surface.fillRect(0, top, 320, height, background);
  surface.setTextColor(color, background);
  const int textY = top + (height > 16 ? 1 : 0);
  surface.setCursor(6, textY);
  surface.print(left);
  if (*right) {
    surface.setCursor(132, textY);
    surface.print(right);
  }
  if (displayBuffered) {
    // The reusable sprite is taller than the footer; clip to this strip so a
    // changed row cannot overwrite the neighboring row's cached pixels.
    lcd.setClipRect(0, y, 320, height);
    displayLine.pushSprite(0, y);
    lcd.clearClipRect();
  }
  snprintf(previous.left, sizeof(previous.left), "%s", left);
  snprintf(previous.right, sizeof(previous.right), "%s", right);
  previous.color = color;
  previous.background = background;
  previous.valid = true;
  ++displayTransfers;
}

void drawScreen() {
  const auto& s = vtx.status();
  char text[64], value[48];
  if (s.connected) {
    if (s.powerMilliwatts) snprintf(text, sizeof(text), "%s %u MHz %u mW", VtxManager::protocolName(s.protocol), s.frequencyMHz, s.powerMilliwatts);
    else snprintf(text, sizeof(text), "%s %u MHz power #%d", VtxManager::protocolName(s.protocol), s.frequencyMHz, s.powerIndex);
  } else if (s.probing) snprintf(text, sizeof(text), "%s: probing %u/%u  %lu", protocolChoiceName(controlProtocol), s.probeIndex + 1, s.probeCount, static_cast<unsigned long>(s.baud));
  else snprintf(text, sizeof(text), "No %s reply - hold B to retry", protocolChoiceName(controlProtocol));
  drawDisplayLine(0, 0, 21, text, "", s.connected ? TFT_GREEN : TFT_YELLOW);
  if (s.connected) snprintf(text, sizeof(text), "Pit:%s  G%u:%lu%s", s.pitMode ? "ON" : "OFF", controlPin, static_cast<unsigned long>(s.baud), s.inverted ? " inv" : "");
  else snprintf(text, sizeof(text), "%s", s.stateText);
  drawDisplayLine(1, 21, 21, text, "", TFT_LIGHTGREY);
  for (uint8_t row = 0; row < FieldCount; ++row) {
    snprintf(text, sizeof(text), "%s%s", fieldNames[row], (edited & (1 << row)) ? " *" : "");
    switch (row) {
      case Band: snprintf(value, sizeof(value), "%s", bandNames[draftChannel / 8]); break;
      case Channel: snprintf(value, sizeof(value), "%u / 8  %u MHz", draftChannel % 8 + 1, VtxManager::channelFrequency(draftChannel)); break;
      case Power:
        if (s.protocol == VtxProtocol::SmartAudio) {
          if (s.saVersion == 21 && draftPower < s.powerCount) snprintf(value, sizeof(value), "#%u (%u dBm)", draftPower, s.powerLevelsDbm[draftPower]);
          else snprintf(value, sizeof(value), "Level #%u", draftPower);
        } else snprintf(value, sizeof(value), "%u mW", powerChoices[draftPower % 8]);
        break;
      case Pit: snprintf(value, sizeof(value), "%s", draftPit ? "ON" : "OFF"); break;
      case Protocol: snprintf(value, sizeof(value), "%s", protocolChoiceName(draftProtocol)); break;
      case Image: snprintf(value, sizeof(value), "%s", patternNames[pattern]); break;
      case Probe: snprintf(value, sizeof(value), "GPIO%u - hold C", draftControlPin); break;
    }
    drawDisplayLine(2 + row, 42 + row * 21, 21, text, value, TFT_WHITE,
                    selected == row ? 0x1928 : TFT_BLACK);
  }
  snprintf(text, sizeof(text), "%.43s", notice);
  drawDisplayLine(FieldCount + 2, 190, 18, text, "", TFT_YELLOW);
  drawDisplayLine(FieldCount + 3, 208, 16, "A: next    B: -    C: +", "", TFT_LIGHTGREY);
  drawDisplayLine(FieldCount + 4, 224, 16, "Hold C: apply   Hold B: probe", "", TFT_LIGHTGREY);
  screenDirty = false;
}

void execute(char* line) {
  int value;
  bool queued = false;
  if (!strcmp(line, "status")) { report(); return; }
  if (!strcmp(line, "probe")) {
    vtx.reprobe(); edited = 0; pendingField = FieldCount;
    draftControlPin = controlPin; draftProtocol = controlProtocol; return;
  }
  if (!strcmp(line, "query")) { vtx.requestStatus(); return; }
  if (!strcmp(line, "protocol smartaudio")) { selectProtocol(VtxProtocol::SmartAudio); return; }
  if (!strcmp(line, "protocol tramp")) { selectProtocol(VtxProtocol::Tramp); return; }
  if (!strcmp(line, "protocol auto")) { selectProtocol(VtxProtocol::Unknown); return; }
  if (!strncmp(line, "button ", 7)) { button(line + 7); return; }
  if (sscanf(line, "pin %d", &value) == 1 && (value == 16 || value == 17)) { selectControlPin(value); return; }
  if (sscanf(line, "freq %d", &value) == 1 && value >= 0 && value <= 65535) queued = vtx.setFrequency(value);
  else if (sscanf(line, "channel %d", &value) == 1 && value >= 0 && value < 40) queued = vtx.setChannel(value);
  else if (sscanf(line, "power-index %d", &value) == 1 && value >= 0 && value < 8) queued = vtx.setPowerIndex(value);
  else if (sscanf(line, "power %d", &value) == 1 && value > 0 && value <= 65535) queued = vtx.setPower(value);
  else if (sscanf(line, "pit %d", &value) == 1 && (value == 0 || value == 1)) queued = vtx.setPit(value);
  else if (sscanf(line, "pattern %d", &value) == 1 && value >= 0 && value < 3) {
    pattern = value; CompositeVideo::showTestPattern(pattern); notify("Video pattern changed"); return;
  } else {
    Serial.println("Commands: status | probe | query | protocol smartaudio/tramp/auto | pin 16/17 | freq MHz | channel 0..39 | power mW | power-index N | pit 0/1 | pattern 0..2 | button a/b/c/holdc/holdb");
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
}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.external_display_value = 0;
  cfg.fallback_board = m5::board_t::board_M5Stack;
  cfg.output_power = true;
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(130);
  M5.Display.fillScreen(TFT_BLACK);  // Clear once at startup, never while refreshing.
  displayLine.setColorDepth(16);
  displayLine.setPsram(false);
  displayBuffered = displayLine.createSprite(320, 22) != nullptr;
  if (!displayBuffered) Serial.println("UI line buffer unavailable; updating changed rows directly");
  M5.BtnB.setHoldThresh(800);
  M5.BtnC.setHoldThresh(800);
  // The speaker shares GPIO25/DAC1. Video exclusively owns DAC2 on GPIO26.
  dac_output_disable(DAC_CHANNEL_1);
  pinMode(25, OUTPUT);
  digitalWrite(25, LOW);
  Serial.printf("\nM5VTX boot: flash=%u XTAL=%u MHz heap=%u\n", ESP.getFlashChipSize(), getXtalFrequencyMhz(), ESP.getFreeHeap());
  Serial.printf("M5 board=%u LCD=%dx%d buttons=A39/B38/C37 speaker=disabled\n", unsigned(M5.getBoard()), M5.Display.width(), M5.Display.height());
  if (CompositeVideo::begin()) {
    CompositeVideo::showTestPattern(pattern);
    Serial.println("VIDEO NTSC GPIO26 DAC2 RGB332 256x240 (speaker disabled)");
  } else notify("Video allocation/setup FAILED");
  Preferences preferences;
  if (preferences.begin("m5vtx", true)) {
    controlPin = preferences.getUChar("control-pin", 16);
    knownPowerCount = preferences.getUChar("sa2-count-v2", sa2VerifiedPowerCount);
    const uint8_t savedProtocol = preferences.getUChar("protocol", static_cast<uint8_t>(VtxProtocol::SmartAudio));
    if (savedProtocol <= static_cast<uint8_t>(VtxProtocol::Tramp)) controlProtocol = static_cast<VtxProtocol>(savedProtocol);
    preferences.end();
  }
  if (controlPin != 16 && controlPin != 17) controlPin = 16;
  draftControlPin = controlPin;
  draftProtocol = controlProtocol;
  if (knownPowerCount == 0 || knownPowerCount > sa2VerifiedPowerCount) knownPowerCount = sa2VerifiedPowerCount;
  vtx.setSa2PowerOffset(sa2GetPowerOffset);
  vtx.setKnownPowerCount(knownPowerCount);
  vtx.setProtocol(controlProtocol);
  vtx.begin(controlPin);
  drawScreen();
}

void loop() {
  M5.update();
  vtx.update();
  readConsole();
  if (M5.BtnA.wasClicked()) button("a");
  if (M5.BtnB.wasClicked()) button("b");
  if (M5.BtnC.wasClicked()) button("c");
  if (M5.BtnB.wasHold()) button("holdb");
  if (M5.BtnC.wasHold()) button("holdc");
  syncDrafts();
  const uint32_t now = millis();
  if ((screenDirty && now - lastScreen > 100) || now - lastScreen > 1000) {
    drawScreen(); lastScreen = now;
  }
  if (now - lastReport > 5000) { report(); lastReport = now; }
  delay(1);
}
