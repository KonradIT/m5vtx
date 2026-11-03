// SPDX-License-Identifier: GPL-3.0-only
// Nonblocking ESP32 transport for igorka48/VTXControl_ESP32 protocol codecs.
#include "vtx_manager.h"

#include <VTX_SmartAudio.h>
#include <VTX_Tramp.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_timer.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_struct.h>
#include <rom/gpio.h>
#include <math.h>
#include <string.h>

namespace {
constexpr uint32_t kReplyMs = 700;
constexpr uint32_t kRecoveryMs = 200;
constexpr uint32_t kPollingMs = 1500;
constexpr uint8_t kAttempts = 2;
// Upstream response macros contain unparenthesized shifts/ORs. Materialize them
// before comparisons so C++ operator precedence cannot alter the tests.
constexpr uint8_t kSaSettingsV1 = SMARTAUDIO_RSP_GET_SETTINGS_V1;
constexpr uint8_t kSaSettingsV2 = SMARTAUDIO_RSP_GET_SETTINGS_V2;
constexpr uint8_t kSaSettingsV21 = SMARTAUDIO_RSP_GET_SETTINGS_V21;
bool due(uint32_t now, uint32_t at) { return int32_t(now - at) >= 0; }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
void hexLog(const char* direction, const uint8_t* bytes, size_t length) {
  Serial.printf("VTX %s t=%lu [%u]", direction, (unsigned long)millis(), unsigned(length));
  for (size_t i = 0; i < length; ++i) Serial.printf(" %02X", bytes[i]);
  Serial.println();
}
uint16_t dbmToMw(uint8_t dbm) {
  return dbm < 48 ? uint16_t(lroundf(powf(10.0f, dbm / 10.0f))) : 0;
}
// A pin's output-enable bit. GPIO32..39, such as the M5StickC Plus2's Grove pins,
// have theirs in a second register.
unsigned outputEnabled(int pin) {
  return pin < 32 ? (GPIO.enable >> pin) & 1 : (GPIO.enable1.data >> (pin - 32)) & 1;
}
}

const VtxManager::Candidate VtxManager::candidates_[] = {
    // Floating receive with both dummy bytes: several unlicensed clones need
    // the final zero to complete the command rather than buffering it until
    // the next command's leading zero.
    {VtxProtocol::SmartAudio,4800,false,4},
    // Fallback for devices that do not require the trailing dummy byte.
    {VtxProtocol::SmartAudio,4800,false,1},
    {VtxProtocol::Tramp,9600,false},
    {VtxProtocol::SmartAudio,4800,false},
    {VtxProtocol::SmartAudio,4900,false},
    {VtxProtocol::SmartAudio,4850,false},
    {VtxProtocol::SmartAudio,4950,false},
    {VtxProtocol::SmartAudio,4560,false},
    {VtxProtocol::SmartAudio,4620,false},
    {VtxProtocol::SmartAudio,4680,false},
    {VtxProtocol::SmartAudio,4740,false},
    {VtxProtocol::SmartAudio,4860,false},
    {VtxProtocol::SmartAudio,4920,false},
    {VtxProtocol::SmartAudio,4980,false},
    {VtxProtocol::SmartAudio,5040,false},
    {VtxProtocol::Tramp,9600,true},
    {VtxProtocol::SmartAudio,4800,true},
    {VtxProtocol::SmartAudio,4900,true},
    {VtxProtocol::SmartAudio,4850,true},
    {VtxProtocol::SmartAudio,4950,true},
    {VtxProtocol::SmartAudio,4560,true},
    {VtxProtocol::SmartAudio,4620,true},
    {VtxProtocol::SmartAudio,4680,true},
    {VtxProtocol::SmartAudio,4740,true},
    {VtxProtocol::SmartAudio,4860,true},
    {VtxProtocol::SmartAudio,4920,true},
    {VtxProtocol::SmartAudio,4980,true},
    {VtxProtocol::SmartAudio,5040,true},
    {VtxProtocol::SmartAudio,4900,false,1},
    {VtxProtocol::SmartAudio,4950,false,1},
    {VtxProtocol::SmartAudio,4800,false,2},
    {VtxProtocol::SmartAudio,4900,false,2},
    {VtxProtocol::SmartAudio,4950,false,2},
    {VtxProtocol::SmartAudio,4800,false,3},
    {VtxProtocol::SmartAudio,4900,false,3},
    {VtxProtocol::SmartAudio,4950,false,3},
    {VtxProtocol::Tramp,9600,false,1}};

const char* VtxManager::protocolName(VtxProtocol protocol) {
  switch (protocol) {
    case VtxProtocol::SmartAudio: return "SmartAudio";
    case VtxProtocol::Tramp: return "Tramp";
    default: return "Unknown";
  }
}

void VtxManager::message(const char* text) {
  snprintf(status_.stateText, sizeof(status_.stateText), "%s", text);
  Serial.printf("VTX %s\n", text);
}

void VtxManager::begin(int pin) {
  setPin(pin);
}

bool VtxManager::setProtocol(VtxProtocol protocol) {
  if (protocol != VtxProtocol::Unknown && protocol != VtxProtocol::SmartAudio &&
      protocol != VtxProtocol::Tramp) return false;
  selectedProtocol_ = protocol;
  // Let setup choose the protocol before begin() configures any hardware.
  if (uartStarted_) reprobe();
  return true;
}

uint8_t VtxManager::probeCandidateCount() const {
  uint8_t count = 0;
  for (const auto& candidate : candidates_)
    if (selectedProtocol_ == VtxProtocol::Unknown || candidate.protocol == selectedProtocol_)
      ++count;
  return count;
}

const VtxManager::Candidate* VtxManager::probeCandidate(uint8_t index) const {
  for (const auto& candidate : candidates_) {
    if (selectedProtocol_ != VtxProtocol::Unknown && candidate.protocol != selectedProtocol_) continue;
    if (!index) return &candidate;
    --index;
  }
  return nullptr;
}

bool VtxManager::setPin(int pin) {
  if (!Board::isControlPin(pin)) {
    Serial.printf("VTX invalid control pin %d for the %s\n", pin, Board::name);
    return false;
  }
  // Clean up the current pin before changing pin_: otherwise an old GPIO edge
  // handler, UART output route, or pull resistor could remain attached to it.
  detachEdgeInterrupt();
  if (uartStarted_) {
    uart_.end();
    uartStarted_ = false;
  }
  if (pin_ >= 0) {
    gpio_matrix_out(pin_, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction(gpio_num_t(pin_), GPIO_MODE_INPUT);
    gpio_set_pull_mode(gpio_num_t(pin_), GPIO_FLOATING);
  }
  pin_ = pin;
  uart_.setRxBufferSize(512);
  reprobe();
  return true;
}

void VtxManager::reprobe() {
  if (pin_ < 0) return;  // nothing goes on any wire before begin()
  const uint32_t oldQueries = status_.queries;
  status_ = VtxStatus{};
  status_.queries = oldQueries;
  status_.probing = true;
  status_.probeCount = probeCandidateCount();
  waiting_ = false;
  gotResponse_ = false;
  verifyPending_ = false;
  pendingLength_ = 0;
  rxLength_ = 0;
  attempt_ = pollPhase_ = misses_ = 0;
  diagnoseLine();
  responseEdges_ = 0;
  lastValid_ = 0;
  settingGraceUntil_ = 0;
  lastTxLength_ = 0;
  const Candidate* first = probeCandidate(0);
  if (!first) {
    status_.probing = false;
    activeProtocol_ = VtxProtocol::Unknown;
    message("No transport candidates for selected protocol");
    return;
  }
  configure(*first);
  nextAction_ = millis() + 50;
  message("Read-only protocol discovery");
}

void VtxManager::diagnoseLine() {
  detachEdgeInterrupt();
  if (uartStarted_) { uart_.end(); uartStarted_ = false; }
  gpio_matrix_out(pin_, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction(gpio_num_t(pin_), GPIO_MODE_INPUT);
  gpio_set_pull_mode(gpio_num_t(pin_), GPIO_PULLUP_ONLY);
  delay(1);
  status_.linePullUp = gpio_get_level(gpio_num_t(pin_));
  gpio_set_pull_mode(gpio_num_t(pin_), GPIO_PULLDOWN_ONLY);
  delay(1);
  status_.linePullDown = gpio_get_level(gpio_num_t(pin_));
  gpio_set_pull_mode(gpio_num_t(pin_), GPIO_FLOATING);
  delay(1);
  status_.lineFloating = gpio_get_level(gpio_num_t(pin_));
  Serial.printf("VTX LINE pin=%d weak_pullup=%d weak_pulldown=%d floating=%d (0/1 changing with pull suggests high impedance)\n",
                pin_,status_.linePullUp,status_.linePullDown,status_.lineFloating);
}

void VtxManager::detachEdgeInterrupt() {
  if (edgeInterruptAttached_) {
    detachInterrupt(pin_);
    edgeInterruptAttached_ = false;
  }
}

void IRAM_ATTR VtxManager::onRxEdge(void* argument) {
  auto* self = static_cast<VtxManager*>(argument);
  // Exclude the host output release/weak pull settling. Count independent
  // electrical activity even if the UART cannot frame or decode the bytes.
  if (uint32_t(uint32_t(esp_timer_get_time()) - self->releasedAtMicros_) >= 1000) ++self->responseEdges_;
}

void VtxManager::releaseLine() {
  // Disconnect the UART output matrix before making the shared wire an input.
  // The RX matrix stays connected. No push-pull output opposes the VTX reply.
  gpio_matrix_out(pin_, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction(gpio_num_t(pin_), GPIO_MODE_INPUT);
  int pull = GPIO_PULLUP_ONLY;
  if (status_.transportProfile == 1 || status_.transportProfile == 4) pull = GPIO_FLOATING;
  else if (activeProtocol_ == VtxProtocol::SmartAudio && status_.transportProfile != 2)
    pull = status_.inverted ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY;
  else if (status_.inverted) pull = GPIO_PULLDOWN_ONLY;
  gpio_set_pull_mode(gpio_num_t(pin_), gpio_pull_mode_t(pull));
}

void VtxManager::configure(const Candidate& candidate) {
  if (selectedProtocol_ != VtxProtocol::Unknown && candidate.protocol != selectedProtocol_) return;
  detachEdgeInterrupt();
  if (uartStarted_) uart_.end();
  activeProtocol_ = candidate.protocol;
  status_.baud = candidate.baud;
  status_.inverted = candidate.inverted;
  status_.transportProfile = candidate.profile;
  status_.stopBits = (candidate.protocol == VtxProtocol::SmartAudio && candidate.profile != 3) ? 2 : 1;
  uart_.begin(candidate.baud,
              status_.stopBits == 2 ? SERIAL_8N2 : SERIAL_8N1,
              pin_, pin_, candidate.inverted);
  // Some Arduino 2.x cores only enable inversion in begin(), leaving a previous
  // inverted candidate's register bits set on a later normal reprobe.
  uart_set_line_inverse(UART_NUM_2, candidate.inverted
      ? UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV : UART_SIGNAL_INV_DISABLE);
  uartStarted_ = true;
  // Arduino's same-pin setup connects both matrix routes. Keep RX enabled while
  // transmitting to capture local echo for diagnostics.
  gpio_matrix_in(pin_, U2RXD_IN_IDX, false);
  releaseLine();
  uart_flush_input(UART_NUM_2);
  rxLength_ = 0;
  Serial.printf("VTX PROBE %u/%u %s baud=%lu inverted=%u pin=%d profile=%u stopbits=%u idle=%d\n",
                unsigned(status_.probeIndex + 1), unsigned(status_.probeCount),
                protocolName(candidate.protocol), (unsigned long)candidate.baud,
                candidate.inverted, pin_,candidate.profile,status_.stopBits,gpio_get_level(gpio_num_t(pin_)));
}

void VtxManager::transmit(const uint8_t* bytes, size_t length) {
  if (length > sizeof(lastTx_)) return;
  rxLength_ = 0;
  uart_flush_input(UART_NUM_2);
  memcpy(lastTx_, bytes, length);
  lastTxLength_ = length;
  hexLog("TX", bytes, length);
  detachEdgeInterrupt();
  gpio_set_level(gpio_num_t(pin_), status_.inverted ? 0 : 1);
  gpio_set_direction(gpio_num_t(pin_), GPIO_MODE_INPUT_OUTPUT);
  gpio_matrix_out(pin_, U2TXD_OUT_IDX, false, false);
  uart_.write(bytes, length);
  // Wait only for the bytes on the wire (max ~40ms); UART RX and video ISR run.
  uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(100));
  releaseLine();
  responseEdges_ = 0;
  releasedAtMicros_ = micros();
  lineSampled_ = false;
  attachInterruptArg(pin_, &VtxManager::onRxEdge, this, CHANGE);
  edgeInterruptAttached_ = true;
  gotResponse_ = false;
  waiting_ = true;
  deadline_ = millis() + kReplyMs;
}

void VtxManager::sendQuery() {
  if (activeProtocol_ == VtxProtocol::Unknown ||
      (selectedProtocol_ != VtxProtocol::Unknown && activeProtocol_ != selectedProtocol_)) return;
  ++status_.queries;
  if (activeProtocol_ == VtxProtocol::SmartAudio) {
    uint8_t frame[7] = {0,0xAA,0x55,SMARTAUDIO_CMD_GET_SETTINGS,0,0,0};
    frame[5] = sa_CRC8(frame + 1, 4);
    if (status_.transportProfile == 2) transmit(frame + 1, sizeof(frame) - 2);
    else if (status_.transportProfile == 1 || status_.transportProfile == 3) transmit(frame, sizeof(frame) - 1);
    else transmit(frame, sizeof(frame));
  } else {
    trampFrame_t frame;
    // Read capabilities then settings, including during probing so a valid
    // limits reply alone cannot make the UI claim current settings are known.
    const uint8_t command = (pollPhase_++ % 4 == 0) ? TRAMP_COMMAND_CMD_RF : TRAMP_COMMAND_GET_CONFIG;
    trampFrameInit(command, &frame);
    trampFrameClose(&frame);
    transmit(reinterpret_cast<uint8_t*>(&frame), sizeof(frame));
  }
}

void VtxManager::requestStatus() {
  if (!status_.probing && activeProtocol_ != VtxProtocol::Unknown) nextAction_ = millis();
}

void VtxManager::update() {
  receive();
  const uint32_t now = millis();
  status_.rxEdges = responseEdges_;
  if (waiting_ && !lineSampled_ && uint32_t(micros() - releasedAtMicros_) >= 20000) {
    lineSampled_ = true;
    status_.settledLevel = gpio_get_level(gpio_num_t(pin_));
    Serial.printf("VTX LINE_RX settled=%d output_enabled=%u edges_after_1ms=%lu\n",
                  status_.settledLevel, outputEnabled(pin_),
                  (unsigned long)responseEdges_);
  }
  if (waiting_) {
    if (gotResponse_) {
      waiting_ = false;
      attempt_ = 0;
      misses_ = 0;
      nextAction_ = now + kRecoveryMs;
      // A limits response has identified Tramp; the next query obtains state.
      if (status_.connected && !verifyPending_) nextAction_ = now + kPollingMs;
    } else if (due(now, deadline_)) {
      Serial.printf("VTX NO_REPLY level=%d edges_after_1ms=%lu\n",
                    gpio_get_level(gpio_num_t(pin_)), (unsigned long)responseEdges_);
      waiting_ = false;
      nextAction_ = now + kRecoveryMs;
      if (verifyPending_) {
        // Some VTX commands have no ACK. A missing SET acknowledgement is not a
        // failed status query; the mandatory GET below is the state authority.
      } else if (status_.probing) {
        if (++attempt_ >= kAttempts) {
          attempt_ = 0;
          if (status_.probeIndex + 1 >= status_.probeCount) {
            status_.probing = false;
            activeProtocol_ = VtxProtocol::Unknown;
            message("No validated VTX response. Hold B to retry.");
            return;
          }
          ++status_.probeIndex;
          pollPhase_ = 0;
          const Candidate* next = probeCandidate(status_.probeIndex);
          if (next) configure(*next);
        }
      } else if ((++misses_ >= 4 || (lastValid_ && now - lastValid_ > 8000)) &&
                 due(now, settingGraceUntil_)) {
        status_.connected = false;
        message("VTX reply lost; hold B to probe again");
      }
    } else return;
  }
  if (!due(now, nextAction_) || activeProtocol_ == VtxProtocol::Unknown) return;
  if (pendingLength_) {
    transmit(pending_, pendingLength_);
    pendingLength_ = 0;
    // SET acknowledgement isn't state confirmation. Give the VTX recovery time
    // then fetch full settings regardless of the ACK.
    verifyPending_ = true;
    settingGraceUntil_ = millis() + 5000;
    nextAction_ = now + kRecoveryMs;
  } else if (verifyPending_) {
    verifyPending_ = false;
    pollPhase_ = 1;  // Tramp 'v', not capability query.
    sendQuery();
  } else sendQuery();
}

void VtxManager::receive() {
  uint8_t fresh[96];
  size_t count = 0;
  while (uart_.available() && count < sizeof(fresh)) {
    const int value = uart_.read();
    if (value < 0) break;
    fresh[count++] = uint8_t(value);
  }
  if (!count) return;
  status_.rxBytes += count;
  hexLog("RX", fresh, count);
  for (size_t i = 0; i < count; ++i) {
    if (rxLength_ == sizeof(rx_)) {
      memmove(rx_, rx_ + 1, --rxLength_);
    }
    rx_[rxLength_++] = fresh[i];
  }
  parseRx();
}

void VtxManager::parseRx() {
  while (rxLength_) {
    size_t consumed = 1;
    if (activeProtocol_ == VtxProtocol::SmartAudio && rx_[0] == 0xAA) {
      if (rxLength_ < 4) return;
      if (rx_[1] != 0x55 || rx_[3] > 27) {
        memmove(rx_, rx_ + 1, --rxLength_);
        continue;
      }
      const size_t length = size_t(rx_[3]) + 5;
      bool valid = false;
      // Standard responses exclude AA55 from CRC. A few clones instead use
      // command-style CRC or include CRC in their advertised payload length.
      // Both variants must still pass a real CRC and semantic validation.
      if (rxLength_ >= length) {
        valid = (sa_CRC8(rx_ + 2, length - 3) == rx_[length - 1]) ||
                (sa_CRC8(rx_, length - 1) == rx_[length - 1]);
        if (valid) {
          consumed = length;
          parseSmartAudio(rx_, length, rx_[3]);
        }
      }
      if (!valid && rx_[3] > 0 && rxLength_ >= length - 1) {
        const size_t alternate = length - 1;
        valid = (sa_CRC8(rx_ + 2, alternate - 3) == rx_[alternate - 1]) ||
                (sa_CRC8(rx_, alternate - 1) == rx_[alternate - 1]);
        if (valid) {
          consumed = alternate;
          parseSmartAudio(rx_, alternate, rx_[3] - 1);
        }
      }
      if (!valid) {
        if (rxLength_ < length) return;
        ++status_.crcErrors;
      }
    } else if (activeProtocol_ == VtxProtocol::Tramp && rx_[0] == TRAMP_SYNC_START) {
      if (rxLength_ < TRAMP_FRAME_LENGTH) return;
      if (rx_[15] == TRAMP_SYNC_STOP && trampCrc(rx_) == rx_[14]) {
        consumed = TRAMP_FRAME_LENGTH;
        if (lastTxLength_ == consumed && memcmp(rx_, lastTx_, consumed) == 0)
          ++status_.echoFrames;
        else parseTramp(rx_);
      } else ++status_.crcErrors;
    }
    rxLength_ -= consumed;
    memmove(rx_, rx_ + consumed, rxLength_);
  }
}

void VtxManager::acceptedResponse(bool settings) {
  ++status_.validResponses;
  lastValid_ = millis();
  gotResponse_ = true;
  misses_ = 0;
  status_.protocol = activeProtocol_;
  if (settings) {
    settingGraceUntil_ = 0;
    const bool first = !status_.connected;
    status_.connected = true;
    status_.probing = false;
    snprintf(status_.stateText, sizeof(status_.stateText), "Settings read at %lu ms", (unsigned long)lastValid_);
    if (first) Serial.printf("VTX DETECTED %s baud=%lu inverted=%u\n", protocolName(activeProtocol_), (unsigned long)status_.baud, status_.inverted);
    Serial.printf("VTX STATUS protocol=%s version=%u freq=%u channel=%u power_mW=%u power_index=%d power_dBm=%d pit=%u mode=%u\n",
                  protocolName(activeProtocol_), status_.saVersion, status_.frequencyMHz,
                  status_.channelIndex, status_.powerMilliwatts, status_.powerIndex,
                  status_.powerDbm, status_.pitMode, status_.operationMode);
  }
}

bool VtxManager::parseSmartAudio(const uint8_t* frame, size_t length, size_t payloadLength) {
  if (activeProtocol_ != VtxProtocol::SmartAudio ||
      (selectedProtocol_ != VtxProtocol::Unknown && selectedProtocol_ != VtxProtocol::SmartAudio)) return false;
  // Requests echo with command CRC including AA55; compare their exact bytes.
  for (size_t i = 0; i + length <= lastTxLength_; ++i)
    if (memcmp(frame, lastTx_ + i, length) == 0) {
      ++status_.echoFrames;
      return false;
    }
  const uint8_t code = frame[2];
  if (verifyPending_ && code >= 2 && code <= 5) {
    // SET replies acknowledge reception but may reflect transient state during
    // PLL retuning. Only use a matching reply to begin the GET readback sooner.
    for (size_t i = 0; i + 4 <= lastTxLength_; ++i) {
      if (lastTx_[i] == 0xaa && lastTx_[i+1] == 0x55 &&
          code == (lastTx_[i+2] >> 1) && payloadLength >= (code == 4 ? 2 : 1)) {
        gotResponse_ = true;
        Serial.printf("VTX SET_ACK command=%u; full settings readback required\n", code);
        return true;
      }
    }
  }
  if (code != kSaSettingsV1 && code != kSaSettingsV2 && code != kSaSettingsV21) return false;
  if (payloadLength < 5) return false;
  const uint8_t* p = frame + 4;
  const uint16_t frequency = (uint16_t(p[3]) << 8) | p[4];
  if (frequency < 5000 || frequency > 6000 || p[0] >= 64) return false;
  status_.saVersion = code == kSaSettingsV1 ? 1 : (code == kSaSettingsV2 ? 2 : 21);
  status_.frequencyMHz = frequency;
  status_.channelIndex = p[0] < 40 ? p[0] : 255;
  status_.operationMode = p[2];
  status_.pitMode = (p[2] & 2) != 0;
  status_.rawPowerIndex = p[1];
  // A GET below the configured offset means the offset is wrong for this VTX, not
  // that the reply is: keep the VTX visible and report its lowest level.
  status_.powerIndex = status_.saVersion != 2 ? p[1] : p[1] >= sa2PowerOffset_ ? p[1] - sa2PowerOffset_ : 0;
  status_.powerValue = status_.powerIndex;
  status_.powerDbm = -1;
  status_.powerMilliwatts = 0;
  if (status_.saVersion == 2 && status_.powerIndex < 8) {
    // The protocol advertises no power table; retain the highest observed index
    // so lowering power does not hide the previously observed choices.
    if (sa2KnownCount_ < status_.powerIndex + 1) sa2KnownCount_ = status_.powerIndex + 1;
    status_.powerCount = sa2KnownCount_;
  }
  if (status_.saVersion == 21 && payloadLength >= 7) {
    status_.powerDbm = p[5];
    status_.powerValue = p[5];
    status_.powerMilliwatts = dbmToMw(p[5]);
    status_.powerCount = 0;
    const size_t reported = size_t(p[6]) + 1; // Many devices report N + a 0dBm pit entry.
    for (size_t i = 0; i < reported && 7 + i < payloadLength && status_.powerCount < 8; ++i) {
      const uint8_t dbm = p[7 + i];
      if (dbm == 0 || dbm >= 48) continue;
      status_.powerLevelsDbm[status_.powerCount++] = dbm;
    }
    status_.powerIndex = -1;
    for (uint8_t i = 0; i < status_.powerCount; ++i)
      if (status_.powerLevelsDbm[i] == p[5]) status_.powerIndex = i;
  }
  acceptedResponse(true);
  return true;
}

bool VtxManager::parseTramp(const uint8_t* frame) {
  if (activeProtocol_ != VtxProtocol::Tramp ||
      (selectedProtocol_ != VtxProtocol::Unknown && selectedProtocol_ != VtxProtocol::Tramp)) return false;
  if (frame[1] == TRAMP_COMMAND_CMD_RF) {
    const uint16_t low = le16(frame + 2), high = le16(frame + 4), power = le16(frame + 6);
    if (low < 5000 || high > 6000 || low >= high || !power || power > 20000) return false;
    status_.minFrequencyMHz = low;
    status_.maxFrequencyMHz = high;
    status_.maxPowerMilliwatts = power;
    acceptedResponse(false);
    return true;
  }
  if (frame[1] != TRAMP_COMMAND_GET_CONFIG) return false;
  const uint16_t frequency = le16(frame + 2), power = le16(frame + 4);
  if (frequency < 5000 || frequency > 6000 || power > 20000 || frame[7] > 1) return false;
  status_.frequencyMHz = frequency;
  status_.powerMilliwatts = power;
  status_.powerValue = power;
  status_.pitMode = frame[7] != 0;
  status_.raceLocked = (frame[6] & 1) != 0;
  status_.channelIndex = 255;  // Tramp has no channel numbers, only frequencies.
  acceptedResponse(true);
  return true;
}

bool VtxManager::canSet() const {
  return status_.connected && !status_.probing && !status_.raceLocked && !pendingLength_ && !verifyPending_ &&
         activeProtocol_ == status_.protocol &&
         (selectedProtocol_ == VtxProtocol::Unknown || status_.protocol == selectedProtocol_);
}

bool VtxManager::queueSmartAudio(uint8_t command, const uint8_t* payload, size_t length) {
  if (!canSet() || status_.protocol != VtxProtocol::SmartAudio || length > 20) return false;
  pending_[0] = 0;
  FrameHeader header;
  header.init(command, length);
  memcpy(pending_ + 1, &header, sizeof(header));
  if (length) memcpy(pending_ + 5, payload, length);
  pending_[5 + length] = sa_CRC8(pending_ + 1, 4 + length);
  pending_[6 + length] = 0;
  pendingLength_ = 7 + length;
  if (status_.transportProfile == 1 || status_.transportProfile == 3) --pendingLength_;
  else if (status_.transportProfile == 2) {
    pendingLength_ -= 2;
    memmove(pending_, pending_ + 1, pendingLength_);
  }
  nextAction_ = millis();
  message("Setting queued; waiting for readback");
  return true;
}

bool VtxManager::queueTramp(uint8_t command, uint16_t argument) {
  if (!canSet() || status_.protocol != VtxProtocol::Tramp) return false;
  trampFrame_t frame;
  trampFrameInit(command, &frame);
  frame.payload.buf[0] = argument & 0xff;
  frame.payload.buf[1] = argument >> 8;
  trampFrameClose(&frame);
  memcpy(pending_, &frame, sizeof(frame));
  pendingLength_ = sizeof(frame);
  nextAction_ = millis();
  message("Setting queued; waiting for readback");
  return true;
}

bool VtxManager::setFrequency(uint16_t megahertz) {
  if (megahertz < status_.minFrequencyMHz || megahertz > status_.maxFrequencyMHz) return false;
  if (status_.protocol == VtxProtocol::Tramp) return queueTramp(TRAMP_COMMAND_SET_FREQ, megahertz);
  const uint8_t payload[] = {uint8_t(megahertz >> 8),uint8_t(megahertz)};
  return queueSmartAudio(SMARTAUDIO_CMD_SET_FREQUENCY, payload, sizeof(payload));
}

bool VtxManager::setChannel(uint8_t index) {
  // SmartAudio's channel byte is band * 8 + channel over the bands the VTX has;
  // its settings reply refuses anything from 64 up. Tramp takes only frequencies.
  if (index >= 64 || status_.protocol != VtxProtocol::SmartAudio) return false;
  return queueSmartAudio(SMARTAUDIO_CMD_SET_CHANNEL, &index, 1);
}

bool VtxManager::setPowerValue(uint16_t value) {
  if (status_.protocol == VtxProtocol::Tramp) return setPower(value);
  if (status_.protocol != VtxProtocol::SmartAudio) return false;
  uint8_t byte = uint8_t(value);
  if (status_.saVersion == 21) {
    if (value >= 128) return false;
    byte = 0x80 | uint8_t(value);
  } else if (value > 255) return false;
  return queueSmartAudio(SMARTAUDIO_CMD_SET_POWER, &byte, 1);
}

bool VtxManager::setPower(uint16_t milliwatts) {
  if (status_.protocol == VtxProtocol::Tramp) {
    if (!milliwatts || !status_.maxPowerMilliwatts || milliwatts > status_.maxPowerMilliwatts) return false;
    return queueTramp(TRAMP_COMMAND_SET_POWER, milliwatts);
  }
  if (status_.saVersion != 21) return false;  // No assumed transmitter-specific power table.
  for (uint8_t i = 0; i < status_.powerCount; ++i) {
    const uint16_t mw = dbmToMw(status_.powerLevelsDbm[i]);
    // Common labels such as 25mW are rounded from 14dBm = 25.12mW.
    if (milliwatts == mw) return setPowerIndex(i);
  }
  return false;
}

bool VtxManager::setPowerIndex(uint8_t index) {
  uint8_t value = index;
  if (status_.protocol != VtxProtocol::SmartAudio) return false;
  if (status_.saVersion == 21) {
    if (index >= status_.powerCount) return false;
    value = 0x80 | status_.powerLevelsDbm[index];
  } else if (status_.saVersion == 2) {
    // v2 has no capability query. Only permit observed indices, plus 0 (minimum),
    // rather than inventing a model-specific list of mW values.
    if (index >= status_.powerCount) return false;
  } else return false;  // v1 uses manufacturer-specific DAC power codes.
  return queueSmartAudio(SMARTAUDIO_CMD_SET_POWER, &value, 1);
}

bool VtxManager::setKnownPowerCount(uint8_t count) {
  if (!count || count > 8) return false;
  sa2KnownCount_ = count;
  if (status_.protocol == VtxProtocol::SmartAudio && status_.saVersion == 2) {
    if (status_.powerIndex >= 0 && status_.powerIndex >= sa2KnownCount_) sa2KnownCount_ = status_.powerIndex + 1;
    status_.powerCount = sa2KnownCount_;
  }
  return true;
}

bool VtxManager::setSa2PowerOffset(uint8_t offset) {
  if (offset > 1 || pendingLength_ || verifyPending_) return false;
  if (offset == sa2PowerOffset_) return true;
  sa2PowerOffset_ = offset;
  sa2KnownCount_ = 0;
  if (status_.protocol == VtxProtocol::SmartAudio && status_.saVersion == 2) {
    status_.powerCount = 0;
    status_.powerIndex = -1;
    requestStatus();
  }
  return true;
}

bool VtxManager::setPit(bool enabled) {
  if (status_.protocol == VtxProtocol::Tramp) return queueTramp(TRAMP_COMMAND_ACTIVE_STATE, enabled ? 0 : 1);
  if (status_.saVersion == 21 && enabled) {
    const uint8_t zeroDbm = 0x80;
    return queueSmartAudio(SMARTAUDIO_CMD_SET_POWER, &zeroDbm, 1);
  }
  if (status_.saVersion >= 2 && !enabled) {
    const uint8_t mode = 4 | ((status_.operationMode & 16) ? 8 : 0);
    return queueSmartAudio(SMARTAUDIO_CMD_SET_MODE, &mode, 1);
  }
  return false;
}
