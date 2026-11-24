#include "supervisor_task.h"
#include "hardware.h"
#include "mailbox.h"
#include "messages.h"
#include "link_tx_task.h"
#include "motor_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>

#define SUPERVISOR_TASK_PERIOD_MS 50
// 300 ms: with 120 ms, Python pauses on the Jetson (TensorRT/YOLO load, GIL) tripped
// false FAULTs. Still brakes in under a third of a second if the Jetson really dies.
#define WATCHDOG_TIMEOUT_MS 300

static mailbox_t *supervisor_mb = NULL;
static mailbox_t *motor_mb = NULL;
static mailbox_t *steer_mb = NULL;

static system_mode_t current_mode = MODE_MANUAL;
static system_state_t current_state = STATE_ARMED;
static system_state_t previous_state = STATE_ARMED;
static volatile uint32_t last_heartbeat_ms = 0; // written by link_rx_task
static bool estop_triggered = false;


// Age of the last heartbeat. last_heartbeat_ms is written by link_rx_task, so it can be
// NEWER than a tick read earlier in this loop: an unsigned subtraction would then wrap
// to ~4e9 ms and fire a false WATCHDOG_TIMEOUT (-> FAULT). Read "now" fresh and clamp.
static uint32_t heartbeat_age_ms(void) {
    uint32_t last = last_heartbeat_ms;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int32_t age = (int32_t)(now - last);
    return age < 0 ? 0 : (uint32_t)age;
}

void supervisor_task(void *pvParameters) {
    supervisor_params_t *params = (supervisor_params_t *)pvParameters;
    supervisor_mb = params->supervisor_mailbox;
    motor_mb = params->motor_mailbox;
    steer_mb = params->steer_mailbox;
    
    Serial.println("[SupervisorTask] Supervisor task started");
    
    // Give other tasks time to initialize (especially link_tx_task)
    vTaskDelay(pdMS_TO_TICKS(100));
    
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
                        // ARM also clears FAULT (watchdog), unless the E-STOP is still pressed.
                        if (current_state == STATE_DISARMED ||
                            (current_state == STATE_FAULT && !estop_triggered)) {
                            current_state = STATE_ARMED;
                            last_heartbeat_ms = current_ms;
                            Serial.println("EVENT:CMD_EXECUTED:SYS_ARM");
                            Serial.flush();
                            Serial.println("[SupervisorTask] System ARMED");
                            link_tx_send_state_event(current_state);
                        }
                        // If already armed, don't print again
                        break;
                        
                    case CMD_SYS_DISARM:
                        if (current_state != STATE_DISARMED) {
                            current_state = STATE_DISARMED;
                            motor_stop();
                            steer_set_angle(SERVO_CENTER);
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
        
        // Watchdog: Check heartbeat timeout
        if (current_mode == MODE_AUTO && current_state != STATE_DISARMED) {
            if (last_heartbeat_ms > 0) {
                uint32_t heartbeat_age = heartbeat_age_ms();
                if (heartbeat_age > WATCHDOG_TIMEOUT_MS) {
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
                if (last_heartbeat_ms > 0 && heartbeat_age_ms() < WATCHDOG_TIMEOUT_MS) {
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
