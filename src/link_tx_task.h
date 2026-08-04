#ifndef LINK_TX_TASK_H
#define LINK_TX_TASK_H

#include "messages.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates the TX queue. Must be called from setup(), before any task is created.
bool link_tx_init(void);
void link_tx_task(void *pvParameters);
void link_tx_send_status(system_mode_t mode, system_state_t state, uint32_t heartbeat_age_ms);
void link_tx_send_state_event(system_state_t state);
void link_tx_send_mode_event(system_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif // LINK_TX_TASK_H



