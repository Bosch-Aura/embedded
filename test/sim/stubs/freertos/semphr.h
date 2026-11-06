#pragma once
#include "FreeRTOS.h"
typedef std::recursive_mutex *SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::recursive_mutex(); }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t) { s->lock(); return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { s->unlock(); return pdTRUE; }
