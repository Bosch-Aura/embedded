#include "supervisor_task.h"
#include "control_task.h"
#include "hardware.h"
#include "serial_out.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>

#define SUPERVISOR_TASK_PERIOD_MS 20

typedef enum { REQ_NONE = 0, REQ_ARM, REQ_DISARM } request_t;

// volatile: written on one core and read in a loop on the other. Without it
// the compiler may hoist the load out of ControlTask's while(1).
//
// Boot state: DISARMED, nothing moves until M:SYS_ARM:0. -DBOOT_ARMED boots
// straight into RUNNING and obeys the first C:SET_SPEED. The watchdog only
// starts counting at the first command, so an armed board that never hears
// from the Jetson stays RUNNING (and still does not move: nothing is latched).
#ifdef BOOT_ARMED
static volatile system_state_t current_state = STATE_RUNNING;
#else
static volatile system_state_t current_state = STATE_DISARMED;
#endif
static volatile uint32_t last_heartbeat_ms = 0; // 0 = no command yet
static volatile int pending_request = REQ_NONE;
static volatile bool obstacle_active = false;
static volatile bool stop_latched = false;

static uint32_t now_ms(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

// Heartbeat age, measured against a clock read AFTER loading the heartbeat.
// Reading the clock first and the heartbeat later (LinkRxTask can store a newer
// one in between) makes the unsigned subtraction wrap to ~4e9 and the watchdog
// fire right after every SYS_ARM.
static bool heartbeat_age_ms(uint32_t *age_ms)
{
    const uint32_t heartbeat = last_heartbeat_ms;
    if (heartbeat == 0)
    {
        return false;
    }
    const uint32_t now = now_ms();
    *age_ms = (now >= heartbeat) ? (now - heartbeat) : 0;
    return true;
}

static void set_state(system_state_t s)
{
    if (s == current_state)
    {
        return;
    }
    current_state = s;
    event_linef("EVENT:STATE_CHANGED:%s", supervisor_state_name(s));
    // Brake / center on this very cycle instead of waiting up to 10 ms.
    control_wake();
}

static void handle_request(request_t req, bool estop)
{
    switch (req)
    {
    case REQ_ARM:
        if (current_state == STATE_DISARMED || current_state == STATE_FAULT)
        {
            if (estop)
            {
                event_line("EVENT:CMD_REJECTED:SYS_ARM:ESTOP_ACTIVE");
                return;
            }
            supervisor_feed_watchdog(); // fresh start for the watchdog
            set_state(supervisor_stop_requested() ? STATE_STOPPED : STATE_RUNNING);
        }
        // Already armed: confirm anyway so the client does not hang waiting.
        event_line("EVENT:CMD_EXECUTED:SYS_ARM");
        break;
    case REQ_DISARM:
        if (current_state != STATE_DISARMED)
        {
            set_state(STATE_DISARMED);
            event_line("EVENT:CMD_EXECUTED:SYS_DISARM");
        }
        break;
    default:
        break;
    }
}

void supervisor_task(void *pvParameters)
{
    (void)pvParameters;
    bool estop_was = false;

    log_line("[Supervisor] started");
    event_linef("EVENT:STATE_CHANGED:%s", supervisor_state_name(current_state));

    while (1)
    {
        const bool estop = estop_is_triggered();

        // Consume the pending request atomically: a new one arriving between a
        // plain read and the clear would be lost.
        const request_t req = (request_t)__atomic_exchange_n(&pending_request, REQ_NONE, __ATOMIC_SEQ_CST);
        handle_request(req, estop);

        if (estop && !estop_was)
        {
            event_line("EVENT:ESTOP_TRIGGERED:GPIO");
            set_state(STATE_FAULT);
        }
        else if (!estop && estop_was)
        {
            event_line("EVENT:ESTOP_RELEASED");
        }
        estop_was = estop;

        // Watchdog: covers every armed state.
        uint32_t age;
        if ((current_state == STATE_RUNNING || current_state == STATE_STOPPED) &&
            heartbeat_age_ms(&age) && age > WATCHDOG_TIMEOUT_MS)
        {
            event_line("EVENT:WATCHDOG_TIMEOUT");
            log_linef("[Supervisor] watchdog: heartbeat age %lu ms", (unsigned long)age);
            set_state(STATE_FAULT);
        }

        // RUNNING <-> STOPPED follows the stop requests. ControlTask already
        // reads supervisor_stop_requested() directly; this makes it visible.
        const bool stop = supervisor_stop_requested();
        if (current_state == STATE_RUNNING && stop)
        {
            log_linef("[Supervisor] STOPPED by %s", stop_latched ? "SYS_STOP" : "OBSTACLE");
            set_state(STATE_STOPPED);
        }
        else if (current_state == STATE_STOPPED && !stop)
        {
            set_state(STATE_RUNNING);
        }

        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_TASK_PERIOD_MS));
    }
}

void supervisor_feed_watchdog(void) { last_heartbeat_ms = now_ms(); }
void supervisor_request_arm(void) { pending_request = REQ_ARM; }
void supervisor_request_disarm(void) { pending_request = REQ_DISARM; }
// Both stop requests wake ControlTask so it brakes now, not on its next cycle.
void supervisor_set_obstacle(bool obstacle)
{
    obstacle_active = obstacle;
    control_wake();
}
bool supervisor_stop_requested(void) { return obstacle_active || stop_latched; }
system_state_t supervisor_get_state(void) { return current_state; }

// Only called from LinkRxTask, so the read-then-write is not racy.
bool supervisor_latch_stop(void)
{
    const bool changed = !stop_latched;
    stop_latched = true;
    control_wake();
    return changed;
}

bool supervisor_release_stop(void)
{
    const bool changed = stop_latched;
    stop_latched = false;
    return changed;
}

const char *supervisor_state_name(system_state_t state)
{
    switch (state)
    {
    case STATE_DISARMED: return "DISARMED";
    case STATE_RUNNING:  return "RUNNING";
    case STATE_STOPPED:  return "STOPPED";
    default:             return "FAULT";
    }
}
