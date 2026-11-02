#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>

enum class VtxProtocol : uint8_t { Unknown, SmartAudio, Tramp };

struct VtxStatus {
  VtxProtocol protocol = VtxProtocol::Unknown;
  bool connected = false;
  bool probing = false;
  uint16_t frequencyMHz = 0;
  uint16_t powerMilliwatts = 0;  // 0 means not reported/unknown, not RF off.
  int16_t powerIndex = -1;
  int16_t rawPowerIndex = -1;  // Exact GET_SETTINGS byte before SA2 normalization.
  // The power as a vtxtable value: dBm on SmartAudio v2.1, the index on v2 (after
  // the offset) and v1, milliwatts on Tramp. -1 until read.
  int32_t powerValue = -1;
  uint8_t channelIndex = 255;  // SmartAudio's band * 8 + channel; 255 unknown, and always on Tramp.
  int16_t powerDbm = -1;
  bool pitMode = false;
  bool raceLocked = false;
  uint8_t saVersion = 0;  // 1, 2, or 21
  uint8_t operationMode = 0;
  uint32_t baud = 0;
  bool inverted = false;
  uint8_t transportProfile = 0;
  uint8_t stopBits = 2;
  int8_t linePullUp = -1;
  int8_t linePullDown = -1;
  int8_t lineFloating = -1;
  uint32_t rxEdges = 0;  // Post-transmit edges, excludes first 1ms of settling.
  int8_t settledLevel = -1;
  uint32_t queries = 0;
  uint32_t rxBytes = 0;
  uint32_t validResponses = 0;
  uint32_t crcErrors = 0;
  uint32_t echoFrames = 0;
  uint8_t probeIndex = 0;
  uint8_t probeCount = 0;
  uint8_t powerCount = 0;
  uint8_t powerLevelsDbm[8] = {};
  uint16_t minFrequencyMHz = 5000;
  uint16_t maxFrequencyMHz = 6000;
  uint16_t maxPowerMilliwatts = 0;
  char stateText[64] = "Not started";
};

// ESP32 half duplex UART adapter for igorka48/VTXControl_ESP32 wire codecs.
// Call update frequently. Discovery only sends read commands. Setters queue a
// command; success means queued, and only later GET replies update status().
class VtxManager {
 public:
  void begin(int pin = 17);
  // GPIO16/17 are the supported Basic control pins; invalid values leave the
  // active wire untouched. Switching releases the old wire before probing.
  bool setPin(int pin);
  int pin() const { return pin_; }
  // Unknown selects automatic discovery. A forced family probes only its own
  // transport candidates; selection survives pin changes and reprobes.
  bool setProtocol(VtxProtocol protocol);
  VtxProtocol selectedProtocol() const { return selectedProtocol_; }
  // True once begin() has configured the wire; before that nothing is sent.
  bool started() const { return uartStarted_; }
  // Whether a setter would be accepted now: connected, and no command on its way
  // out or awaiting its readback.
  bool canSet() const;
  void update();
  void reprobe();
  void requestStatus();
  const VtxStatus& status() const { return status_; }
  bool setFrequency(uint16_t megahertz);
  // SmartAudio only: band * 8 + channel, as the VTX numbers its own bands.
  bool setChannel(uint8_t index);
  bool setPower(uint16_t milliwatts);
  bool setPowerIndex(uint8_t index);
  // A vtxtable power value: dBm on SmartAudio v2.1, the raw byte on v2 and v1,
  // milliwatts on Tramp.
  bool setPowerValue(uint16_t value);
  // How many SA2 power indices to offer (1..8): the specification's four, or what
  // the caller has observed and persisted. An index read back above it raises it.
  // It describes indices only, never guessed RF watts.
  bool setKnownPowerCount(uint8_t count);
  // Some SA2 clones report one-based GET power while SET remains zero-based.
  // Configure only for a verified device; changing it discards cached ranges.
  bool setSa2PowerOffset(uint8_t offset);
  bool setPit(bool enabled);
  static const char* protocolName(VtxProtocol protocol);

 private:
  // Profiles: 0 SA pulldown/dummy+suffix, 1 float/dummy, 2 pullup/raw,
  // 3 pulldown/8N1/dummy, 4 float/dummy+suffix; Tramp profile1 floats RX.
  struct Candidate {
    VtxProtocol protocol;
    uint32_t baud;
    bool inverted;
    uint8_t profile;
    Candidate(VtxProtocol protocolValue, uint32_t baudValue, bool invertedValue, uint8_t profileValue = 0)
        : protocol(protocolValue), baud(baudValue), inverted(invertedValue), profile(profileValue) {}
  };
  static const Candidate candidates_[];
  HardwareSerial uart_{2};
  VtxStatus status_;
  VtxProtocol selectedProtocol_ = VtxProtocol::Unknown;
  int pin_ = 17;
  bool uartStarted_ = false;
  bool edgeInterruptAttached_ = false;
  uint8_t rx_[128] = {};
  size_t rxLength_ = 0;
  uint8_t lastTx_[32] = {};
  size_t lastTxLength_ = 0;
  uint8_t pending_[32] = {};
  size_t pendingLength_ = 0;
  uint8_t attempt_ = 0;
  uint8_t pollPhase_ = 0;
  uint8_t misses_ = 0;
  uint8_t sa2KnownCount_ = 0;
  uint8_t sa2PowerOffset_ = 0;
  bool waiting_ = false;
  bool gotResponse_ = false;
  bool verifyPending_ = false;
  uint32_t deadline_ = 0;
  uint32_t nextAction_ = 0;
  uint32_t lastValid_ = 0;
  uint32_t settingGraceUntil_ = 0;
  volatile uint32_t responseEdges_ = 0;
  volatile uint32_t releasedAtMicros_ = 0;
  bool lineSampled_ = false;
  static void IRAM_ATTR onRxEdge(void* argument);
  VtxProtocol activeProtocol_ = VtxProtocol::Unknown;
  uint8_t probeCandidateCount() const;
  const Candidate* probeCandidate(uint8_t index) const;
  void configure(const Candidate& candidate);
  void diagnoseLine();
  void detachEdgeInterrupt();
  void releaseLine();
  void transmit(const uint8_t* bytes, size_t length);
  void sendQuery();
  void receive();
  void parseRx();
  bool parseSmartAudio(const uint8_t* frame, size_t length, size_t payloadLength);
  bool parseTramp(const uint8_t* frame);
  void acceptedResponse(bool settings);
  bool queueSmartAudio(uint8_t command, const uint8_t* payload, size_t length);
  bool queueTramp(uint8_t command, uint16_t argument);
  void message(const char* text);
};
