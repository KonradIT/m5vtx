#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class SdVideoPlayer {
 public:
  static constexpr size_t maxEntries = 32;
  struct Entry {
    char path[128] = {};
    char name[96] = {};
    char title[65] = {};
    uint32_t frameCount = 0;
    uint16_t fpsNum = 0;
    uint16_t fpsDen = 0;
    uint16_t version = 0;
    uint16_t headerSize = 0;
    uint32_t paletteCrc = 0;
  };
  struct Status {
    bool mounted = false;
    bool scanning = false;
    bool playing = false;
    bool loading = false;
    int selectedIndex = -1;
    uint32_t frameIndex = 0;
    uint32_t totalFrames = 0;
    uint16_t fpsNum = 0;
    uint16_t fpsDen = 0;
    uint16_t version = 0;
    uint16_t headerSize = 0;
    uint32_t framesPresented = 0;
    uint32_t underruns = 0;
    uint32_t sdErrors = 0;
    uint32_t rejectedFiles = 0;
    uint32_t lastFrameReadMs = 0;
    uint32_t maxReadUs = 0;
    char message[96] = "SD not initialized";
  };

  // The supplied mutex must also guard every LCD SPI transfer. Initialization
  // and the first root-directory scan run on the worker; there is no autoplay.
  bool begin(SemaphoreHandle_t spiMutex);
  bool rescan();
  size_t count() const;
  Entry entry(size_t index) const;  // Empty path means invalid/out of range.
  bool play(size_t index);
  // Call outside the shared SPI mutex. Returns after the worker has stopped
  // touching the framebuffer, so a built-in pattern can be displayed safely.
  void stop();
  Status status() const;

 private:
  SemaphoreHandle_t spiMutex_ = nullptr;
  SemaphoreHandle_t stateMutex_ = nullptr;
  TaskHandle_t task_ = nullptr;
  Entry entries_[maxEntries];
  size_t count_ = 0;
  Status status_;
  bool scanRequested_ = false;
  int requestedIndex_ = -1;
  uint32_t generation_ = 0;
  bool workerIdle_ = true;
  static void taskEntry(void* context);
  void run();
  void scan();
  bool current(uint32_t generation) const;
  void setMessage(const char* message);
};
