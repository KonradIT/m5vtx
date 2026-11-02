/* Copyright (c) 2020, Peter Barrett
 * See ../LICENSE. Adapted from rossumur/esp_8_bit video_out.h.
 */
#include "esp8bit_video.h"

#include <Arduino.h>
#include <math.h>
#include "driver/dac.h"
#include "driver/i2s.h"
#include "driver/periph_ctrl.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "rom/lldesc.h"
#include "soc/i2s_struct.h"
#include "soc/rtc.h"
#include "soc/soc_memory_types.h"

#if !CONFIG_IDF_TARGET_ESP32
#error "esp8bit_video requires the original ESP32's internal DAC"
#endif

namespace Esp8BitVideo {
namespace {
// SMPTE 170M at four samples per color cycle. Half a line is 455 samples, so the
// half-line pulses of the vertical interval land on samples exactly.
constexpr int halfLine = samplesPerLine / 2;
constexpr int syncSamples = 67;         // 4.68 us
constexpr int equalizingSamples = 34;   // 2.37 us
constexpr int broadSamples = 387;       // 27.03 us, then a 4.75 us serration
constexpr int burstStart = 76;          // 19 cycles after the sync edge
constexpr int burstEnd = 112;           // 9 cycles
constexpr int activeStart = 130;        // 9.08 us, as on the camera this was compared with
constexpr int activePixels = 253;       // up to 62.09 us, leaving a 1.47 us front porch
constexpr int firstPixel = 1;           // the framebuffer's other 3 columns stay in the overscan
// Picture lines. 240 of them per field, on the lines a camera uses.
constexpr int field1First = 23;
constexpr int field2First = 286;
constexpr uint16_t sync = Detail::syncCode << 8;
constexpr uint16_t blank = Detail::blankCode << 8;
constexpr int burstAmplitude = Detail::burstAmplitudeCode;
using Detail::fill;
using Detail::Half;
using Detail::sample;

// The ISR fills a line as the DMA finishes one, so it must keep up within the
// lines in hand. With two, a timer tick was enough to miss one, and the DMA sent
// a stale line again: 17 extra lines a field, at the rate of the 1 kHz tick.
constexpr int dmaLines = 4;
DRAM_ATTR lldesc_t dma[dmaLines] = {};
DRAM_ATTR int nextBuffer = 0;           // the next one the DMA finishes
DRAM_ATTR uint32_t burstPairs[2][(burstEnd - burstStart) / 2];
DRAM_ATTR Detail::PaletteBank palettes[2];
DRAM_ATTR const uint16_t (*activePalette)[8] = nullptr;
DRAM_ATTR uint8_t* pixels = nullptr;
DRAM_ATTR Detail::FrameExchange exchange;
DRAM_ATTR portMUX_TYPE exchangeLock = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR volatile uint32_t frameCount = 0;
DRAM_ATTR volatile int nextLine = 1;    // numbered as in SMPTE 170M, 1..525
DRAM_ATTR uint32_t linePhase = 0;       // the subcarrier's sample phase at the start of the next line
DRAM_ATTR bool palRaster = false;
DRAM_ATTR bool secondFrame = false;     // PAL's V switch and burst blanking span two frames
DRAM_ATTR SampleBand palBands[2] = {};
intr_handle_t interruptHandle = nullptr;
bool started = false;

// Half a line of the vertical interval, starting at sample from.
inline void IRAM_ATTR renderHalf(uint16_t* dst, int from, Half half) {
    const int low = half == Half::broad ? broadSamples : half == Half::equalizing ? equalizingSamples : 0;
    fill(dst, from, from + low, sync);
    fill(dst, from + low, from + halfLine, blank);
}

// Line numbers of SMPTE 170M: lines 1..9 are field 1's vertical interval, and
// field 2's is the same half a line later, from the middle of line 263 to the
// middle of line 272.
inline bool IRAM_ATTR verticalLine(int line, Half& first, Half& second) {
    if (line <= 9) {
        first = second = line >= 4 && line <= 6 ? Half::broad : Half::equalizing;
        return true;
    }
    if (line < 264 || line > 272) return false;
    first = line <= 266 || line >= 270 ? Half::equalizing : Half::broad;
    second = line <= 265 || line >= 269 ? Half::equalizing : Half::broad;
    if (line == 272) second = Half::none;
    return true;
}

void IRAM_ATTR renderLine(uint16_t* dst, int line, uint32_t phase) {
    Half first, second;
    if (verticalLine(line, first, second)) {
        renderHalf(dst, 0, first);
        renderHalf(dst, halfLine, second);
        return;
    }
    fill(dst, 0, syncSamples, sync);
    fill(dst, syncSamples, burstStart, blank);

    const uint32_t* burst = burstPairs[phase >> 1];
    uint32_t* pairs = reinterpret_cast<uint32_t*>(dst) + burstStart / 2;
    for (int i = 0; i < (burstEnd - burstStart) / 2; ++i) pairs[i] = burst[i];

    if (line == 263) {                  // at its middle the next vertical interval starts
        fill(dst, burstEnd, halfLine, blank);
        renderHalf(dst, halfLine, Half::equalizing);
        return;
    }
    const int row = line >= field2First ? line - field2First : line - field1First;
    if (row < 0 || row >= height) {
        fill(dst, burstEnd, samplesPerLine, blank);
        return;
    }
    fill(dst, burstEnd, activeStart, blank);
    fill(dst, activeStart + 3 * activePixels, samplesPerLine, blank);
    const uint8_t* src = pixels + row * width + firstPixel;
    // Three DAC samples per pixel, as in the upstream SMS/NES blitter.
    for (int x = 0, i = activeStart; x < activePixels; ++x) {
        const uint16_t* wave = activePalette[src[x]];
        sample(dst, i, wave[(i + phase) & 3]); ++i;
        sample(dst, i, wave[(i + phase) & 3]); ++i;
        sample(dst, i, wave[(i + phase) & 3]); ++i;
    }
}

// Negative U axis is the burst reference. The subcarrier runs through every line
// and 910 samples hold 227.5 cycles, so a line starts on the opposite phase to the
// one before, as with any camera. Burst edges at half amplitude.
void buildBurst() {
    for (int half = 0; half < 2; ++half) {
        uint16_t* out = reinterpret_cast<uint16_t*>(burstPairs[half]);
        for (int i = 0; i < burstEnd - burstStart; ++i) {
            const int amplitude = i < 4 || i >= burstEnd - burstStart - 4 ? burstAmplitude / 2 : burstAmplitude;
            const int p = (i + burstStart + 2 * half) & 3;
            const int code = Detail::blankCode + (p == 0 ? -amplitude : p == 2 ? amplitude : 0);
            sample(out, i, static_cast<uint16_t>(code << 8));
        }
    }
}

void IRAM_ATTR renderNext() {
    int line = nextLine;
    uint16_t* dst = reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(dma[nextBuffer].buf));
    const bool pal = palRaster;
    if (pal) {
        Detail::renderPalLine(dst, line, secondFrame, linePhase, nextBuffer & 1, pixels, activePalette, palBands);
        linePhase = (linePhase + palSamplesPerLine) & 3;
    } else {
        renderLine(dst, line, linePhase);
        linePhase ^= 2;
    }
    // Each field's picture is finished by now and the next one is lines away,
    // so a queued image may start with the next field.
    if (pal ? line == 311 || line == 624 : line == 263 || line == 1) {
        portENTER_CRITICAL_ISR(&exchangeLock);
        if (exchange.onLineRendered(height)) {
            pixels = exchange.front();
            activePalette = palettes[exchange.frontBank()].wave;
        }
        portEXIT_CRITICAL_ISR(&exchangeLock);
    }
    if (++line > (pal ? palLinesPerFrame : linesPerFrame)) {
        line = 1;
        ++frameCount;
        if (pal) secondFrame = !secondFrame;
    }
    nextLine = line;
}

void IRAM_ATTR interrupt(void*) {
    const uint32_t status = I2S0.int_st.val;
    const bool eof = I2S0.int_st.out_eof;
    I2S0.int_clr.val = status;          // first: an EOF during the rendering must not be lost
    if (eof) {
        // Normally the one the interrupt is for; after a long delay, all it missed.
        const int last = reinterpret_cast<lldesc_t*>(I2S0.out_eof_des_addr) - dma;
        for (;;) {
            renderNext();
            const bool done = nextBuffer == last;
            nextBuffer = nextBuffer + 1 == dmaLines ? 0 : nextBuffer + 1;
            if (done) break;
        }
    }
}

// The descriptors and first lines of the current standard, and the ISR's state to
// continue from them.
void prepareRaster() {
    for (int i = 0; i < dmaLines; ++i) {
        const int samples = !palRaster ? samplesPerLine : palSamplesPerLine + (i & 1 ? -1 : 1);
        dma[i].length = samples * 2;
        dma[i].size = samples * 2;
        uint16_t* line = reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(dma[i].buf));
        if (palRaster) Detail::renderPalLine(line, i + 1, false, (i * palSamplesPerLine) & 3, i & 1, pixels, activePalette, palBands);
        else renderLine(line, i + 1, (i & 1) * 2);
    }
    nextLine = dmaLines + 1;
    nextBuffer = 0;
    linePhase = palRaster ? (dmaLines * palSamplesPerLine) & 3 : (dmaLines & 1) * 2;
    secondFrame = false;
}

// Upstream's 40MHz crystal APLL coefficients: 4x the subcarrier, NTSC's or PAL's.
void setSampleClock() {
    if (palRaster) rtc_clk_apll_enable(true, 0x04, 0xA4, 0x6, 1);
    else rtc_clk_apll_enable(true, 0x46, 0x97, 0x4, 1);
}

void stopOutput() {
    I2S0.int_ena.val = 0;
    I2S0.conf.tx_start = 0;
    I2S0.out_link.stop = 1;
}

void cleanup() {
    stopOutput();
    if (interruptHandle) {
        esp_intr_free(interruptHandle);
        interruptHandle = nullptr;
    }
    for (auto& descriptor : dma) {
        if (descriptor.buf) heap_caps_free(const_cast<uint8_t*>(descriptor.buf));
        descriptor = {};
    }
    i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
}
}

bool begin(uint8_t* frontRgb332, uint8_t* backRgb332) {
    if (started) return true;
    if (!frontRgb332 || !backRgb332 || frontRgb332 == backRgb332 ||
        !esp_ptr_internal(frontRgb332) || !esp_ptr_internal(backRgb332) ||
        rtc_clk_xtal_freq_get() != RTC_XTAL_FREQ_40M) return false;
    exchange.reset(frontRgb332, backRgb332);
    pixels = frontRgb332;
    palettes[0].assign(nullptr);
    palettes[1].assign(nullptr);
    activePalette = palettes[0].wave;
    periph_module_enable(PERIPH_I2S0_MODULE);
    I2S0.int_ena.val = 0;
    I2S0.conf.tx_start = 0;
    I2S0.conf.val = 1;
    I2S0.conf.val = 0;
    I2S0.conf.tx_right_first = 1;
    I2S0.conf.tx_mono = 1;
    I2S0.conf2.lcd_en = 1;
    I2S0.fifo_conf.tx_fifo_mod_force_en = 1;
    I2S0.sample_rate_conf.tx_bits_mod = 16;
    I2S0.conf_chan.tx_chan_mod = 1;
    buildBurst();
    for (int i = 0; i < dmaLines; ++i) {
        // Room for either standard's longest buffer.
        dma[i].buf = static_cast<uint8_t*>(heap_caps_malloc((palSamplesPerLine + 1) * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (!dma[i].buf) { cleanup(); return false; }
        dma[i].owner = 1;
        dma[i].eof = 1;
        dma[i].empty = reinterpret_cast<uint32_t>(&dma[i + 1 == dmaLines ? 0 : i + 1]);
    }
    palRaster = false;
    prepareRaster();
    if (esp_intr_alloc(ETS_I2S0_INTR_SOURCE,
                      ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_INTRDISABLED,
                      interrupt, nullptr, &interruptHandle) != ESP_OK) {
        cleanup(); return false;
    }
    I2S0.out_link.addr = reinterpret_cast<uint32_t>(dma);
    setSampleClock();
    I2S0.clkm_conf.clkm_div_num = 1;
    I2S0.clkm_conf.clkm_div_b = 0;
    I2S0.clkm_conf.clkm_div_a = 1;
    I2S0.sample_rate_conf.tx_bck_div_num = 1;
    I2S0.clkm_conf.clka_en = 1;
    I2S0.fifo_conf.tx_fifo_mod = 1;

    // DAC1 is connected to the M5Stack speaker. Only DAC2/GPIO26 is powered.
    i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
    if (i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN) != ESP_OK) {
        cleanup(); return false;
    }
    I2S0.int_clr.val = 0xFFFFFFFF;
    I2S0.int_ena.out_eof = 1;
    frameCount = 0;
    if (esp_intr_enable(interruptHandle) != ESP_OK) { cleanup(); return false; }
    I2S0.out_link.start = 1;
    I2S0.conf.tx_start = 1;
    started = true;
    return true;
}

bool running() { return started; }
uint32_t frames() { return frameCount; }

bool setStandard(Standard next, const SampleBand* bands, int bandCount) {
    if (!started) return false;
    const bool pal = next == Standard::Pal;
    if (pal == palRaster) return true;
    // On this core the ISR is not in the middle of a line, and with int_ena clear
    // it will not run again until the new raster is ready.
    stopOutput();
    palRaster = pal;
    for (int i = 0; i < 2; ++i) palBands[i] = pal && i < bandCount ? bands[i] : SampleBand{0, 0, nullptr};
    palettes[0].setPal(pal);
    palettes[1].setPal(pal);
    prepareRaster();
    setSampleClock();
    // Whatever was left of the old raster's line, and back to the first descriptor.
    I2S0.conf.tx_reset = 1;
    I2S0.conf.tx_reset = 0;
    I2S0.conf.tx_fifo_reset = 1;
    I2S0.conf.tx_fifo_reset = 0;
    I2S0.lc_conf.out_rst = 1;
    I2S0.lc_conf.out_rst = 0;
    I2S0.out_link.addr = reinterpret_cast<uint32_t>(dma);
    I2S0.int_clr.val = 0xFFFFFFFF;
    I2S0.int_ena.out_eof = 1;
    I2S0.out_link.start = 1;
    I2S0.conf.tx_start = 1;
    return true;
}

Standard standard() { return palRaster ? Standard::Pal : Standard::Ntsc; }

uint8_t* framebuffer() {
    portENTER_CRITICAL(&exchangeLock);
    uint8_t* result = exchange.front();
    portEXIT_CRITICAL(&exchangeLock);
    return result;
}

uint8_t* backbuffer() {
    if (!started) return nullptr;
    portENTER_CRITICAL(&exchangeLock);
    uint8_t* result = exchange.writable();
    portEXIT_CRITICAL(&exchangeLock);
    return result;
}

bool setBackPalette(const uint8_t* rgb888) {
    if (!started) return false;
    portENTER_CRITICAL(&exchangeLock);
    const int bank = exchange.writableBank();
    portEXIT_CRITICAL(&exchangeLock);
    if (bank < 0) return false;
    // One producer owns the writable buffer until present(). It cannot change
    // banks while we compare/build its palette because no flip is queued.
    // The next present() release fence publishes both pixels and this table.
    palettes[bank].assign(rgb888);
    return true;
}

bool setBackPaletteSignal(uint8_t index, float y, float u, float v) {
    if (!started) return false;
    portENTER_CRITICAL(&exchangeLock);
    const int bank = exchange.writableBank();
    portEXIT_CRITICAL(&exchangeLock);
    if (bank < 0) return false;
    palettes[bank].setSignal(index, y, u, v);
    return true;
}

bool present() {
    if (!started) return false;
    portENTER_CRITICAL(&exchangeLock);
    // Publish all completed pixel stores before making the buffer visible to
    // the ISR on the other core. portMUX also orders the ownership metadata.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    const bool result = exchange.queue();
    portEXIT_CRITICAL(&exchangeLock);
    return result;
}

bool presentPending() {
    portENTER_CRITICAL(&exchangeLock);
    const bool result = exchange.pending();
    portEXIT_CRITICAL(&exchangeLock);
    return result;
}

uint32_t presentedFrames() {
    portENTER_CRITICAL(&exchangeLock);
    const uint32_t result = exchange.presented();
    portEXIT_CRITICAL(&exchangeLock);
    return result;
}
}
