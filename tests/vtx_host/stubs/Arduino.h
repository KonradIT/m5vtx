#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
extern uint32_t mockMillis;
inline uint32_t millis() { return mockMillis; }
inline uint32_t micros() { return mockMillis*1000; }
inline void delay(uint32_t ms) { mockMillis+=ms; }
#define IRAM_ATTR
#define CHANGE 3
inline void detachInterrupt(int) {}
inline void attachInterruptArg(int,void(*)(void*),void*,int) {}
struct MockConsole {
  template<typename... Args> void printf(const char*, Args...) {}
  void println() {}
};
extern MockConsole Serial;
#define SERIAL_8N1 0
#define SERIAL_8N2 1
