#include "control_task.h"
#include "hardware.h"
#include "supervisor_task.h"
#include "serial_out.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>
#include <string.h>

#define CONTROL_TASK_PERIOD_MS 10 // 100 Hz

// Dead time between cutting PWM and flipping IN3/IN4. Reversing an energised
// H-bridge (plugging) draws roughly twice the stall current.
#ifndef MOTOR_DIRECTION_DEADTIME_MS
#define MOTOR_DIRECTION_DEADTIME_MS 60
#endif

// Where the ultrasonic sensor looks. An obstacle blocks only motion toward it,
// so the car can back away from something in front. 0 = sensor at the rear.
#ifndef ULTRASONIC_FACING_FORWARD
#define ULTRASONIC_FACING_FORWARD 1
#endif

// CMD_REJECTED:SET_SPEED is sent for every DIFFERENT speed (or reason) that is
// rejected, at most once per this interval. A rejection that falls inside the
// interval is sent when it ends, so the last one is never lost.
#ifndef REJECT_REPORT_MIN_INTERVAL_MS
#define REJECT_REPORT_MIN_INTERVAL_MS 200
#endif

// ---------------------------------------------------------------------------
// Commands from LinkRxTask / SupervisorTask (core 1) to ControlTask (core 0).
// A sequence number per command marks it as new; ControlTask consumes it once.
//
// A brake goes through the SAME slot as the speed (speed 0, seq++), so the
// order of arrival decides: SET_SPEED then BRAKE_NOW ends braked, BRAKE_NOW
// then SET_SPEED ends moving. A separate brake flag lost the brake when a
// SET_SPEED arrived just before it in the same cycle.
// ---------------------------------------------------------------------------
typedef struct {
    int32_t speed;
    uint32_t speed_seq;
    int32_t angle;
    uint32_t steer_seq;
    bool forward;
    uint32_t brake_events; // E:BRAKE_NOW received, to confirm them after acting
    uint32_t forget_held;  // M:OBSTACLE_RESET received: drop the resume speed
} commands_t;

static commands_t cmds = {0, 0, SERVO_CENTER, 0, true, 0, 0};
static portMUX_TYPE cmds_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t volatile control_handle = NULL;
// Copies of ControlTask's speed and obstacle-hold speed, for M:STATUS.
static volatile uint8_t status_speed = 0;
static volatile uint8_t status_held = 0;

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

static void drop_speed(bool confirm)
{
    taskENTER_CRITICAL(&cmds_lock);
    cmds.speed = 0;
    cmds.speed_seq++;
    if (confirm)
    {
        cmds.brake_events++;
    }
    taskEXIT_CRITICAL(&cmds_lock);
    control_wake();
}

void control_brake(void) { drop_speed(true); }
void control_drop_speed(void) { drop_speed(false); }

void control_forget_held_speed(void)
{
    taskENTER_CRITICAL(&cmds_lock);
    cmds.forget_held++;
    taskEXIT_CRITICAL(&cmds_lock);
    control_wake();
}

uint8_t control_latched_speed(void) { return status_speed; }
uint8_t control_held_speed(void) { return status_held; }

void control_wake(void)
{
    TaskHandle_t h = control_handle;
    if (h != NULL)
    {
        xTaskNotifyGive(h);
    }
}

// Clamp in the SIGNED type, before narrowing: casting first turns -1 into 255
// (full throttle) and -30 into 65506 (full right lock on a left command).
static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
// Traction.
//   applied_dir: what IN3/IN4 are now: -1 braked (both LOW), 0 back, 1 forward.
//   last_dir:    the last direction the motor was DRIVEN in (-1 = never).
//   pwm_off_ms:  when the PWM was last cut after driving.
//
// The dead time is measured from the last time the bridge was energised the
// OTHER way, whatever happened in between. Braking (SET_SPEED:0, BRAKE_NOW, an
// obstacle) leaves the motor spinning by inertia, so "braked" is not "safe to
// reverse": checking only the current pins let a reverse through 10-20 ms
// after the brake.
// ---------------------------------------------------------------------------
static int8_t applied_dir = -1;
static int8_t last_dir = -1;
static bool pwm_on = false;
static uint32_t pwm_off_ms = 0;

static void pwm_cut(uint32_t now)
{
    if (pwm_on)
    {
        pwm_on = false;
        pwm_off_ms = now;
    }
}

static void traction_brake(uint32_t now)
{
    motor_stop();
    pwm_cut(now);
    applied_dir = -1;
}

// Applies `speed` in direction `forward`. Non-blocking: during the dead time
// the loop keeps running, so a brake or a state change is still handled on the
// next cycle.
static void traction_drive(uint8_t speed, bool forward, uint32_t now)
{
    if (speed == 0)
    {
        traction_brake(now);
        return;
    }
    const int8_t want = forward ? 1 : 0;
    if (applied_dir == want)
    {
        motor_set_speed(speed);
        pwm_on = true;
        last_dir = want;
        return;
    }
    if (last_dir >= 0 && last_dir != want)
    {
        // Reversing with respect to the last motion: cut the PWM (keep the
        // pins as they are) and wait out the dead time.
        if (pwm_on)
        {
            motor_set_speed(0);
            pwm_cut(now);
            return;
        }
        if (now - pwm_off_ms < MOTOR_DIRECTION_DEADTIME_MS)
        {
            return;
        }
    }
    motor_set_direction(forward);
    applied_dir = want;
    motor_set_speed(speed);
    pwm_on = true;
    last_dir = want;
}

void control_task(void *pvParameters)
{
    (void)pvParameters;
    control_handle = xTaskGetCurrentTaskHandle();

    uint8_t speed = 0;            // latched speed: held until changed or cleared
    uint16_t angle = SERVO_CENTER; // what the servo is at
    uint32_t seen_speed_seq = 0;
    uint32_t seen_steer_seq = 0;
    uint32_t seen_brake_events = 0;
    uint32_t seen_forget_held = 0;
    // Obstacle hold: the speed the car had (or was last told) when the hold
    // braked it. Restored by itself when the obstacle clears; 0 = nothing to
    // resume. Any other stop, a new applied speed or OBSTACLE_RESET drops it.
    uint8_t held_speed = 0;
    // CMD_REJECTED:SET_SPEED bookkeeping, see REJECT_REPORT_MIN_INTERVAL_MS.
    int32_t reported_speed = 0;  // last rejection sent (0 = none this episode)
    const char *reported_reason = "";
    bool reject_pending = false; // a different rejection waits to be sent
    int32_t pending_speed = 0;
    const char *pending_reason = "";
    bool reject_sent_once = false;
    uint32_t last_reject_ms = 0;
    int32_t pending_angle = SERVO_CENTER;
    bool steer_pending = false;

    log_line("[Control] started");

    while (1)
    {
        const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // Read BEFORE the command snapshot: LinkRx flags OBSTACLE_RESET
        // (forget_held) before asking UltrasonicTask to release, so seeing the
        // release here guarantees the snapshot below also sees the forget.
        const bool obstacle = supervisor_obstacle_active();

        taskENTER_CRITICAL(&cmds_lock);
        const commands_t c = cmds;
        taskEXIT_CRITICAL(&cmds_lock);

        const bool new_speed = c.speed_seq != seen_speed_seq;
        const bool new_steer = c.steer_seq != seen_steer_seq;
        const bool braked_now = c.brake_events != seen_brake_events;
        const bool forget_held = c.forget_held != seen_forget_held;
        seen_speed_seq = c.speed_seq;
        seen_steer_seq = c.steer_seq;
        seen_brake_events = c.brake_events;
        seen_forget_held = c.forget_held;

        const system_state_t state = supervisor_get_state();
        const bool armed = (state == STATE_RUNNING || state == STATE_STOPPED);
        // The stop flags are read directly (not via the STOPPED state) so the
        // brake does not wait for the next supervisor cycle.
        //   SYS_STOP latch: nothing moves, servo frozen.
        //   obstacle:       only motion toward the sensor is blocked.
        const bool latched = supervisor_stop_latched();
        const bool toward_sensor = (c.forward == (ULTRASONIC_FACING_FORWARD != 0));
        const bool toward_obstacle = obstacle && toward_sensor;
        const bool can_move = armed && !latched && !toward_obstacle;
        const bool can_steer = armed && !latched;
        // Braked ONLY by the obstacle: the one stop that resumes by itself.
        const bool obstacle_hold = armed && !latched && toward_obstacle;

#ifdef ULTRASONIC_DEBUG
        static bool was_toward_obstacle = false;
        if (toward_obstacle != was_toward_obstacle)
        {
            was_toward_obstacle = toward_obstacle;
            if (toward_obstacle)
                log_linef("[Control] >>> MOTOR FRENADO por obstaculo (velocidad %u -> 0)", (unsigned)speed);
            else
                log_linef("[Control] >>> obstaculo liberado: retoma a la velocidad guardada (%u)", (unsigned)held_speed);
        }
#endif

        // ---- Traction: act first, report after ------------------------
        const uint8_t old_speed = speed;
        const uint8_t old_held = held_speed;
        bool resumed = false;
        if (forget_held)
        {
            held_speed = 0; // OBSTACLE_RESET: a manual release never resumes
        }
        if (!can_move)
        {
            // Fail-safe: brake. An obstacle hold remembers the speed so the car
            // resumes by itself when the path clears. Any other stop (not
            // armed, SYS_STOP) clears it: the Jetson has to send a new
            // C:SET_SPEED. Backing away from an obstacle is allowed.
            if (!obstacle_hold)
            {
                held_speed = 0;
            }
            else if (speed > 0)
            {
                held_speed = speed;
            }
            speed = 0;
        }
        if (new_speed)
        {
            const uint8_t v = (uint8_t)clamp(c.speed, 0, MOTOR_SPEED_MAX);
            if (can_move)
            {
                speed = v;
                held_speed = 0; // a speed actually applied replaces the memory
            }
            else if (obstacle_hold)
            {
                held_speed = v; // the latest command is what resumes (0 cancels)
            }
            else if (c.speed > 0)
            {
                const char *reason = !armed ? supervisor_state_name(state) : "SYS_STOP";
                // The same speed for the same reason again says nothing new.
                reject_pending = c.speed != reported_speed || strcmp(reason, reported_reason) != 0;
                pending_speed = c.speed;
                pending_reason = reason;
            }
        }
        if (can_move)
        {
            // Episode over: the next rejection is reported whatever its value.
            reported_speed = 0;
            reported_reason = "";
            reject_pending = false;
            if (held_speed > 0)
            {
                // The hold is over. Resume only toward the sensor, i.e. because
                // the obstacle cleared; if the Jetson turned the car around,
                // backing away needs its own C:SET_SPEED.
                if (toward_sensor)
                {
                    speed = held_speed;
                    resumed = true;
                }
                held_speed = 0;
            }
            traction_drive(speed, c.forward, now);
        }
        else
        {
            traction_brake(now);
        }

        if (braked_now)
        {
            event_line("EVENT:CMD_EXECUTED:BRAKE_NOW");
        }
        if (obstacle_hold && held_speed != old_held)
        {
            event_linef("EVENT:OBSTACLE_HOLD:%u", (unsigned)held_speed);
        }
        if (resumed)
        {
            event_linef("EVENT:OBSTACLE_RESUMED:%u", (unsigned)speed);
        }
        if (speed != old_speed && can_move)
        {
            event_linef("EVENT:CMD_EXECUTED:SET_SPEED:%u", (unsigned)speed);
        }
        status_speed = speed;
        status_held = held_speed;
        if (reject_pending &&
            (!reject_sent_once || now - last_reject_ms >= REJECT_REPORT_MIN_INTERVAL_MS))
        {
            event_linef("EVENT:CMD_REJECTED:SET_SPEED:%s", pending_reason);
            reported_speed = pending_speed;
            reported_reason = pending_reason;
            reject_pending = false;
            reject_sent_once = true;
            last_reject_ms = now;
        }

        // ---- Steering --------------------------------------------------
        // Not armed: center, and forget any pending command.
        // SYS_STOP: freeze where it is, but KEEP the last angle asked for and
        // apply it on resume (the Jetson only sends C:SET_STEER when the angle
        // changes, so one dropped while stopped would never be re-sent).
        // An obstacle does not freeze the servo: the car may be steering while
        // it backs away.
        // Moving the servo on resume is harmless: after SYS_STOP the speed is 0
        // until a new C:SET_SPEED arrives.
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
        else if (can_steer && steer_pending)
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
