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
void supervisor_trigger_fault(const char *reason);
system_mode_t supervisor_get_mode(void);
system_state_t supervisor_get_state(void);

#ifdef __cplusplus
}
#endif

#endif // SUPERVISOR_TASK_H



