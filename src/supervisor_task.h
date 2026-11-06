#ifndef SUPERVISOR_TASK_H
#define SUPERVISOR_TASK_H

#include <stdbool.h>
#include "messages.h"

#ifdef __cplusplus
extern "C" {
#endif

// Owns the system state, the link watchdog and the E-STOP input.
// SupervisorTask is the ONLY writer of the state; everyone else asks.
void supervisor_task(void *pvParameters);

system_state_t supervisor_get_state(void);
const char *supervisor_state_name(system_state_t state);

// LinkRxTask: every understood command keeps the link alive.
void supervisor_feed_watchdog(void);
// LinkRxTask: M:SYS_ARM:0 / M:SYS_DISARM:0. Handled on the next supervisor
// cycle (<= 20 ms); the last request wins.
void supervisor_request_arm(void);
void supervisor_request_disarm(void);

// STOPPED requests. Either one holds the car; with both gone it goes back to
// RUNNING. The SYS_STOP latch survives DISARM/FAULT/re-ARM: only
// M:SYS_RESUME:0 (or a reboot) clears it. latch/release return true if the
// latch actually changed.
void supervisor_set_obstacle(bool obstacle); // UltrasonicTask
bool supervisor_latch_stop(void);            // M:SYS_STOP:0
bool supervisor_release_stop(void);          // M:SYS_RESUME:0
bool supervisor_stop_requested(void);

#ifdef __cplusplus
}
#endif

#endif // SUPERVISOR_TASK_H
