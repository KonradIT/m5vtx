#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// M5V: a little-endian header followed by top-row-first 256x240 byte frames.
// v1 uses RGB332 bytes; v2 uses indices into its fixed RGB888 palette.
namespace SdVideoFormat {
constexpr size_t headerBytes = 512;
constexpr uint16_t version = 1;
constexpr size_t indexedHeaderBytes = 1024;
constexpr uint16_t indexedVersion = 2;
constexpr size_t maxHeaderBytes = indexedHeaderBytes;
constexpr size_t prefixBytes = 16;
constexpr size_t paletteBytes = 256 * 3;
constexpr size_t paletteOffset = 96;
constexpr uint16_t width = 256;
constexpr uint16_t height = 240;
constexpr uint32_t frameBytes = uint32_t(width) * height;
constexpr size_t titleBytes = 64;
constexpr uint16_t maxFps = 30;

struct Header {
  uint16_t version = 0;
  uint16_t headerSize = 0;
  bool isIndexed = false;
  uint16_t fpsNum = 0;
  uint16_t fpsDen = 0;
  uint32_t frameCount = 0;
  char title[titleBytes + 1] = {};
  uint32_t paletteCrc = 0;
  uint8_t palette[paletteBytes] = {};
};

enum class Error : uint8_t {
  None, ShortHeader, Magic, Version, HeaderSize, Dimensions, FrameRate,
  FrameCount, FrameSize, Flags, Title, Reserved, Checksum, FileSize
};

inline uint16_t read16(const uint8_t* p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t read32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint32_t crc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffu;
  for (size_t n = 0; n < length; ++n) {
    crc ^= bytes[n];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & uint32_t(-int32_t(crc & 1)));
  }
  return crc ^ 0xffffffffu;
}
inline uint64_t fileBytes(uint32_t frames, uint16_t headerSize) {
  return uint64_t(headerSize) + uint64_t(frames) * frameBytes;
}
inline uint64_t fileBytes(uint32_t frames) {
  return fileBytes(frames, headerBytes);
}

// Integer rational timing avoids long-run drift for rates such as 30000/1001.
struct FrameClock {
  uint64_t dueUs = 0;
  uint32_t periodUs = 0;
  uint32_t remainder = 0;
  uint32_t accumulator = 0;
  uint16_t divisor = 1;
  void start(uint64_t nowUs, uint16_t numerator, uint16_t denominator) {
    dueUs = nowUs;
    const uint64_t duration = uint64_t(1000000) * denominator;
    periodUs = uint32_t(duration / numerator);
    remainder = uint32_t(duration % numerator);
    divisor = numerator;
    accumulator = 0;
  }
  void advance() {
    dueUs += periodUs;
    accumulator += remainder;
    if (accumulator >= divisor) { ++dueUs; accumulator -= divisor; }
  }
};

inline bool validUtf8(const uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length;) {
    const uint8_t first = bytes[i++];
    if (first < 0x80) { if (first < 0x20 || first == 0x7f) return false; continue; }
    uint32_t value;
    uint8_t continuation;
    uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) { value = first & 0x1f; continuation = 1; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { value = first & 0x0f; continuation = 2; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { value = first & 7; continuation = 3; minimum = 0x10000; }
    else return false;
    if (i + continuation > length) return false;
    while (continuation--) {
      const uint8_t next = bytes[i++];
      if ((next & 0xc0) != 0x80) return false;
      value = (value << 6) | (next & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
  }
  return true;
}

// Inspect only a fixed prefix before choosing how much header data to read.
// Never trust an arbitrary on-card length as an allocation/read size.
inline Error inspectPrefix(const uint8_t* bytes, size_t length, uint16_t& headerSize) {
  headerSize = 0;
  if (!bytes || length < prefixBytes) return Error::ShortHeader;
  const bool rgb332 = !memcmp(bytes, "M5RGB332", 8);
  const bool indexed = !memcmp(bytes, "M5PAL256", 8);
  if (!rgb332 && !indexed) return Error::Magic;
  if (read16(bytes + 8) != (indexed ? indexedVersion : version)) return Error::Version;
  const uint16_t expected = indexed ? indexedHeaderBytes : headerBytes;
  if (read16(bytes + 10) != expected) return Error::HeaderSize;
  headerSize = expected;
  return Error::None;
}

inline Error decode(const uint8_t* bytes, size_t length, uint64_t actualFileBytes, Header& output) {
  output = Header{};
  uint16_t headerSize;
  const auto prefixError = inspectPrefix(bytes, length, headerSize);
  if (prefixError != Error::None) return prefixError;
  if (length < headerSize) return Error::ShortHeader;
  const bool indexed = headerSize == indexedHeaderBytes;
  if (read16(bytes + 12) != width || read16(bytes + 14) != height) return Error::Dimensions;
  const uint16_t numerator = read16(bytes + 16), denominator = read16(bytes + 18);
  if (!denominator || numerator < denominator || uint32_t(numerator) > uint32_t(maxFps) * denominator)
    return Error::FrameRate;
  const uint32_t frames = read32(bytes + 20);
  if (!frames) return Error::FrameCount;
  if (read32(bytes + 24) != frameBytes) return Error::FrameSize;
  if (read32(bytes + 28)) return Error::Flags;
  size_t titleLength = 0;
  while (titleLength < titleBytes && bytes[32 + titleLength]) ++titleLength;
  if (titleLength == titleBytes || !validUtf8(bytes + 32, titleLength)) return Error::Title;
  for (size_t i = titleLength + 1; i < titleBytes; ++i)
    if (bytes[32 + i]) return Error::Title;
  const size_t checksumOffset = headerSize - 4;
  const size_t reservedStart = indexed ? paletteOffset + paletteBytes : 96;
  for (size_t i = reservedStart; i < checksumOffset; ++i) if (bytes[i]) return Error::Reserved;
  if (crc32(bytes, checksumOffset) != read32(bytes + checksumOffset)) return Error::Checksum;
  if (actualFileBytes != fileBytes(frames, headerSize)) return Error::FileSize;
  output.version = indexed ? indexedVersion : version;
  output.headerSize = headerSize;
  output.isIndexed = indexed;
  output.fpsNum = numerator;
  output.fpsDen = denominator;
  output.frameCount = frames;
  memcpy(output.title, bytes + 32, titleLength);
  if (indexed) {
    memcpy(output.palette, bytes + paletteOffset, paletteBytes);
    output.paletteCrc = crc32(output.palette, paletteBytes);
  }
  return Error::None;
}

inline const char* errorMessage(Error error) {
  switch (error) {
    case Error::None: return "OK";
    case Error::ShortHeader: return "Truncated header";
    case Error::Magic: return "Wrong file magic";
    case Error::Version: return "Unsupported version";
    case Error::HeaderSize: return "Unsupported header size";
    case Error::Dimensions: return "Expected 256x240";
    case Error::FrameRate: return "Expected 1..30 fps";
    case Error::FrameCount: return "Empty video";
    case Error::FrameSize: return "Expected 61440-byte frames";
    case Error::Flags: return "Unsupported flags";
    case Error::Title: return "Invalid video title";
    case Error::Reserved: return "Nonzero reserved header bytes";
    case Error::Checksum: return "Header CRC mismatch";
    case Error::FileSize: return "Payload size mismatch";
    default: return "Invalid video";
  }
}
}
