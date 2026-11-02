#pragma once
#include "SPI.h"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#define FILE_READ "r"
struct MockFileRecord { std::string path; std::vector<uint8_t> bytes; bool directory=false; };
class File {
 public:
  File()=default;
  explicit File(std::shared_ptr<MockFileRecord> record):record_(record) {}
  explicit operator bool() const { return bool(record_); }
  const char* path() const { assertSharedBus(); return record_?record_->path.c_str():""; }
  const char* name() const {
    assertSharedBus(); if(!record_) return "";
    const auto slash=record_->path.find_last_of('/');
    return record_->path.c_str()+(slash==std::string::npos?0:slash+1);
  }
  bool isDirectory() const { assertSharedBus(); return record_ && record_->directory; }
  size_t size() const { assertSharedBus(); return record_?record_->bytes.size():0; }
  size_t read(uint8_t* output,size_t wanted);
  bool seek(uint32_t position);
  File openNextFile();
  void close() { assertSharedBus(); record_.reset(); }
 private:
  std::shared_ptr<MockFileRecord> record_;
  size_t position_=0;
  size_t directoryIndex_=0;
};
class MockSd {
 public:
  std::vector<std::shared_ptr<MockFileRecord>> files;
  std::atomic<bool> mountAllowed{true}, failPayloadRead{false};
  std::atomic<unsigned> beginCalls{0}, directoryCalls{0}, seekCalls{0};
  std::atomic<uint32_t> lastSeekPosition{0};
  std::atomic<unsigned> delayPerReadMs{1};
  std::atomic<size_t> maxReadSize{0};
  bool begin(uint8_t,SPIClass&,uint32_t,const char*,uint8_t,bool) {
    assertSharedBus(); ++beginCalls; return mountAllowed;
  }
  void end() { assertSharedBus(); }
  File open(const char* path,const char* mode=FILE_READ);
};
extern MockSd SD;
