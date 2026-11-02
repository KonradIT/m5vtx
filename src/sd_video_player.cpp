#include "sd_video_player.h"

#include "composite_video.h"
#include "sd_video_format.h"
#include <SD.h>
#include <SPI.h>
#include <esp_timer.h>
#include <string.h>
#include <strings.h>

namespace {
constexpr uint8_t sdCs = 4;
constexpr uint32_t sdFrequency = 20000000;
constexpr size_t readChunk = 8192;
constexpr size_t directoryLimit = 512;

class Lock {
 public:
  explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) {
    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  ~Lock() { if (mutex_) xSemaphoreGive(mutex_); }
 private:
  SemaphoreHandle_t mutex_;
};

bool videoExtension(const char* name) {
  const size_t length = name ? strlen(name) : 0;
  return length > 4 && !strcasecmp(name + length - 4, ".m5v");
}

void closeFile(File& file, SemaphoreHandle_t spiMutex) {
  Lock bus(spiMutex);
  file.close();
}

// Caller holds the shared bus. This consumes exactly the declared, supported
// header, leaving the stream positioned at the first frame for both versions.
size_t readHeader(File& file, uint8_t* bytes) {
  const size_t prefix = file.read(bytes, SdVideoFormat::prefixBytes);
  if (prefix != SdVideoFormat::prefixBytes) return prefix;
  uint16_t headerSize;
  if (SdVideoFormat::inspectPrefix(bytes, prefix, headerSize) != SdVideoFormat::Error::None) return prefix;
  return prefix + file.read(bytes + prefix, headerSize - prefix);
}
}

bool SdVideoPlayer::begin(SemaphoreHandle_t spiMutex) {
  if (task_) return spiMutex_ == spiMutex;
  if (!spiMutex) return false;
  spiMutex_ = spiMutex;
  stateMutex_ = xSemaphoreCreateMutex();
  if (!stateMutex_) return false;
  scanRequested_ = true;
  status_.scanning = true;
  snprintf(status_.message, sizeof(status_.message), "Scanning SD card");
  if (xTaskCreatePinnedToCore(taskEntry, "sd-video", 10240, this, 1, &task_, 0) != pdPASS) {
    vSemaphoreDelete(stateMutex_);
    stateMutex_ = nullptr;
    status_.scanning = false;
    snprintf(status_.message, sizeof(status_.message), "SD worker allocation failed");
    return false;
  }
  return true;
}

size_t SdVideoPlayer::count() const {
  if (!stateMutex_) return 0;
  Lock state(stateMutex_);
  return count_;
}

SdVideoPlayer::Entry SdVideoPlayer::entry(size_t index) const {
  if (!stateMutex_) return Entry{};
  Lock state(stateMutex_);
  return index < count_ ? entries_[index] : Entry{};
}

SdVideoPlayer::Status SdVideoPlayer::status() const {
  if (!stateMutex_) return status_;
  Lock state(stateMutex_);
  return status_;
}

bool SdVideoPlayer::rescan() {
  if (!task_) return false;
  Lock state(stateMutex_);
  ++generation_;
  requestedIndex_ = -1;
  scanRequested_ = true;
  status_.scanning = true;
  status_.playing = status_.loading = false;
  status_.selectedIndex = -1;
  snprintf(status_.message, sizeof(status_.message), "Scanning SD card");
  return true;
}

bool SdVideoPlayer::play(size_t index) {
  if (!task_) return false;
  Lock state(stateMutex_);
  if (status_.scanning || scanRequested_ || !status_.mounted || index >= count_) return false;
  ++generation_;
  requestedIndex_ = int(index);
  status_.selectedIndex = int(index);
  status_.loading = true;
  status_.playing = false;
  status_.frameIndex = 0;
  status_.framesPresented = status_.underruns = 0;
  status_.lastFrameReadMs = status_.maxReadUs = 0;
  status_.totalFrames = entries_[index].frameCount;
  status_.fpsNum = entries_[index].fpsNum;
  status_.fpsDen = entries_[index].fpsDen;
  status_.version = entries_[index].version;
  status_.headerSize = entries_[index].headerSize;
  snprintf(status_.message, sizeof(status_.message), "Loading %.70s", entries_[index].name);
  return true;
}

void SdVideoPlayer::stop() {
  if (!task_) return;
  {
    Lock state(stateMutex_);
    ++generation_;
    requestedIndex_ = -1;
    status_.playing = status_.loading = false;
    status_.selectedIndex = -1;
    if (!status_.scanning) snprintf(status_.message, sizeof(status_.message), "Playback stopped");
  }
  // Never hold the shared SPI bus while waiting: the worker may need it to
  // complete a bounded read and close its file before acknowledging the stop.
  for (;;) {
    bool idle;
    { Lock state(stateMutex_); idle = workerIdle_; }
    if (idle) return;
    vTaskDelay(1);
  }
}

bool SdVideoPlayer::current(uint32_t generation) const {
  Lock state(stateMutex_);
  return generation == generation_ && requestedIndex_ >= 0 && !scanRequested_;
}

void SdVideoPlayer::setMessage(const char* message) {
  Lock state(stateMutex_);
  snprintf(status_.message, sizeof(status_.message), "%s", message);
}

void SdVideoPlayer::taskEntry(void* context) {
  static_cast<SdVideoPlayer*>(context)->run();
}

void SdVideoPlayer::scan() {
  {
    Lock state(stateMutex_);
    count_ = 0;
    status_.scanning = true;
    status_.mounted = false;
    status_.rejectedFiles = 0;
  }
  bool mounted;
  {
    Lock bus(spiMutex_);
    // Match the Basic's TFT/SD VSPI wiring. Both libraries reconfigure their
    // own SPI transactions; the external mutex prevents concurrent transfers.
    SPI.begin(18, 19, 23, sdCs);
    SD.end();
    mounted = SD.begin(sdCs, SPI, sdFrequency, "/sd", 3, false);
  }
  if (!mounted) {
    Lock state(stateMutex_);
    status_.scanning = false;
    ++status_.sdErrors;
    snprintf(status_.message, sizeof(status_.message), "No SD card; select Rescan SD to retry");
    return;
  }
  File directory;
  {
    Lock bus(spiMutex_);
    directory = SD.open("/");
  }
  if (!directory) {
    closeFile(directory, spiMutex_);
    Lock state(stateMutex_);
    status_.scanning = false;
    ++status_.sdErrors;
    snprintf(status_.message, sizeof(status_.message), "Cannot read SD root directory");
    return;
  }
  size_t accepted = 0, examined = 0;
  uint32_t rejected = 0;
  uint8_t bytes[SdVideoFormat::maxHeaderBytes];
  for (; examined < directoryLimit; ++examined) {
    File file;
    {
      Lock bus(spiMutex_);
      file = directory.openNextFile();
    }
    if (!file) { closeFile(file, spiMutex_); break; }
    Entry candidate;
    bool eligible = false;
    uint64_t actualSize = 0;
    size_t received = 0;
    {
      Lock bus(spiMutex_);
      const char* path = file.path();
      const char* name = file.name();
      eligible = !file.isDirectory() && videoExtension(name);
      if (eligible) {
        if (!path || strlen(path) >= sizeof(candidate.path)) eligible = false;
        else {
          snprintf(candidate.path, sizeof(candidate.path), "%s", path);
          snprintf(candidate.name, sizeof(candidate.name), "%s", name);
          actualSize = uint64_t(file.size());
          received = readHeader(file, bytes);
        }
      }
      file.close();
    }
    if (eligible) {
      SdVideoFormat::Header header;
      const auto error = SdVideoFormat::decode(bytes, received, actualSize, header);
      if (error != SdVideoFormat::Error::None) {
        ++rejected;
        Serial.printf("SD_VIDEO skip %s: %s\n", candidate.name, SdVideoFormat::errorMessage(error));
      } else {
        candidate.frameCount = header.frameCount;
        candidate.fpsNum = header.fpsNum;
        candidate.fpsDen = header.fpsDen;
        candidate.version = header.version;
        candidate.headerSize = header.headerSize;
        candidate.paletteCrc = header.paletteCrc;
        snprintf(candidate.title, sizeof(candidate.title), "%s", header.title);
        Lock state(stateMutex_);
        size_t position = 0;
        while (position < accepted && strcasecmp(entries_[position].path, candidate.path) < 0) ++position;
        if (position < maxEntries) {
          if (accepted < maxEntries) ++accepted;
          for (size_t n = accepted - 1; n > position; --n) entries_[n] = entries_[n - 1];
          entries_[position] = candidate;
        }
      }
    }
    // Let UART/UI tasks run between directory operations, including on cards
    // containing many unrelated files. Scanning is only boot/manual work.
    vTaskDelay(1);
  }
  closeFile(directory, spiMutex_);
  {
    Lock state(stateMutex_);
    count_ = accepted;
    status_.mounted = true;
    status_.scanning = false;
    status_.rejectedFiles = rejected;
    snprintf(status_.message, sizeof(status_.message), "%u SD video%s%s",
             unsigned(accepted), accepted == 1 ? "" : "s",
             examined == directoryLimit ? " (directory scan limit reached)" : "");
  }
  Serial.printf("SD_VIDEO scan: %u valid, %lu rejected, 20MHz SPI\n", unsigned(accepted), (unsigned long)rejected);
}

void SdVideoPlayer::run() {
  for (;;) {
    bool doScan = false;
    int selection = -1;
    uint32_t generation;
    Entry chosen;
    {
      Lock state(stateMutex_);
      doScan = scanRequested_;
      scanRequested_ = false;
      generation = generation_;
      if (!doScan && requestedIndex_ >= 0 && size_t(requestedIndex_) < count_) {
        selection = requestedIndex_;
        chosen = entries_[selection];
        workerIdle_ = false;
      } else workerIdle_ = true;
    }
    if (doScan) { scan(); continue; }
    if (selection < 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

    File file;
    uint8_t headerBytes[SdVideoFormat::maxHeaderBytes];
    size_t headerRead = 0;
    uint64_t actualSize = 0;
    {
      Lock bus(spiMutex_);
      file = SD.open(chosen.path, FILE_READ);
      if (file) {
        actualSize = uint64_t(file.size());
        headerRead = readHeader(file, headerBytes);
      }
    }
    SdVideoFormat::Header header;
    const auto validation = SdVideoFormat::decode(headerBytes, headerRead, actualSize, header);
    bool failed = !file || validation != SdVideoFormat::Error::None ||
                  header.frameCount != chosen.frameCount || header.fpsNum != chosen.fpsNum || header.fpsDen != chosen.fpsDen ||
                  header.version != chosen.version || header.headerSize != chosen.headerSize || header.paletteCrc != chosen.paletteCrc;
    if (failed && current(generation)) {
      Lock state(stateMutex_);
      ++status_.sdErrors;
      snprintf(status_.message, sizeof(status_.message), "Video changed or unreadable; rescan SD");
    }
    uint32_t frameIndex = 0;
    bool first = true;
    SdVideoFormat::FrameClock clock;
    while (!failed && current(generation)) {
      uint8_t* pixels = CompositeVideo::backbuffer();
      if (!pixels) { vTaskDelay(1); continue; }
      if (!CompositeVideo::setBackPalette(header.isIndexed ? header.palette : nullptr)) {
        failed = true;
        setMessage("Video palette unavailable; playback stopped");
        break;
      }
      const uint64_t readStarted = uint64_t(esp_timer_get_time());
      size_t offset = 0;
      while (offset < SdVideoFormat::frameBytes && current(generation)) {
        const size_t remaining = SdVideoFormat::frameBytes - offset;
        const size_t wanted = remaining < readChunk ? remaining : readChunk;
        size_t received;
        const uint64_t chunkStarted = uint64_t(esp_timer_get_time());
        {
          Lock bus(spiMutex_);
          received = file.read(pixels + offset, wanted);
        }
        const uint32_t readUs = uint32_t(uint64_t(esp_timer_get_time()) - chunkStarted);
        {
          Lock state(stateMutex_);
          if (readUs > status_.maxReadUs) status_.maxReadUs = readUs;
        }
        if (received != wanted) { failed = true; break; }
        offset += received;
        taskYIELD();
      }
      if (!current(generation)) break;
      if (failed) {
        Lock state(stateMutex_);
        ++status_.sdErrors;
        snprintf(status_.message, sizeof(status_.message), "SD read failed; playback stopped");
        break;
      }
      uint64_t now = uint64_t(esp_timer_get_time());
      {
        Lock state(stateMutex_);
        status_.lastFrameReadMs = uint32_t((now - readStarted + 999) / 1000);
      }
      if (first) { clock.start(now, header.fpsNum, header.fpsDen); first = false; }
      else if (now > clock.dueUs + 2000) {
        Lock state(stateMutex_);
        ++status_.underruns;
        clock.start(now, header.fpsNum, header.fpsDen);
      }
      while (current(generation) && uint64_t(esp_timer_get_time()) < clock.dueUs) vTaskDelay(1);
      if (!current(generation)) break;
      if (!CompositeVideo::present()) {
        failed = true;
        setMessage("Video output unavailable; playback stopped");
        break;
      }
      // stop() must not return while a queued SD frame could still flip over a
      // subsequently selected built-in pattern. The engine flips at vblank.
      while (CompositeVideo::presentPending()) vTaskDelay(1);
      if (!current(generation)) break;
      {
        Lock state(stateMutex_);
        status_.loading = false;
        status_.playing = true;
        status_.frameIndex = frameIndex;
        ++status_.framesPresented;
        if (status_.framesPresented == 1)
          snprintf(status_.message, sizeof(status_.message), "Playing %.70s", chosen.name);
      }
      clock.advance();
      if (++frameIndex == header.frameCount) {
        frameIndex = 0;
        Lock bus(spiMutex_);
        if (!file.seek(header.headerSize)) {
          failed = true;
          Lock state(stateMutex_);
          ++status_.sdErrors;
          snprintf(status_.message, sizeof(status_.message), "SD seek failed; playback stopped");
        }
      }
    }
    closeFile(file, spiMutex_);
    while (CompositeVideo::presentPending()) vTaskDelay(1);
    {
      Lock state(stateMutex_);
      if (generation == generation_) {
        status_.playing = status_.loading = false;
        requestedIndex_ = -1;
        status_.selectedIndex = -1;
      }
      workerIdle_ = true;
    }
  }
}
