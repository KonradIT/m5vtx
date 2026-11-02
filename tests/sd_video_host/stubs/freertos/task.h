#pragma once
#include "FreeRTOS.h"
using TaskHandle_t=void*;
inline void vTaskDelay(uint32_t ticks) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ticks?ticks:1));
}
inline int xTaskCreatePinnedToCore(void(*entry)(void*),const char*,uint32_t,void* context,int,TaskHandle_t* handle,int) {
  try { std::thread(entry,context).detach(); *handle=context; return pdPASS; }
  catch(...) { return -1; }
}
