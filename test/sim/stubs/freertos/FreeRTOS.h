#pragma once
#include "../sim_core.h"
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
struct portMUX_TYPE { std::recursive_mutex m; };
#define portMUX_INITIALIZER_UNLOCKED {}
#define taskENTER_CRITICAL(mux) (mux)->m.lock()
#define taskEXIT_CRITICAL(mux) (mux)->m.unlock()
struct sim_task;
typedef sim_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
