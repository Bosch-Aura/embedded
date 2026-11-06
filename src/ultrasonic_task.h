#ifndef ULTRASONIC_TASK_H
#define ULTRASONIC_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

void ultrasonic_task(void *pvParameters);

// M:OBSTACLE_RESET:0 (LinkRxTask): release an obstacle hold now. Handled on the
// next reading; the hold comes back if the obstacle is still there.
void ultrasonic_request_reset(void);

#ifdef __cplusplus
}
#endif

#endif // ULTRASONIC_TASK_H

