#include "link_tx_task.h"
#include "hardware.h"
#include "messages.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#define TX_QUEUE_SIZE 10

typedef enum {
    MSG_TYPE_STATUS,
    MSG_TYPE_STATE_EVENT,
    MSG_TYPE_MODE_EVENT
} msg_type_t;

typedef struct {
    msg_type_t type;
    system_mode_t mode;
    system_state_t state;
    uint32_t heartbeat_age_ms;
} telemetry_msg_t;

static QueueHandle_t tx_queue = NULL;

// A-5: the queue must exist BEFORE any task can produce into it. Creating it
// inside link_tx_task() was a race: producers found tx_queue == NULL and dropped
// the initial STATE_CHANGED / MODE_CHANGED events in silence.
bool link_tx_init(void) {
    tx_queue = xQueueCreate(TX_QUEUE_SIZE, sizeof(telemetry_msg_t));
    if (tx_queue == NULL) {
        Serial.println("[LinkTxTask] Failed to create TX queue");
        return false;
    }
    return true;
}

void link_tx_task(void *pvParameters) {
    Serial.println("[LinkTxTask] LinkTx task started");
    
    while (1) {
        telemetry_msg_t msg;
        if (xQueueReceive(tx_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            char buffer[128];
            
            switch (msg.type) {
                case MSG_TYPE_STATUS:
                    // STATUS messages disabled - use events instead
                    // Skip sending STATUS messages to reduce noise
                    continue; // Skip to next iteration
                    
                case MSG_TYPE_STATE_EVENT:
                    snprintf(buffer, sizeof(buffer), "EVENT:STATE_CHANGED:%s\n",
                             msg.state == STATE_DISARMED ? "DISARMED" :
                             msg.state == STATE_ARMED ? "ARMED" :
                             msg.state == STATE_RUNNING ? "RUNNING" : "FAULT");
                    Serial.print(buffer);  // Also print to Serial (USB) for dashboard
                    Serial.flush();        // Ensure it's sent immediately
                    Serial1.print(buffer); // And to Serial1 (UART)
                    Serial1.flush();
                    break;
                    
                case MSG_TYPE_MODE_EVENT:
                    snprintf(buffer, sizeof(buffer), "EVENT:MODE_CHANGED:%s\n",
                             msg.mode == MODE_AUTO ? "AUTO" : "MANUAL");
                    Serial.print(buffer);  // Also print to Serial (USB) for dashboard
                    Serial.flush();        // Ensure it's sent immediately
                    Serial1.print(buffer); // And to Serial1 (UART)
                    Serial1.flush();
                    break;
            }
        }
    }
}

void link_tx_send_status(system_mode_t mode, system_state_t state, uint32_t heartbeat_age_ms) {
    if (tx_queue != NULL) {
        telemetry_msg_t msg = {
            .type = MSG_TYPE_STATUS,
            .mode = mode,
            .state = state,
            .heartbeat_age_ms = heartbeat_age_ms
        };
        xQueueSend(tx_queue, &msg, 0); // Non-blocking
    }
}

void link_tx_send_state_event(system_state_t state) {
    if (tx_queue != NULL) {
        telemetry_msg_t msg = {
            .type = MSG_TYPE_STATE_EVENT,
            .mode = MODE_MANUAL, // Not used for state events
            .state = state,
            .heartbeat_age_ms = 0
        };
        xQueueSend(tx_queue, &msg, 0); // Non-blocking
    }
}

void link_tx_send_mode_event(system_mode_t mode) {
    if (tx_queue != NULL) {
        telemetry_msg_t msg = {
            .type = MSG_TYPE_MODE_EVENT,
            .mode = mode,
            .state = STATE_DISARMED, // Not used for mode events
            .heartbeat_age_ms = 0
        };
        xQueueSend(tx_queue, &msg, 0); // Non-blocking
    }
}
