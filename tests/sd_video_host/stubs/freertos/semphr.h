#pragma once
#include "FreeRTOS.h"
#include <mutex>
struct MockSemaphore { std::mutex mutex; std::thread::id owner; };
using SemaphoreHandle_t=MockSemaphore*;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new MockSemaphore; }
inline void vSemaphoreDelete(SemaphoreHandle_t sem) { delete sem; }
inline int xSemaphoreTake(SemaphoreHandle_t sem,uint32_t) {
  sem->mutex.lock(); sem->owner=std::this_thread::get_id(); return 1;
}
inline int xSemaphoreGive(SemaphoreHandle_t sem) {
  sem->owner=std::thread::id{}; sem->mutex.unlock(); return 1;
}
