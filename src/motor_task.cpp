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
#define STOP_COOLDOWN_MS 5000 // 5 seconds cooldown after stop

static mailbox_t *motor_mailbox = NULL;
static TaskHandle_t motor_task_handle = NULL;

// C-1: single authorization helper, used both for fresh commands and for the
// no-command fallback. Traction is only allowed in states where the vehicle is
// actually supposed to be able to move.
static bool motor_control_allowed(void)
{
    system_state_t state = supervisor_get_state();
    system_mode_t mode = supervisor_get_mode();

    if (mode == MODE_AUTO)
    {
        // In AUTO mode, need to be RUNNING
        return (state == STATE_RUNNING);
    }
    // In MANUAL mode, ARMED is enough
    return (state == STATE_ARMED || state == STATE_RUNNING);
}

void motor_task(void *pvParameters)
{
    motor_mailbox = (mailbox_t *)pvParameters;
    motor_task_handle = xTaskGetCurrentTaskHandle();

    uint8_t current_speed = 0;
    bool motor_direction = true; // forward
    bool has_valid_command = false;
    uint32_t last_stop_timestamp = 0;
    bool in_cooldown = false;
    int32_t last_ignored_speed = -1; // Track last ignored speed command to avoid repeated logs
    int32_t last_reported_speed = -1; // Track last speed reported over the link to avoid repeated logs

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
            last_reported_speed = 0;
            has_valid_command = false;
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
                    // Check system state before allowing speed commands
                    if (!motor_control_allowed()) {
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
                        // C-5: clamp in the SIGNED type, before narrowing to uint8_t.
                        // Casting first would turn -1 into 255 (full speed) and 300 into 44.
                        if (value < 0)
                        {
                            value = 0;
                        }
                        else if (value > MOTOR_SPEED_MAX)
                        {
                            value = MOTOR_SPEED_MAX;
                        }
                        uint8_t new_speed = (uint8_t)value;

                        current_speed = new_speed;
                        // Ensure forward direction when setting speed
                        motor_set_direction(true);
                        motor_set_speed(current_speed);
                        motor_direction = true;
                        lights_set_reverse(false);
                        // Only print if the applied speed actually changed
                        if ((int32_t)current_speed != last_reported_speed)
                        {
                            last_reported_speed = current_speed;
                            Serial.print("EVENT:CMD_EXECUTED:SET_SPEED:");
                            Serial.println(current_speed);
                            Serial.flush();
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
                    last_reported_speed = 0;
                    motor_direction = true;
                    last_stop_timestamp = current_time;
                    in_cooldown = true;
                    Serial.println("[MotorTask] Motor stopped (brake/stop command), 5 second cooldown started");
                    break;

                default:
                    break;
                }
            }
        }

        // Apply motor control based on state.
        // C-1: FAIL-SAFE. Anything other than "fresh AND authorized command"
        // means brake. The whole loop is guarded by the system state, not only
        // the CMD_SET_SPEED branch, so DISARM/FAULT stop the vehicle for good
        // and an expired command never re-applies the last speed.
        if (in_cooldown || !motor_control_allowed())
        {
            // Ensure motor stays stopped during cooldown / while not authorized
            motor_stop();
            current_speed = 0;
            last_reported_speed = 0;
            motor_direction = true;
            lights_set_reverse(false);
        }
        else if (has_valid_command)
        {
            // Valid command is already applied above, nothing to do here
        }
        else
        {
            // No fresh command in the mailbox (expired, or link lost): brake.
            // NEVER hold the last valid speed - that is what made the vehicle
            // restart by itself 5 s after an emergency stop.
            motor_stop();
            current_speed = 0;
            last_reported_speed = 0;
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
