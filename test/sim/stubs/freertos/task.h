#pragma once
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t t);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *params,
                                   UBaseType_t prio, TaskHandle_t *handle, BaseType_t core);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void xTaskNotifyGive(TaskHandle_t h);
typedef enum { eNoAction, eSetBits } eNotifyAction;
BaseType_t xTaskNotify(TaskHandle_t h, uint32_t v, eNotifyAction a);
inline void vTaskNotifyGiveFromISR(TaskHandle_t h, BaseType_t *woken) { xTaskNotifyGive(h); if (woken) *woken = pdFALSE; }
#define portYIELD_FROM_ISR() do {} while (0)
