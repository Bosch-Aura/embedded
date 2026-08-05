#ifndef MOTOR_TASK_H
#define MOTOR_TASK_H

#include <stdbool.h>
#include "mailbox.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

void motor_task(void *pvParameters);
void motor_task_trigger_emergency(void);

// BLOQUEANTE-2: request a direction. MotorTask is the only task that drives the
// H-bridge pins; it applies this with a dead time so the bridge is never
// reversed while energised. Callers must NOT call motor_set_direction().
void motor_set_requested_direction(bool forward);

#ifdef __cplusplus
}
#endif

#endif // MOTOR_TASK_H



