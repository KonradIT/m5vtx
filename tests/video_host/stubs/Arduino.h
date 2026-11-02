#pragma once
#include <stdint.h>
#include <stdlib.h>

// What src/composite_video.cpp takes from the framework, for the host.
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
inline uint32_t millis() { return 0; }
inline void delay(uint32_t) {}
