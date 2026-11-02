#pragma once
#include <cstdint>
#include <thread>
#include <chrono>
#define portMAX_DELAY 0xffffffffu
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define taskYIELD() std::this_thread::yield()
using BaseType_t=int;
