#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>

namespace Esp8BitVideo {
constexpr int width = 256;
constexpr int height = 240;
constexpr int samplesPerLine = 910;     // 227.5 color cycles
constexpr int linesPerFrame = 525;      // two interlaced fields
constexpr double sampleRate = 14318186.442057292;

enum class Standard : uint8_t { Ntsc, Pal };
constexpr int palHeight = 288;
constexpr int palSamplesPerLine = 1135; // 283.75 color cycles, 63.9996 us
constexpr int palLinesPerFrame = 625;   // two interlaced fields
constexpr double palSampleRate = 17734476.725260417;
// PAL only: rows whose middle columns come from prepared DAC samples, for detail
// finer than a column holds. The samples stay the caller's, in internal RAM.
constexpr int palBandFirstColumn = 60;
constexpr int palBandColumns = 135;
constexpr int palBandSamples = 486;
struct SampleBand {
  int first, last;                      // rows, last not included
  const uint16_t* samples;              // palBandSamples of them
};

bool begin(uint8_t* frontRgb332, uint8_t* backRgb332);
bool running();
uint32_t frames();
// Restarts the raster when the standard changes, keeping the current images. Stop
// any producer first and call on begin()'s core. PAL scans palHeight rows of each
// framebuffer, so both must be that large before selecting it. Up to two bands.
bool setStandard(Standard next, const SampleBand* bands = nullptr, int bandCount = 0);
Standard standard();
uint8_t* framebuffer();
uint8_t* backbuffer();
bool setBackPalette(const uint8_t* rgb888);
// One color of the writable frame's palette in signal terms, for what RGB cannot
// reach: y is 0 at black and 1 at white, u and v as .492(B-Y) and .877(R-Y). Set
// after setBackPalette, which overwrites it.
bool setBackPaletteSignal(uint8_t index, float y, float u, float v);
bool present();
bool presentPending();
uint32_t presentedFrames();

namespace Detail {
// DAC codes of the levels, from a staircase through the VTX and an FM receiver. Their
// ratios are the standard's; the size gives about the deviation that a real FPV camera
// has on another VTX. The output has a floor: codes up to about 12 all
// give the same level, and that level wanders by a fifth of the sync pulse over seconds,
// so sync stays just above it. Above it a code is about 0.46 phase units per sample:
// 40 IRE of sync is 19 codes and 100 IRE of picture 46, with 7.5 IRE of setup.
constexpr int syncCode = 14;
constexpr int blankCode = 33;
constexpr int blackCode = 36;
constexpr int whiteCode = 79;
// The subcarrier comes through the DAC and the VTX's input at about 0.61 of what the
// samples say: burst and chroma are raised to arrive at their nominal size.
constexpr int burstAmplitudeCode = 16;
constexpr float chromaBoost = 1.74f;
// Chroma also arrives about 8 degrees behind the burst, on U and V alike, in raw samples at
// the receiver: it is sent that far ahead. The cosine and sine of 8 degrees.
constexpr float chromaLeadCos = 0.990268f;
constexpr float chromaLeadSin = 0.139173f;

// ITU-R BT.470 system B/G, in samples from the sync edge. A half line is 567.5 samples:
// the second half starts on 568 and a whole line stays exactly 1135.
constexpr int palHalfLine = 568;
constexpr int palSyncSamples = 83;        // 4.68 us
constexpr int palEqualizingSamples = 42;  // 2.37 us
constexpr int palBroadSamples = 484;      // 27.29 us, then a 4.7 us serration
constexpr int palBurstStart = 99;         // 5.58 us
constexpr int palBurstEnd = 139;          // 10 cycles
constexpr int palActiveStart = 186;       // 10.49 us
constexpr int palActiveSamples = 922;     // 51.99 us, leaving a 1.52 us front porch
// Five columns take 18 samples, so that 256 of them fill the picture.
constexpr int palColumnGroups = 51;
constexpr int palField1First = 23;
constexpr int palField2First = 336;
// 300 mV of sync stays 19 codes, which makes 700 mV of picture 44. No setup, so black
// is blanking. The burst is as large as NTSC's, on axes 45 degrees from the samples.
constexpr int palWhiteCode = blankCode + 44;
constexpr int palBurstCode = 11;
constexpr int palBurstEdgeCode = 6;
static_assert(palColumnGroups * 18 + 4 == palActiveSamples, "PAL columns must fill the picture");
static_assert(palBandColumns / 5 * 18 == palBandSamples && palBandFirstColumn % 5 == 0 && palBandColumns % 5 == 0,
              "A band must cover whole groups of columns");

// Where column x starts within the picture's samples.
constexpr int palColumnStart(int x) {
  return x / 5 * 18 + x % 5 * 4 - (x % 5 >= 3 ? 1 : 0);
}

// Owned by a framebuffer bank, never modified while that bank is scanned out.
// Foreground code prepares its four-phase DAC table; the ISR only reads wave.
class PaletteBank {
 public:
  uint16_t wave[256][8] = {};           // each color's four samples, twice

  // True means rebuilt, false means the same palette was already cached.
  bool assign(const uint8_t* rgb888) {
    if (valid_ && ((!rgb888 && !indexed_) ||
                   (rgb888 && indexed_ && !memcmp(rgb_, rgb888, sizeof(rgb_))))) return false;
    if (rgb888) memcpy(rgb_, rgb888, sizeof(rgb_));
    indexed_ = rgb888 != nullptr;
    build();
    valid_ = true;
    return true;
  }

  // Only while this bank is not scanned out. The next assign() rebuilds the table.
  void setSignal(int color, float y, float u, float v) {
    encode(color, y, u, v);
    valid_ = false;
  }

  // Only while the raster is stopped. Keeps the assigned colors, not setSignal's.
  void setPal(bool pal) {
    if (pal == pal_) return;
    pal_ = pal;
    build();
  }

 private:
  void build() {
    for (int color = 0; color < 256; ++color) {
      // Preserve the legacy v1 waveform exactly instead of rounding its RGB
      // levels through an intermediate RGB888 representation.
      const float red = indexed_ ? rgb_[color * 3] / 255.0f : ((color >> 5) & 7) / 7.0f;
      const float green = indexed_ ? rgb_[color * 3 + 1] / 255.0f : ((color >> 2) & 7) / 7.0f;
      const float blue = indexed_ ? rgb_[color * 3 + 2] / 255.0f : (color & 3) / 3.0f;
      const float y = .299f * red + .587f * green + .114f * blue;
      encode(color, y, .492f * (blue - y), .877f * (red - y));
    }
  }

  void encode(int color, float y, float nominalU, float nominalV) {
    // PAL's V switch averages a chroma lag away, so it is sent without the lead.
    const float u = pal_ ? nominalU : chromaLeadCos * nominalU - chromaLeadSin * nominalV;
    const float v = pal_ ? nominalV : chromaLeadSin * nominalU + chromaLeadCos * nominalV;
    const int black = pal_ ? blankCode : blackCode;
    const int white = pal_ ? palWhiteCode : whiteCode;
    // SMPTE 170M with a negative-cosine burst: U*cos(t)-V*sin(t). A PAL line with V
    // inverted reads these four samples backwards, which gives U*cos(t)+V*sin(t).
    const float phase[4] = {u, -v, -u, v};
    for (int sample = 0; sample < 4; ++sample) {
      const int code = lroundf(black + (white - black) * (y + chromaBoost * phase[sample]));
      wave[color][sample + 4] = wave[color][sample] = static_cast<uint16_t>((code < 0 ? 0 : (code > 255 ? 255 : code)) << 8);
    }
  }

  uint8_t rgb_[256 * 3] = {};
  bool valid_ = false;
  bool indexed_ = false;
  bool pal_ = false;
};

// ESP32's 16-bit mono FIFO sends the neighboring halfword first.
inline __attribute__((always_inline)) void sample(uint16_t* dst, int index, uint16_t value) {
  dst[index ^ 1] = value;
}

// Two equal samples per store keep ISR work comfortably below a scanline.
inline __attribute__((always_inline)) void fill(uint16_t* dst, int from, int to, uint16_t value) {
  if (from & 1) sample(dst, from++, value);
  // These stores land in uint16_t samples: without may_alias the optimizer can move them.
  typedef uint32_t __attribute__((may_alias)) SamplePair;
  SamplePair* pairs = reinterpret_cast<SamplePair*>(dst);
  const uint32_t pair = (static_cast<uint32_t>(value) << 16) | value;
  for (int i = from / 2; i < to / 2; ++i) pairs[i] = pair;
  if (to & 1) sample(dst, to - 1, value);
}

// Half a line of the vertical interval.
enum class Half : uint8_t { none, equalizing, broad };

// Five equalizing, five broad and five equalizing pulses per field. Lines 623 and 318
// are the two that are only half of the interval; 623 keeps its picture, see below.
inline __attribute__((always_inline)) bool palVerticalLine(int line, Half& first, Half& second) {
  if (line <= 5) {
    first = line <= 3 ? Half::broad : Half::equalizing;
    second = line <= 2 ? Half::broad : Half::equalizing;
    return true;
  }
  if (line >= 624) {
    first = second = Half::equalizing;
    return true;
  }
  if (line < 311 || line > 318) return false;
  first = line == 314 || line == 315 ? Half::broad : Half::equalizing;
  second = line >= 313 && line <= 315 ? Half::broad : line == 318 ? Half::none : Half::equalizing;
  return true;
}

// Bruch blanking: nine lines without burst around each field's interval, moved by a
// line every other field, so that every field's bursts begin and end on the same swing.
inline __attribute__((always_inline)) bool palBurstLine(int line, bool secondFrame) {
  if (secondFrame) return line > 5 && line < 623 && (line < 311 || line > 319);
  return line > 6 && line < 622 && (line < 310 || line > 318);
}

inline __attribute__((always_inline)) void palHalf(uint16_t* dst, int from, int to, Half half, int lead) {
  const int low = half == Half::broad ? palBroadSamples : half == Half::equalizing ? palEqualizingSamples : 0;
  if (low) fill(dst, from ? from - lead : 0, from + low - lead, static_cast<uint16_t>(syncCode << 8));
  fill(dst, from + low - lead, to - lead, static_cast<uint16_t>(blankCode << 8));
}

// Five columns as plain loads and stores. A sample at a time, with its phase worked out
// each time, took 65 us of the 64 a line has. A color's table holds its samples twice, so
// a column reads four neighbors from where the line's phase says: upwards, or downwards
// with V inverted. Odd is the store's alignment, as sample() would have it.
template <bool Inverted, int Odd>
inline __attribute__((always_inline)) void palGroup(uint16_t* d, const uint8_t* s, const uint16_t* first, const uint16_t* second) {
  constexpr int k0 = Inverted ? 3 : 0, k1 = Inverted ? 2 : 1, k2 = Inverted ? 1 : 2, k3 = Inverted ? 0 : 3;
  const uint16_t* w = first + (s[0] << 3);
  d[(0 + Odd) ^ 1] = w[k0]; d[(1 + Odd) ^ 1] = w[k1]; d[(2 + Odd) ^ 1] = w[k2]; d[(3 + Odd) ^ 1] = w[k3];
  w = first + (s[1] << 3);
  d[(4 + Odd) ^ 1] = w[k0]; d[(5 + Odd) ^ 1] = w[k1]; d[(6 + Odd) ^ 1] = w[k2]; d[(7 + Odd) ^ 1] = w[k3];
  w = first + (s[2] << 3);
  d[(8 + Odd) ^ 1] = w[k0]; d[(9 + Odd) ^ 1] = w[k1]; d[(10 + Odd) ^ 1] = w[k2];
  w = second + (s[3] << 3);
  d[(11 + Odd) ^ 1] = w[k0]; d[(12 + Odd) ^ 1] = w[k1]; d[(13 + Odd) ^ 1] = w[k2]; d[(14 + Odd) ^ 1] = w[k3];
  w = second + (s[4] << 3);
  d[(15 + Odd) ^ 1] = w[k0]; d[(16 + Odd) ^ 1] = w[k1]; d[(17 + Odd) ^ 1] = w[k2];
}

// All of a picture line. phase is the subcarrier's at its first sample, already reversed
// if V is inverted. Groups turn the phase by two samples, and their fourth column by three.
template <bool Inverted, int Odd>
inline __attribute__((always_inline)) void palPicture(uint16_t* dst, int at, uint32_t phase, const uint8_t* s,
                                                      const uint16_t (*palette)[8], const uint16_t* direct) {
  const uint16_t* table = palette[0];
  const auto start = [table](uint32_t p) { return table + (p & 3) + (Inverted ? 1 : 0); };
  const uint32_t turn = Inverted ? 1 : 3;
  const uint16_t* first = start(phase);
  const uint16_t* second = start(phase + turn);
  const uint16_t* third = start(phase + 2);
  const uint16_t* fourth = start(phase + 2 + turn);
  uint16_t* d = dst + at - Odd;
  for (int group = 0; group < palColumnGroups; ++group) {
    if (direct && group == palBandFirstColumn / 5) {
      for (int i = 0; i < palBandSamples; ++i) d[(i + Odd) ^ 1] = direct[i];
      d += palBandSamples;
      s += palBandColumns;
      group += palBandColumns / 5;
    }
    palGroup<Inverted, Odd>(d, s, group & 1 ? third : first, group & 1 ? fourth : second);
    d += 18;
    s += 5;
  }
  constexpr int k0 = Inverted ? 3 : 0, k1 = Inverted ? 2 : 1, k2 = Inverted ? 1 : 2, k3 = Inverted ? 0 : 3;
  const uint16_t* w = third + (s[0] << 3);
  d[(0 + Odd) ^ 1] = w[k0]; d[(1 + Odd) ^ 1] = w[k1]; d[(2 + Odd) ^ 1] = w[k2]; d[(3 + Odd) ^ 1] = w[k3];
}

// One line, into a DMA buffer. 1135 samples are not whole 32-bit words, so buffers
// alternate between 1136 samples, ending in the next line's first, and 1134, starting
// on this line's second (lead 1). Each line starts low, so that sample is always sync.
// phase is the subcarrier's sample phase at the line's start.
inline __attribute__((always_inline)) void renderPalLine(uint16_t* dst, int line, bool secondFrame, uint32_t phase, int lead,
                                                         const uint8_t* pixels, const uint16_t (*palette)[8],
                                                         const SampleBand* bands) {
  const uint16_t syncValue = syncCode << 8;
  const uint16_t blankValue = blankCode << 8;
  Half first, second;
  if (palVerticalLine(line, first, second)) {
    palHalf(dst, 0, palHalfLine, first, lead);
    palHalf(dst, palHalfLine, palSamplesPerLine, second, lead);
  } else {
    // The first and last burst of a field keep V as it is.
    const bool inverted = !((line + (secondFrame ? 1 : 0)) & 1);
    fill(dst, 0, palSyncSamples - lead, syncValue);
    fill(dst, palSyncSamples - lead, palBurstStart - lead, blankValue);
    if (palBurstLine(line, secondFrame)) {
      // -U plus or minus V: samples -a,-a,+a,+a, or -a,+a,+a,-a with V inverted.
      uint16_t full[4], edge[4];
      for (int p = 0; p < 4; ++p) {
        const bool high = inverted ? p == 1 || p == 2 : p >= 2;
        full[p] = static_cast<uint16_t>((blankCode + (high ? palBurstCode : -palBurstCode)) << 8);
        edge[p] = static_cast<uint16_t>((blankCode + (high ? palBurstEdgeCode : -palBurstEdgeCode)) << 8);
      }
      for (int i = palBurstStart; i < palBurstEnd; ++i) {
        const bool atEdge = i < palBurstStart + 4 || i >= palBurstEnd - 4;
        sample(dst, i - lead, (atEdge ? edge : full)[(i + phase) & 3]);
      }
    } else {
      fill(dst, palBurstStart - lead, palBurstEnd - lead, blankValue);
    }
    const int row = line >= palField2First ? line - palField2First : line - palField1First;
    if (row < 0 || row >= palHeight) {
      fill(dst, palBurstEnd - lead, palSamplesPerLine - lead, blankValue);
    } else {
      fill(dst, palBurstEnd - lead, palActiveStart - lead, blankValue);
      fill(dst, palActiveStart + palActiveSamples - lead, palSamplesPerLine - lead, blankValue);
      const uint32_t p = palActiveStart + phase;
      const int at = palActiveStart - lead;
      const uint8_t* src = pixels + row * width;
      const uint16_t* direct = nullptr;
      if (row >= bands[0].first && row < bands[0].last) direct = bands[0].samples;
      else if (row >= bands[1].first && row < bands[1].last) direct = bands[1].samples;
      if (inverted) {
        if (lead) palPicture<true, 1>(dst, at, 0 - p, src, palette, direct);
        else palPicture<true, 0>(dst, at, 0 - p, src, palette, direct);
      } else {
        if (lead) palPicture<false, 1>(dst, at, p, src, palette, direct);
        else palPicture<false, 0>(dst, at, p, src, palette, direct);
      }
      // A half line at each end of the picture: the first starts mid line, and the
      // last goes over to the next interval's first pulse there.
      if (line == palField1First) fill(dst, palActiveStart - lead, palHalfLine - lead, blankValue);
      else if (line == palField2First + palHeight - 1) palHalf(dst, palHalfLine, palSamplesPerLine, Half::equalizing, lead);
    }
  }
  if (!lead) sample(dst, palSamplesPerLine, syncValue);
}

// A small platform-independent ownership state machine. Production callers
// protect every access with the ESP32 cross-core portMUX; methods are inlined
// into the IRAM ISR. Pixel copying and allocation never happen under its lock.
class FrameExchange {
 public:
  inline __attribute__((always_inline)) void reset(uint8_t* front, uint8_t* back) {
    front_ = front;
    back_ = back;
    pending_ = false;
    presented_ = 0;
    frontBank_ = 0;
  }
  inline __attribute__((always_inline)) uint8_t* front() const { return front_; }
  inline __attribute__((always_inline)) uint8_t* writable() const { return pending_ ? nullptr : back_; }
  inline __attribute__((always_inline)) bool pending() const { return pending_; }
  inline __attribute__((always_inline)) uint32_t presented() const { return presented_; }
  inline __attribute__((always_inline)) uint8_t frontBank() const { return frontBank_; }
  inline __attribute__((always_inline)) int writableBank() const {
    return pending_ || !front_ || !back_ ? -1 : (frontBank_ ^ 1);
  }
  inline __attribute__((always_inline)) bool queue() {
    if (pending_ || !front_ || !back_) return false;
    pending_ = true;
    return true;
  }
  inline __attribute__((always_inline)) bool onLineRendered(int line) {
    // DMA prefetches two scanlines. At line 240 every old active row is already
    // copied into DMA, so the old framebuffer can be reused immediately.
    if (line != height || !pending_) return false;
    uint8_t* previous = front_;
    front_ = back_;
    back_ = previous;
    frontBank_ ^= 1;
    ++presented_;
    pending_ = false;
    return true;
  }

 private:
  uint8_t* front_ = nullptr;
  uint8_t* back_ = nullptr;
  bool pending_ = false;
  uint8_t frontBank_ = 0;
  uint32_t presented_ = 0;
};
}
}
