#include "motor_task.h"
#include "hardware.h"
#include "mailbox.h"
#include "messages.h"
#include "supervisor_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>

#define MOTOR_TASK_PERIOD_MS 10 // 100 Hz
#define EMERGENCY_NOTIFICATION_BIT (1 << 0)
#define DEFAULT_FORWARD_SPEED 220 // Default speed when no command received (0-255)
#define STOP_COOLDOWN_MS 5000 // 5 seconds cooldown after stop

static mailbox_t *motor_mailbox = NULL;
static TaskHandle_t motor_task_handle = NULL;

// True when the supervisor allows the motor to move (AUTO: RUNNING, MANUAL: ARMED/RUNNING).
// In FAULT or DISARMED the motor must never move.
static bool motor_can_move(void)
{
    system_state_t state = supervisor_get_state();
    if (supervisor_get_mode() == MODE_AUTO)
    {
        return state == STATE_RUNNING;
    }
    return state == STATE_ARMED || state == STATE_RUNNING;
}

void motor_task(void *pvParameters)
{
    motor_mailbox = (mailbox_t *)pvParameters;
    motor_task_handle = xTaskGetCurrentTaskHandle();

    uint8_t current_speed = 0;
    uint8_t last_valid_speed = 0; // Store last valid speed command
    bool has_received_speed_command = false; // Track if we've ever received a speed command
    bool motor_direction = true; // forward
    bool has_valid_command = false;
    uint32_t last_stop_timestamp = 0;
    bool in_cooldown = false;
    int32_t last_ignored_speed = -1; // Track last ignored speed command to avoid repeated logs

    Serial.println("[MotorTask] Motor task started");

    while (1)
    {
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        // Check for emergency notifications FIRST (<1ms response) - before cooldown check
        uint32_t notification_value = ulTaskNotifyTake(pdTRUE, 0); // Clear notification bits
        if (notification_value > 0)
        {
            Serial.println("EVENT:CMD_EXECUTED:EMERGENCY_BRAKE");
            Serial.flush();
            Serial.println("[MotorTask] Emergency brake triggered!");
            motor_stop();
            lights_set_reverse(false);
            current_speed = 0;
            has_valid_command = false;
            // Forget the held speed: after the cooldown the car must stay stopped
            // until the Jetson sends a new SET_SPEED (it resumes explicitly).
            last_valid_speed = 0;
            has_received_speed_command = false;
            last_stop_timestamp = current_time;
            in_cooldown = true;
            Serial.println("[MotorTask] 5 second cooldown started");
            // Don't continue here - let it fall through to ensure motor stays stopped in cooldown check
        }
        
        // Check if cooldown period has elapsed
        if (last_stop_timestamp > 0)
        {
            uint32_t elapsed = current_time - last_stop_timestamp;
            if (elapsed >= STOP_COOLDOWN_MS)
            {
                in_cooldown = false;
                last_stop_timestamp = 0; // Reset
                Serial.println("[MotorTask] Stop cooldown expired, motor can move again");
            }
            else
            {
                in_cooldown = true;
            }
        }
        else
        {
            in_cooldown = false;
        }

        // Read mailbox for motor commands
        topic_t topic;
        command_type_t cmd;
        int32_t value;
        uint32_t ts_ms;
        bool expired;

        has_valid_command = false;

        if (mailbox_read(motor_mailbox, &topic, &cmd, &value, &ts_ms, &expired))
        {
            if (!expired)
            {
                has_valid_command = true;
                switch (cmd)
                {
                case CMD_SET_SPEED: {
                    // SET_SPEED:0 is a stop: always honour it, in any state/mode/cooldown.
                    if (value <= 0)
                    {
                        if (current_speed != 0 || has_received_speed_command)
                        {
                            Serial.println("EVENT:CMD_EXECUTED:SET_SPEED:0");
                            Serial.flush();
                        }
                        motor_stop();
                        lights_set_reverse(false);
                        current_speed = 0;
                        last_valid_speed = 0;
                        has_received_speed_command = false;
                        last_ignored_speed = -1;
                        break;
                    }

                    // Check system state before allowing speed commands
                    bool can_control = motor_can_move();

                    if (!can_control) {
                        // Only print if this is a different command than the last ignored one
                        if (last_ignored_speed != value) {
                            Serial.println("[MotorTask] SET_SPEED ignored - system DISARMED");
                            Serial.flush();
                            last_ignored_speed = value;
                        }
                        has_valid_command = false; // Treat as no command
                    }
                    // Only allow speed commands if not in cooldown
                    else if (in_cooldown)
                    {
                        Serial.println("[MotorTask] Speed command ignored (in cooldown)");
                        has_valid_command = false; // Treat as no command
                        // Reset ignored tracking when command can be executed but is in cooldown
                        last_ignored_speed = -1;
                    }
                    else
                    {
                        // Reset ignored tracking when command can be executed
                        last_ignored_speed = -1;
                        uint8_t new_speed = (uint8_t)value;
                        if (new_speed > MOTOR_SPEED_MAX)
                        {
                            new_speed = MOTOR_SPEED_MAX;
                        }
                        // Only execute and print if speed actually changed
                        if (new_speed != current_speed)
                        {
                            current_speed = new_speed;
                            last_valid_speed = current_speed; // Store last valid speed
                            has_received_speed_command = true; // Mark that we've received a speed command
                            // Ensure forward direction when setting speed
                            motor_set_direction(true);
                            motor_set_speed(current_speed);
                            motor_direction = true;
                            lights_set_reverse(false);
                            Serial.print("EVENT:CMD_EXECUTED:SET_SPEED:");
                            Serial.println(current_speed);
                            Serial.flush();
                        } else {
                            // Speed didn't change, but still update last_valid_speed and flags
                            last_valid_speed = current_speed;
                            has_received_speed_command = true;
                            motor_set_direction(true);
                            motor_set_speed(current_speed);
                            motor_direction = true;
                            lights_set_reverse(false);
                        }
                    }
                    break;
                }

                case CMD_BRAKE_NOW:
                case CMD_STOP:
                    Serial.println("EVENT:CMD_EXECUTED:BRAKE_NOW");
                    Serial.flush();
                    motor_stop();
                    lights_set_reverse(false);
                    current_speed = 0;
                    motor_direction = true;
                    // Forget the held speed so the car does not resume by itself after the cooldown.
                    last_valid_speed = 0;
                    has_received_speed_command = false;
                    last_stop_timestamp = current_time;
                    in_cooldown = true;
                    Serial.println("[MotorTask] Motor stopped (brake/stop command), 5 second cooldown started");
                    break;

                default:
                    break;
                }
            }
        }

        // Apply motor control based on state
        if (in_cooldown)
        {
            // Ensure motor stays stopped during cooldown
            motor_stop();
            current_speed = 0;
        }
        else if (has_valid_command)
        {
            // Valid command is already applied above, nothing to do here
        }
        else if (has_received_speed_command && !motor_can_move())
        {
            // FAULT (watchdog/E-STOP) or DISARMED: never hold a speed - stop and forget it.
            motor_stop();
            current_speed = 0;
            last_valid_speed = 0;
            has_received_speed_command = false;
            motor_direction = true;
            lights_set_reverse(false);
            Serial.println("[MotorTask] Held speed cleared - system not RUNNING");
        }
        else if (has_received_speed_command)
        {
            // Command expired, but maintain last valid speed (don't revert to default)
            current_speed = last_valid_speed;
            motor_set_direction(true);
            motor_set_speed(current_speed);
            motor_direction = true;
            lights_set_reverse(false);
        }
        else
        {
            // No speed command ever received, stop the motor
            motor_stop();
            current_speed = 0;
            motor_direction = true;
            lights_set_reverse(false);
        }

        vTaskDelay(pdMS_TO_TICKS(MOTOR_TASK_PERIOD_MS));
    }
}

void motor_task_trigger_emergency(void)
{
    if (motor_task_handle != NULL)
    {
        xTaskNotify(motor_task_handle, EMERGENCY_NOTIFICATION_BIT, eSetBits);
    }
}
