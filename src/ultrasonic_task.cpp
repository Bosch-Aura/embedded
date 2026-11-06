#include "ultrasonic_task.h"
#include "hardware.h"
#include "supervisor_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "serial_out.h"
#include <Arduino.h>

// Obstacle detection: ONLY VALID READINGS COUNT.
//
//   valid near (< threshold)          -> evidence of an obstacle
//   valid far  (>= threshold + margin) -> evidence of a clear path
//   anything else (no echo, out of range, invalid) -> IGNORED
//
// An invalid reading is not evidence of anything: no echo can be open space,
// an object too close, a soft or angled surface, or a dead sensor. So it never
// brakes, never clears, and never breaks a run of valid readings.
//
//   CLEAR   --N valid near in a row--> BLOCKED   (EVENT:OBSTACLE_DETECTED:<cm>)
//   BLOCKED --N valid far in a row-->  CLEAR     (EVENT:OBSTACLE_CLEARED)
//
// "In a row" means among valid readings: one valid reading of the other kind
// restarts the count, invalid ones in between do not.
//
// Evidence expires: if more than ULTRASONIC_EVIDENCE_MAX_GAP_MS pass between
// two valid readings of a run, the run starts over. Otherwise three spurious
// near readings seconds apart, with silence in between, would add up to a stop.
//
// Readings in the hysteresis band [threshold, threshold + margin) count as far
// while CLEAR and as near while BLOCKED, so an object at the edge does not
// toggle the brake.
//
// BLOCKED holds the car (STOPPED). Since the sensor faces forward, only
// forward motion is blocked: the car can still back away (see ControlTask).
//
// With nothing in range there are no valid far readings, so after the obstacle
// is removed sideways the car would wait forever. M:OBSTACLE_RESET:0 releases
// it; if the obstacle is still there, N near readings block it again.
//
// Sensor health, informational only (never brakes):
//   EVENT:ULTRASONIC_SILENT  no valid reading for ULTRASONIC_SILENT_MS
//   EVENT:ULTRASONIC_OK      first valid reading after that
// Silence can be open space or a dead sensor; the firmware cannot tell which.

#define ULTRASONIC_TASK_PERIOD_MS 50

#ifndef ULTRASONIC_DEBOUNCE_COUNT
#define ULTRASONIC_DEBOUNCE_COUNT 3 // valid readings in a row to brake, and to clear
#endif
#ifndef ULTRASONIC_CLEAR_MARGIN_CM
#define ULTRASONIC_CLEAR_MARGIN_CM 5
#endif
#ifndef ULTRASONIC_EVIDENCE_MAX_GAP_MS
#define ULTRASONIC_EVIDENCE_MAX_GAP_MS 1000
#endif
#ifndef ULTRASONIC_SILENT_MS
#define ULTRASONIC_SILENT_MS 2000
#endif

static volatile bool reset_requested = false;

void ultrasonic_request_reset(void)
{
    reset_requested = true;
}

void ultrasonic_task(void *pvParameters)
{
    (void)pvParameters;
    bool blocked = false;
    uint8_t run = 0;           // valid readings in a row pointing to the OTHER state
    uint32_t last_run_ms = 0;  // time of the last valid reading of the run
    uint32_t last_valid_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    bool silent = false;

    log_line("[Ultrasonic] started");

    while (1)
    {
        const uint16_t cm = ultrasonic_read_cm(); // 0 = no valid reading
        const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (__atomic_exchange_n(&reset_requested, false, __ATOMIC_SEQ_CST))
        {
            event_line("EVENT:CMD_EXECUTED:OBSTACLE_RESET");
            run = 0;
            if (blocked)
            {
                blocked = false;
                supervisor_set_obstacle(false);
                event_line("EVENT:OBSTACLE_CLEARED");
            }
        }

        if (cm != 0)
        {
            last_valid_ms = now;
            if (silent)
            {
                silent = false;
                event_line("EVENT:ULTRASONIC_OK");
            }

            const uint16_t limit = blocked
                ? ULTRASONIC_OBSTACLE_THRESHOLD_CM + ULTRASONIC_CLEAR_MARGIN_CM
                : ULTRASONIC_OBSTACLE_THRESHOLD_CM;
            const bool near = cm < limit;

            if (near == blocked)
            {
                run = 0; // a valid reading confirming the current state
            }
            else
            {
                if (run > 0 && now - last_run_ms > ULTRASONIC_EVIDENCE_MAX_GAP_MS)
                {
                    run = 0; // stale evidence: start over with this reading
                }
                run++;
                last_run_ms = now;
            }

            if (run >= ULTRASONIC_DEBOUNCE_COUNT)
            {
                blocked = near;
                run = 0;
                supervisor_set_obstacle(blocked);
                if (blocked)
                {
                    event_linef("EVENT:OBSTACLE_DETECTED:%u", (unsigned)cm);
                }
                else
                {
                    event_line("EVENT:OBSTACLE_CLEARED");
                }
            }
        }
        else if (!silent && now - last_valid_ms >= ULTRASONIC_SILENT_MS)
        {
            silent = true;
            event_line("EVENT:ULTRASONIC_SILENT");
        }

        vTaskDelay(pdMS_TO_TICKS(ULTRASONIC_TASK_PERIOD_MS));
    }
}
