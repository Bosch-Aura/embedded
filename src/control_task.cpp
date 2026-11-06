#include "control_task.h"
#include "hardware.h"
#include "supervisor_task.h"
#include "serial_out.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>

#define CONTROL_TASK_PERIOD_MS 10 // 100 Hz

// Dead time between cutting PWM and flipping IN3/IN4. Reversing an energised
// H-bridge (plugging) draws roughly twice the stall current.
#ifndef MOTOR_DIRECTION_DEADTIME_MS
#define MOTOR_DIRECTION_DEADTIME_MS 60
#endif

// ---------------------------------------------------------------------------
// Commands from LinkRxTask (core 1) to ControlTask (core 0).
// A sequence number per command marks it as new; ControlTask consumes it once.
// ---------------------------------------------------------------------------
typedef struct {
    int32_t speed;
    uint32_t speed_seq;
    int32_t angle;
    uint32_t steer_seq;
    bool forward;
    bool brake;
} commands_t;

static commands_t cmds = {0, 0, SERVO_CENTER, 0, true, false};
static portMUX_TYPE cmds_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t control_handle = NULL;

void control_set_speed(int32_t speed)
{
    taskENTER_CRITICAL(&cmds_lock);
    cmds.speed = speed;
    cmds.speed_seq++;
    taskEXIT_CRITICAL(&cmds_lock);
}

void control_set_steer(int32_t angle)
{
    taskENTER_CRITICAL(&cmds_lock);
    cmds.angle = angle;
    cmds.steer_seq++;
    taskEXIT_CRITICAL(&cmds_lock);
}

void control_set_direction(bool forward)
{
    taskENTER_CRITICAL(&cmds_lock);
    cmds.forward = forward;
    taskEXIT_CRITICAL(&cmds_lock);
}

void control_brake(void)
{
    taskENTER_CRITICAL(&cmds_lock);
    cmds.brake = true;
    taskEXIT_CRITICAL(&cmds_lock);
    control_wake();
}

void control_wake(void)
{
    if (control_handle != NULL)
    {
        xTaskNotifyGive(control_handle);
    }
}

// Clamp in the SIGNED type, before narrowing: casting first turns -1 into 255
// (full throttle) and -30 into 65506 (full right lock on a left command).
static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
// Traction. applied_dir: -1 braked (IN3=IN4=LOW), 0 backward, 1 forward.
// ---------------------------------------------------------------------------
static int8_t applied_dir = -1;
static bool reversing = false;
static uint32_t reverse_since = 0;

static void traction_brake(void)
{
    motor_stop();
    applied_dir = -1;
    reversing = false;
}

// Applies `speed` in direction `forward`, never flipping the bridge while
// energised. Non-blocking: during the dead time the loop keeps running, so a
// brake or a state change is still handled on the next cycle.
static void traction_drive(uint8_t speed, bool forward, uint32_t now)
{
    if (speed == 0)
    {
        traction_brake();
        return;
    }
    const int8_t want = forward ? 1 : 0;
    if (applied_dir == want)
    {
        reversing = false;
        motor_set_speed(speed);
    }
    else if (applied_dir < 0)
    {
        // Braked: pins LOW, PWM 0, so switching is safe right now.
        motor_set_direction(forward);
        applied_dir = want;
        motor_set_speed(speed);
    }
    else
    {
        motor_set_speed(0);
        if (!reversing)
        {
            reversing = true;
            reverse_since = now;
        }
        else if (now - reverse_since >= MOTOR_DIRECTION_DEADTIME_MS)
        {
            motor_set_direction(forward);
            applied_dir = want;
            reversing = false;
            motor_set_speed(speed);
        }
    }
}

void control_task(void *pvParameters)
{
    (void)pvParameters;
    control_handle = xTaskGetCurrentTaskHandle();

    uint8_t speed = 0;            // latched speed: held until changed or cleared
    uint16_t angle = SERVO_CENTER; // what the servo is at
    uint32_t seen_speed_seq = 0;
    uint32_t seen_steer_seq = 0;
    bool ignored_logged = false;
    int32_t pending_angle = SERVO_CENTER;
    bool steer_pending = false;

    log_line("[Control] started");

    while (1)
    {
        const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        taskENTER_CRITICAL(&cmds_lock);
        const commands_t c = cmds;
        cmds.brake = false;
        taskEXIT_CRITICAL(&cmds_lock);

        const bool new_speed = c.speed_seq != seen_speed_seq;
        const bool new_steer = c.steer_seq != seen_steer_seq;
        seen_speed_seq = c.speed_seq;
        seen_steer_seq = c.steer_seq;

        const system_state_t state = supervisor_get_state();
        const bool armed = (state == STATE_RUNNING || state == STATE_STOPPED);
        // The stop flag is read directly (not via the STOPPED state) so the
        // brake does not wait for the next supervisor cycle.
        const bool can_move = armed && !supervisor_stop_requested();

        // ---- Traction --------------------------------------------------
        if (c.brake)
        {
            event_line("EVENT:CMD_EXECUTED:BRAKE_NOW");
            speed = 0;
        }

        if (new_speed)
        {
            if (can_move)
            {
                const uint8_t s = (uint8_t)clamp(c.speed, 0, MOTOR_SPEED_MAX);
                if (s != speed)
                {
                    event_linef("EVENT:CMD_EXECUTED:SET_SPEED:%u", (unsigned)s);
                }
                speed = s;
                ignored_logged = false;
            }
            else if (!ignored_logged)
            {
                log_linef("[Control] SET_SPEED ignored (%s)", supervisor_state_name(state));
                ignored_logged = true;
            }
        }

        if (can_move)
        {
            traction_drive(speed, c.forward, now);
        }
        else
        {
            // Fail-safe: anything but "armed and not stopped" brakes AND clears
            // the latched speed, so the car never restarts by itself when the
            // stop clears. The Jetson has to send a new C:SET_SPEED.
            speed = 0;
            traction_brake();
        }

        // ---- Steering --------------------------------------------------
        // Not armed: center, and forget any pending command.
        // Stopped: freeze where it is, but KEEP the last angle asked for and
        // apply it on resume. The Jetson only sends C:SET_STEER when the angle
        // changes, so one dropped while stopped would never be re-sent.
        // Moving the servo on resume is harmless: speed is 0 until a new
        // C:SET_SPEED arrives.
        if (new_steer)
        {
            pending_angle = c.angle;
            steer_pending = true;
        }
        uint16_t target = angle;
        if (!armed)
        {
            target = SERVO_CENTER;
            steer_pending = false;
        }
        else if (can_move && steer_pending)
        {
            target = (uint16_t)clamp(pending_angle, SERVO_LEFT, SERVO_RIGHT);
            steer_pending = false;
        }
        if (target != angle)
        {
            angle = target;
            steer_set_angle(angle);
            event_linef("EVENT:CMD_EXECUTED:SET_STEER:%u", (unsigned)angle);
        }

        // Sleeps one period, or less if control_wake() is called.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONTROL_TASK_PERIOD_MS));
    }
}
