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
// BLOQUEANTE-2: dead time between cutting PWM and flipping IN3/IN4. Reversing
// an energised H-bridge (plugging) draws roughly twice the stall current.
#define MOTOR_DIRECTION_DEADTIME_MS 60

// Direction the link/web asked for. Owned by MotorTask, which is the only task
// allowed to touch the H-bridge pins; everyone else goes through
// motor_set_requested_direction().
static volatile bool requested_direction = true; // true = forward

static mailbox_t *motor_mailbox = NULL;
static TaskHandle_t motor_task_handle = NULL;

void motor_set_requested_direction(bool forward)
{
    requested_direction = forward;
}

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
    // -1 = unknown/braked (IN3=IN4=LOW), 0 = backward, 1 = forward.
    // "Unknown" matters: after motor_stop() both pins are LOW, so the direction
    // must be re-applied before the motor can turn again.
    int8_t applied_dir = -1;
    uint32_t dir_change_ts = 0;
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
            applied_dir = -1; // motor_stop() cleared IN3/IN4
            dir_change_ts = 0;
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

                        // BLOQUEANTE-2: honour the requested direction instead
                        // of forcing forward on every 10 ms cycle. Flipping
                        // IN3/IN4 with PWM applied is plugging: the web UI
                        // resending "backward" at 10 Hz was inverting the
                        // H-bridge ten times a second under load.
                        const int8_t want_dir = requested_direction ? 1 : 0;

                        if (applied_dir == want_dir)
                        {
                            dir_change_ts = 0;
                            motor_set_speed(current_speed);
                            // Only print if the applied speed actually changed
                            if ((int32_t)current_speed != last_reported_speed)
                            {
                                last_reported_speed = current_speed;
                                Serial.print("EVENT:CMD_EXECUTED:SET_SPEED:");
                                Serial.println(current_speed);
                                Serial.flush();
                            }
                        }
                        else if (applied_dir < 0)
                        {
                            // Coming from a braked state: IN3/IN4 are already
                            // LOW and PWM is 0, so switching is safe right now.
                            motor_set_direction(want_dir == 1);
                            applied_dir = want_dir;
                            lights_set_reverse(want_dir == 0);
                            dir_change_ts = 0;
                        }
                        else
                        {
                            // Reversal while energised: cut PWM, wait out the
                            // dead time, and only then switch. Non-blocking so
                            // the emergency notification keeps being polled.
                            motor_set_speed(0);
                            if (dir_change_ts == 0)
                            {
                                dir_change_ts = current_time;
                            }
                            else if ((current_time - dir_change_ts) >= MOTOR_DIRECTION_DEADTIME_MS)
                            {
                                motor_set_direction(want_dir == 1);
                                applied_dir = want_dir;
                                lights_set_reverse(want_dir == 0);
                                dir_change_ts = 0;
                            }
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
                    applied_dir = -1; // motor_stop() cleared IN3/IN4
                    dir_change_ts = 0;
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
            applied_dir = -1;
            dir_change_ts = 0;
            lights_set_reverse(false);
        }
        else if (has_valid_command)
        {
            // Valid command is already applied above, nothing to do here
        }
        else
        {
            // LATCHING: hold the last commanded speed instead of braking, so a
            // single C:SET_SPEED keeps the vehicle moving until told otherwise.
            //
            // This is only safe because of the branch above and the watchdog:
            //   - Losing the link stops the M:PING:0 stream, the watchdog
            //     latches FAULT, motor_control_allowed() goes false, and the
            //     branch above brakes and clears current_speed.
            //   - DISARM, E-STOP, ultrasonic and cooldown all land in that same
            //     branch, which zeroes current_speed - so there is nothing left
            //     to latch and the vehicle cannot restart on its own.
            // Without the watchdog covering BOTH modes and without FAULT being
            // sticky, this branch would be the C-1 bug all over again.
            if (applied_dir >= 0)
            {
                motor_set_speed(current_speed);
            }
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
