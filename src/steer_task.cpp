#include "steer_task.h"
#include "hardware.h"
#include "mailbox.h"
#include "messages.h"
#include "supervisor_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>

#define STEER_TASK_PERIOD_MS 10  // 100 Hz

static mailbox_t *steer_mailbox = NULL;

void steer_task(void *pvParameters) {
    steer_mailbox = (mailbox_t *)pvParameters;
    
    uint16_t current_angle = SERVO_CENTER;
    int32_t last_ignored_angle = -1; // Track last ignored angle command to avoid repeated logs
    
    Serial.println("[SteerTask] Steer task started");
    
    while (1) {
        // Read mailbox for steering commands
        topic_t topic;
        command_type_t cmd;
        int32_t value;
        uint32_t ts_ms;
        bool expired;
        
        if (mailbox_read(steer_mailbox, &topic, &cmd, &value, &ts_ms, &expired)) {
            if (!expired) {
                switch (cmd) {
                    case CMD_SET_STEER: {
                        // Check system state before allowing steering commands
                        system_state_t state = supervisor_get_state();
                        system_mode_t mode = supervisor_get_mode();
                        bool can_control = false;
                        
                        if (mode == MODE_AUTO) {
                            // In AUTO mode, need to be RUNNING
                            can_control = (state == STATE_RUNNING);
                        } else {
                            // In MANUAL mode, ARMED is enough
                            can_control = (state == STATE_ARMED || state == STATE_RUNNING);
                        }
                        
                        if (!can_control) {
                            // Only print if this is a different command than the last ignored one
                            if (last_ignored_angle != value) {
                                Serial.println("[SteerTask] SET_STEER ignored - system DISARMED");
                                Serial.flush();
                                last_ignored_angle = value;
                            }
                            break;
                        }
                        
                        // Reset ignored tracking when command can be executed
                        last_ignored_angle = -1;
                        
                        // A-8: clamp in the SIGNED type, before narrowing to uint16_t.
                        // Casting first turned -30 into 65506, which then clamped to
                        // SERVO_RIGHT: a left command produced full RIGHT lock.
                        if (value < SERVO_LEFT) {
                            value = SERVO_LEFT;
                        } else if (value > SERVO_RIGHT) {
                            value = SERVO_RIGHT;
                        }
                        uint16_t new_angle = (uint16_t)value;
                        // Only execute and print if angle actually changed
                        if (new_angle != current_angle) {
                            current_angle = new_angle;
                            steer_set_angle(current_angle);
                            Serial.print("EVENT:CMD_EXECUTED:SET_STEER:");
                            Serial.println(current_angle);
                            Serial.flush();
                        }
                        break;
                    }
                        
                    case CMD_STOP:
                        // Only execute and print if not already centered
                        if (current_angle != SERVO_CENTER) {
                            current_angle = SERVO_CENTER;
                            steer_set_angle(current_angle);
                            Serial.println("EVENT:CMD_EXECUTED:SET_STEER_CENTER");
                            Serial.flush();
                            Serial.println("[SteerTask] Steering centered (stop command)");
                        }
                        break;
                        
                    default:
                        break;
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(STEER_TASK_PERIOD_MS));
    }
}
