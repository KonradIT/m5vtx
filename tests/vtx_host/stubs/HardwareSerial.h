#pragma once
#include "Arduino.h"
#include <deque>
#include <vector>
class HardwareSerial {
 public:
  std::deque<uint8_t> incoming;
  std::vector<uint8_t> outgoing;
  explicit HardwareSerial(int) {}
  void setRxBufferSize(size_t) {}
  void begin(uint32_t,uint32_t,int,int,bool) {}
  void end() {}
  int available() { return int(incoming.size()); }
  int read() { if (incoming.empty()) return -1; int v=incoming.front(); incoming.pop_front(); return v; }
  size_t write(const uint8_t* p,size_t n) { outgoing.assign(p,p+n); return n; }
};
