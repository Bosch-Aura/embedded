#ifndef SUPERVISOR_TASK_H
#define SUPERVISOR_TASK_H

#include "mailbox.h"
#include "messages.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mailbox_t *supervisor_mailbox;
    mailbox_t *motor_mailbox;
    mailbox_t *steer_mailbox;
} supervisor_params_t;

void supervisor_task(void *pvParameters);
void supervisor_update_heartbeat(void);
system_state_t supervisor_get_state(void);
const char *supervisor_state_name(system_state_t state);

// STOPPED requests. Either one keeps the system in STOPPED; when both are gone
// it goes back to RUNNING.
void supervisor_set_obstacle(bool obstacle);   // UltrasonicTask
// M:SYS_STOP:0 latches the stop and only M:SYS_RESUME:0 releases it: no other
// command (SET_SPEED, SET_STEER, SYS_ARM, SYS_DISARM) clears it. Both return
// true if the latch actually changed.
bool supervisor_latch_stop(void);
bool supervisor_release_stop(void);
// True while a stop is requested. MotorTask and SteerTask check it directly so
// the brake does not wait for the next SupervisorTask cycle.
bool supervisor_stop_requested(void);

#ifdef __cplusplus
}
#endif

#endif // SUPERVISOR_TASK_H



