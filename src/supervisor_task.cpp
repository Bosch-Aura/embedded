#include "supervisor_task.h"
#include "hardware.h"
#include "mailbox.h"
#include "messages.h"
#include "link_tx_task.h"
#include "motor_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>

#define SUPERVISOR_TASK_PERIOD_MS 20
// Watchdog timeouts come from hardware.h (WATCHDOG_TIMEOUT_AUTO_MS /
// WATCHDOG_TIMEOUT_MANUAL_MS). Do not redefine them here.

static mailbox_t *supervisor_mb = NULL;
static mailbox_t *motor_mb = NULL;
static mailbox_t *steer_mb = NULL;

static system_mode_t current_mode = MODE_MANUAL;
// C-3: the system boots DISARMED, as docs/BRAIN_TEAM_PROTOCOL.md promises.
static system_state_t current_state = STATE_DISARMED;
static system_state_t previous_state = STATE_DISARMED;
static uint32_t last_heartbeat_ms = 0;
static bool estop_triggered = false;

void supervisor_task(void *pvParameters) {
    supervisor_params_t *params = (supervisor_params_t *)pvParameters;
    supervisor_mb = params->supervisor_mailbox;
    motor_mb = params->motor_mailbox;
    steer_mb = params->steer_mailbox;
    
    Serial.println("[SupervisorTask] Supervisor task started");

    // A-5: the 100 ms "give link_tx_task time to initialize" delay is gone.
    // The TX queue is now created in setup() before any task exists, so there is
    // nothing to wait for.

    // Print initial state and mode at boot
    Serial.print("EVENT:STATE_CHANGED:");
    Serial.println(current_state == STATE_DISARMED ? "DISARMED" :
                   current_state == STATE_ARMED ? "ARMED" :
                   current_state == STATE_RUNNING ? "RUNNING" : "FAULT");
    Serial.flush();
    
    Serial.print("EVENT:MODE_CHANGED:");
    Serial.println(current_mode == MODE_AUTO ? "AUTO" : "MANUAL");
    Serial.flush();
    
    // Also send via link_tx for UART transmission
    link_tx_send_state_event(current_state);
    link_tx_send_mode_event(current_mode);
    
    while (1) {
        uint32_t current_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        // Read supervisor mailbox for commands
        topic_t topic;
        command_type_t cmd;
        int32_t value;
        uint32_t ts_ms;
        bool expired;
        
        if (mailbox_read(supervisor_mb, &topic, &cmd, &value, &ts_ms, &expired)) {
            if (!expired) {
                switch (cmd) {
                    case CMD_SYS_ARM:
                        // C-3 / M-4: FAULT is a valid starting point for arming;
                        // it is the natural recovery transition.
                        if (current_state == STATE_DISARMED || current_state == STATE_FAULT) {
                            // Only rearm if the cause of the fault is gone
                            if (estop_is_triggered()) {
                                Serial.println("EVENT:CMD_REJECTED:SYS_ARM:ESTOP_ACTIVE");
                                Serial.flush();
                                Serial.println("[SupervisorTask] SYS_ARM rejected - E-STOP still active");
                                break;
                            }
                            current_state = STATE_ARMED;
                            last_heartbeat_ms = current_ms;
                            Serial.println("EVENT:CMD_EXECUTED:SYS_ARM");
                            Serial.flush();
                            Serial.println("[SupervisorTask] System ARMED");
                            link_tx_send_state_event(current_state);
                        } else {
                            // Already armed: confirm anyway so the client does not hang
                            // waiting for EVENT:CMD_EXECUTED:SYS_ARM
                            Serial.println("EVENT:CMD_EXECUTED:SYS_ARM");
                            Serial.flush();
                        }
                        break;

                    case CMD_SYS_DISARM:
                        if (current_state != STATE_DISARMED) {
                            current_state = STATE_DISARMED;
                            // C-1: the motor is owned by MotorTask. Notify it instead of
                            // touching the hardware from this task; otherwise MotorTask
                            // overwrites the stop within 10 ms.
                            motor_task_trigger_emergency();
                            mailbox_write(steer_mb, TOPIC_STEER, CMD_STOP, 0, 100);
                            Serial.println("EVENT:CMD_EXECUTED:SYS_DISARM");
                            Serial.flush();
                            Serial.println("[SupervisorTask] System DISARMED");
                            link_tx_send_state_event(current_state);
                        }
                        break;
                        
                    case CMD_SYS_MODE:
                        {
                            system_mode_t new_mode = (value == MODE_AUTO) ? MODE_AUTO : MODE_MANUAL;
                            if (new_mode != current_mode) {
                                current_mode = new_mode;
                                // Reset heartbeat when switching to AUTO mode
                                // This prevents immediate watchdog timeout if last_heartbeat_ms was old
                                if (current_mode == MODE_AUTO) {
                                    last_heartbeat_ms = 0;
                                    Serial.println("[SupervisorTask] Heartbeat reset - waiting for first UART message");
                                }
                                Serial.print("EVENT:CMD_EXECUTED:SYS_MODE:");
                                Serial.println(current_mode == MODE_AUTO ? "AUTO" : "MANUAL");
                                Serial.flush();
                                Serial.print("[SupervisorTask] Mode changed to: ");
                                Serial.println(current_mode == MODE_AUTO ? "AUTO" : "MANUAL");
                                link_tx_send_mode_event(current_mode);
                            }
                        }
                        break;
                        
                    default:
                        break;
                }
            }
        }
        
        // Check E-STOP GPIO
        bool estop_current = estop_is_triggered();
        if (estop_current && !estop_triggered) {
            Serial.println("EVENT:ESTOP_TRIGGERED:GPIO");
            Serial.flush();
            Serial.println("[SupervisorTask] E-STOP triggered via GPIO!");
            estop_triggered = true;
            current_state = STATE_FAULT;
            motor_task_trigger_emergency();
            mailbox_write(steer_mb, TOPIC_STEER, CMD_STOP, 0, 100);
        } else if (!estop_current && estop_triggered) {
            Serial.println("EVENT:ESTOP_RELEASED");
            Serial.flush();
            Serial.println("[SupervisorTask] E-STOP released");
            estop_triggered = false;
        }
        
        // Watchdog: Check heartbeat timeout.
        // C-4: it must cover BOTH modes. MANUAL is the default mode, so guarding
        // only AUTO left the boot configuration with no link-loss protection.
        if (current_state != STATE_DISARMED && current_state != STATE_FAULT) {
            uint32_t watchdog_timeout_ms = (current_mode == MODE_AUTO)
                                               ? WATCHDOG_TIMEOUT_AUTO_MS
                                               : WATCHDOG_TIMEOUT_MANUAL_MS;
            if (last_heartbeat_ms > 0) {
                uint32_t heartbeat_age = current_ms - last_heartbeat_ms;
                if (heartbeat_age > watchdog_timeout_ms) {
                    Serial.println("EVENT:WATCHDOG_TIMEOUT");
                    Serial.flush();
                    Serial.print("[SupervisorTask] Watchdog timeout! Heartbeat age: ");
                    Serial.print(heartbeat_age);
                    Serial.println(" ms");
                    current_state = STATE_FAULT;
                    motor_task_trigger_emergency();
                    mailbox_write(steer_mb, TOPIC_STEER, CMD_STOP, 0, 100);
                }
            }
        }
        
        // State machine transitions
        if (current_state == STATE_ARMED) {
            if (current_mode == MODE_AUTO) {
                // In AUTO mode, transition to RUNNING if we have valid heartbeat
                if (last_heartbeat_ms > 0 && (current_ms - last_heartbeat_ms) < WATCHDOG_TIMEOUT_AUTO_MS) {
                    if (current_state != STATE_RUNNING) {
                        current_state = STATE_RUNNING;
                        Serial.println("EVENT:STATE_AUTO_TRANSITION:ARMED->RUNNING");
                        Serial.flush();
                    }
                }
            } else {
                // In MANUAL mode, ARMED automatically transitions to RUNNING
                if (current_state != STATE_RUNNING) {
                    current_state = STATE_RUNNING;
                    Serial.println("EVENT:STATE_AUTO_TRANSITION:ARMED->RUNNING");
                    Serial.flush();
                }
            }
        }
        
        // Send event if state changed (for automatic transitions)
        if (current_state != previous_state) {
            link_tx_send_state_event(current_state);
            previous_state = current_state;
        }
        
        // Periodic STATUS messages removed - use M:GET_STATUS:0 to request status on demand
        // or use telemetry_monitor.py to see all telemetry
        
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_TASK_PERIOD_MS));
    }
}

void supervisor_update_heartbeat(void) {
    last_heartbeat_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

system_mode_t supervisor_get_mode(void) {
    return current_mode;
}

system_state_t supervisor_get_state(void) {
    return current_state;
}
