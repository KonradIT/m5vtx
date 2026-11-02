#pragma once
#include <stdlib.h>

#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_INTERNAL 0
inline void* heap_caps_malloc(size_t size, int) { return malloc(size); }
inline void heap_caps_free(void* memory) { free(memory); }
