#ifndef LINK_RX_TASK_H
#define LINK_RX_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

// Reads commands from USB and from the UART to the Jetson and dispatches them.
void link_rx_task(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif // LINK_RX_TASK_H
