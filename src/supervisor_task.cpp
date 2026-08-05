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

// MEDIO-3: these are written by SupervisorTask (core 1) and read by
// motor_control_allowed() from MotorTask (core 0), and last_heartbeat_ms is
// written from LinkRxTask and WebTask too. Without volatile the compiler is
// free to hoist the load out of MotorTask's while(1) - which would freeze the
// traction authorization at whatever value it had on the first iteration.
static volatile system_mode_t current_mode = MODE_MANUAL;

// Boot state.
//
// C-3 made the system boot DISARMED, matching docs/BRAIN_TEAM_PROTOCOL.md and
// requiring an explicit M:SYS_ARM:0 before anything can move.
//
// -DBOOT_ARMED (set in platformio.ini) skips that handshake: the vehicle comes
// up ARMED and, in MANUAL mode, immediately auto-transitions to RUNNING. The
// consequence is that the FIRST C:SET_SPEED to arrive is obeyed - there is no
// arming step to forget and no dashboard needed. It is still not able to move
// on its own: with no sustained command MotorTask brakes every cycle (C-1).
// Note that the watchdog only starts counting after the first heartbeat, so a
// board that boots ARMED and never receives anything stays in RUNNING.
#ifdef BOOT_ARMED
static volatile system_state_t current_state = STATE_ARMED;
static system_state_t previous_state = STATE_ARMED;
#else
static volatile system_state_t current_state = STATE_DISARMED;
static system_state_t previous_state = STATE_DISARMED;
#endif
static volatile uint32_t last_heartbeat_ms = 0;
static bool estop_triggered = false;

void supervisor_task(void *pvParameters) {
    supervisor_params_t *params = (supervisor_params_t *)pvParameters;
    supervisor_mb = params->supervisor_mailbox;
    motor_mb = params->motor_mailbox;
    steer_mb = params->steer_mailbox;
    
    // Timestamp of the last management command actually acted upon, so a stale
    // entry sitting in the mailbox until its TTL expires is not re-executed.
    uint32_t last_processed_cmd_ts = 0;

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
            // Management commands are EDGE triggered, not level triggered.
            // mailbox_read() does not consume the entry, so a SYS_ARM (TTL 5 s)
            // was re-executed on every 20 ms cycle for five seconds. That did
            // not just spam EVENT:CMD_EXECUTED:SYS_ARM - it silently re-armed
            // the vehicle right after a watchdog FAULT, defeating the very
            // protection that had just stopped it. Only act on a timestamp we
            // have not processed yet.
            if (!expired && ts_ms != last_processed_cmd_ts) {
                last_processed_cmd_ts = ts_ms;
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

// ALTO-1: latch the system into FAULT from another task.
// FAULT never clears by itself, so whoever calls this guarantees the vehicle
// stays stopped until an operator re-arms. Used by the ultrasonic emergency:
// braking alone was not enough, because once the 5 s cooldown expired the state
// was still RUNNING and a fresh SET_SPEED made the vehicle drive off again.
void supervisor_trigger_fault(const char *reason) {
    if (current_state != STATE_FAULT) {
        current_state = STATE_FAULT;
        Serial.print("EVENT:FAULT_TRIGGERED:");
        Serial.println(reason != NULL ? reason : "UNKNOWN");
        Serial.flush();
    }
}

system_mode_t supervisor_get_mode(void) {
    return current_mode;
}

system_state_t supervisor_get_state(void) {
    return current_state;
}
