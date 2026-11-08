#include "supervisor_task.h"
#include "control_task.h"
#include "hardware.h"
#include "serial_out.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>

#define SUPERVISOR_TASK_PERIOD_MS 20

// A FAULT caused by the watchdog clears by itself once the heartbeat has been
// stable (no gap longer than WATCHDOG_TIMEOUT_MS) for this long. A FAULT
// caused by the E-STOP still needs M:SYS_ARM:0.
#ifndef WATCHDOG_RECOVERY_MS
#define WATCHDOG_RECOVERY_MS 500
#endif

typedef enum { FAULT_NONE = 0, FAULT_WATCHDOG, FAULT_ESTOP } fault_cause_t;

typedef enum { REQ_NONE = 0, REQ_ARM, REQ_DISARM } request_t;

// SYS_ARM / SYS_DISARM requests from LinkRxTask, handled IN ORDER. A single
// "last one wins" slot let DISARM followed by ARM within one cycle skip the
// DISARM entirely: no brake, latched speed kept, car still driving.
#define REQUEST_QUEUE_LEN 8
static request_t requests[REQUEST_QUEUE_LEN];
static uint8_t req_head = 0, req_count = 0;
static portMUX_TYPE req_lock = portMUX_INITIALIZER_UNLOCKED;

static void push_request(request_t r)
{
    taskENTER_CRITICAL(&req_lock);
    if (req_count == REQUEST_QUEUE_LEN)
    {
        // Full (a flood of ARM/DISARM): drop the oldest, keep the newest.
        req_head = (req_head + 1) % REQUEST_QUEUE_LEN;
        req_count--;
    }
    requests[(req_head + req_count) % REQUEST_QUEUE_LEN] = r;
    req_count++;
    taskEXIT_CRITICAL(&req_lock);
}

static request_t pop_request(void)
{
    request_t r = REQ_NONE;
    taskENTER_CRITICAL(&req_lock);
    if (req_count > 0)
    {
        r = requests[req_head];
        req_head = (req_head + 1) % REQUEST_QUEUE_LEN;
        req_count--;
    }
    taskEXIT_CRITICAL(&req_lock);
    return r;
}

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
static volatile uint32_t last_heartbeat_ms = 0;
static volatile bool have_heartbeat = false; // false until the first command
static volatile bool obstacle_active = false;
static volatile bool stop_latched = false;

// Only touched by SupervisorTask.
static fault_cause_t fault_cause = FAULT_NONE;
static bool recovering = false;         // heartbeat stable since recovery_start_ms
static uint32_t recovery_start_ms = 0;
static const char *stopped_by_sent = NULL; // last EVENT:STOPPED_BY reason sent

static uint32_t now_ms(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

// Heartbeat age, measured against a clock read AFTER loading the heartbeat.
// Reading the clock first and the heartbeat later (LinkRxTask can store a newer
// one in between) makes the unsigned subtraction wrap to ~4e9 and the watchdog
// fire right after every SYS_ARM. With that order, plain unsigned subtraction
// is also correct across the 32-bit tick wrap (~49.7 days); clamping
// "now < heartbeat" to 0 blinded the watchdog for another 49 days there.
static bool heartbeat_age_ms(uint32_t *age_ms)
{
    // acquire pairs with the release in supervisor_feed_watchdog(): once
    // have_heartbeat is seen true, last_heartbeat_ms is the stored value.
    if (!__atomic_load_n(&have_heartbeat, __ATOMIC_ACQUIRE))
    {
        return false;
    }
    const uint32_t heartbeat = __atomic_load_n(&last_heartbeat_ms, __ATOMIC_ACQUIRE);
    const uint32_t now = now_ms();
    *age_ms = now - heartbeat;
    return true;
}

// Why the car is STOPPED. SYS_STOP wins: it blocks everything, the obstacle
// only blocks motion toward the sensor. NULL if nothing holds it.
static const char *stop_cause_name(void)
{
    return stop_latched ? "SYS_STOP" : obstacle_active ? "OBSTACLE" : NULL;
}

// EVENT:STOPPED_BY:<cause>. force: send it even if it did not change.
static void report_stopped_by(bool force)
{
    const char *by = stop_cause_name();
    if (by == NULL || (!force && by == stopped_by_sent))
    {
        return;
    }
    stopped_by_sent = by;
    event_linef("EVENT:STOPPED_BY:%s", by);
}

static void set_state(system_state_t s)
{
    if (s == current_state)
    {
        return;
    }
    current_state = s;
    if (s != STATE_FAULT)
    {
        fault_cause = FAULT_NONE;
        recovering = false;
    }
    if (s == STATE_DISARMED || s == STATE_FAULT)
    {
        // Drop the latched speed explicitly, not only through the state: a
        // re-ARM in the same cycle could otherwise come before ControlTask
        // ever sees DISARMED/FAULT, and the old speed would still be there.
        control_drop_speed();
    }
    event_linef("EVENT:STATE_CHANGED:%s", supervisor_state_name(s));
    if (s == STATE_STOPPED)
    {
        report_stopped_by(true);
    }
    // Brake / center on this very cycle instead of waiting up to 10 ms.
    control_wake();
}

static void enter_fault(fault_cause_t cause)
{
    // Set the cause even if already in FAULT: an E-STOP during a watchdog
    // FAULT must not be cleared by the heartbeat coming back.
    fault_cause = cause;
    recovering = false;
    set_state(STATE_FAULT);
}

// FAULT by watchdog -> RUNNING/STOPPED once the heartbeat is stable again.
static void try_watchdog_recovery(void)
{
    if (current_state != STATE_FAULT || fault_cause != FAULT_WATCHDOG)
    {
        return;
    }
    uint32_t age;
    if (!heartbeat_age_ms(&age) || age > WATCHDOG_TIMEOUT_MS)
    {
        recovering = false;
        return;
    }
    const uint32_t now = now_ms();
    if (!recovering)
    {
        recovering = true;
        recovery_start_ms = now;
        return;
    }
    if (now - recovery_start_ms < WATCHDOG_RECOVERY_MS)
    {
        return;
    }
    // Speed 0 again, in case a C:SET_SPEED sent during the FAULT is still in
    // the slot: the car moves only with a C:SET_SPEED sent after this.
    control_drop_speed();
    event_line("EVENT:WATCHDOG_RECOVERED");
    set_state(supervisor_stop_requested() ? STATE_STOPPED : STATE_RUNNING);
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
        else if (current_state == STATE_STOPPED)
        {
            // Already STOPPED: say why again, the client may have just connected.
            report_stopped_by(true);
        }
        // Already armed: confirm anyway so the client does not hang waiting.
        event_line("EVENT:CMD_EXECUTED:SYS_ARM");
        break;
    case REQ_DISARM:
        set_state(STATE_DISARMED);
        event_line("EVENT:CMD_EXECUTED:SYS_DISARM"); // also if already disarmed
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

        for (request_t req = pop_request(); req != REQ_NONE; req = pop_request())
        {
            handle_request(req, estop);
        }

        if (estop && !estop_was)
        {
            event_line("EVENT:ESTOP_TRIGGERED:GPIO");
            enter_fault(FAULT_ESTOP);
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
            enter_fault(FAULT_WATCHDOG);
        }
        if (!estop)
        {
            try_watchdog_recovery();
        }

        // RUNNING <-> STOPPED follows the stop requests. ControlTask already
        // reads supervisor_stop_requested() directly; this makes it visible.
        const bool stop = supervisor_stop_requested();
        if (current_state == STATE_RUNNING && stop)
        {
            set_state(STATE_STOPPED); // also sends EVENT:STOPPED_BY
        }
        else if (current_state == STATE_STOPPED && !stop)
        {
            set_state(STATE_RUNNING);
        }
        else if (current_state == STATE_STOPPED)
        {
            // Still STOPPED, but the cause may have changed (the obstacle
            // cleared while SYS_STOP is latched, or the other way around).
            report_stopped_by(false);
        }

        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_TASK_PERIOD_MS));
    }
}

void supervisor_feed_watchdog(void)
{
    __atomic_store_n(&last_heartbeat_ms, now_ms(), __ATOMIC_RELEASE);
    __atomic_store_n(&have_heartbeat, true, __ATOMIC_RELEASE);
}
void supervisor_request_arm(void) { push_request(REQ_ARM); }
void supervisor_request_disarm(void)
{
    // Stop now; the state change follows on the next supervisor cycle.
    control_drop_speed();
    push_request(REQ_DISARM);
}
// Both stop requests wake ControlTask so it brakes now, not on its next cycle.
void supervisor_set_obstacle(bool obstacle)
{
    obstacle_active = obstacle;
    control_wake();
}
bool supervisor_stop_requested(void) { return obstacle_active || stop_latched; }
bool supervisor_obstacle_active(void) { return obstacle_active; }
bool supervisor_stop_latched(void) { return stop_latched; }
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
