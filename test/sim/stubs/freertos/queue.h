#pragma once
#include "FreeRTOS.h"
#include <vector>
struct sim_queue { std::mutex m; std::condition_variable cv; std::deque<std::vector<uint8_t>> q; size_t n, sz; };
typedef sim_queue *QueueHandle_t;
inline QueueHandle_t xQueueCreate(size_t n, size_t sz) { auto *q = new sim_queue(); q->n = n; q->sz = sz; return q; }
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
