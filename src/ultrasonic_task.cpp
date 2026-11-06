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
// Readings in the hysteresis band [threshold, threshold + margin) count as far
// while CLEAR and as near while BLOCKED, so an object sitting at the edge does
// not toggle the brake.
//
// Consequence: once BLOCKED, the car stays stopped until the sensor sees a
// clear path. If the obstacle is removed and nothing is within range, there
// are no valid readings and the car keeps waiting.
//
// BLOCKED puts the system in STOPPED (speed 0, servo frozen). It does not
// disarm or FAULT. The latched speed is cleared while braking, so the car moves
// again only with a new C:SET_SPEED (the Jetson re-sends it on OBSTACLE_CLEARED).

#define ULTRASONIC_TASK_PERIOD_MS 50

#ifndef ULTRASONIC_DEBOUNCE_COUNT
#define ULTRASONIC_DEBOUNCE_COUNT 3 // valid readings in a row to brake, and to clear
#endif
#ifndef ULTRASONIC_CLEAR_MARGIN_CM
#define ULTRASONIC_CLEAR_MARGIN_CM 5
#endif

void ultrasonic_task(void *pvParameters)
{
    (void)pvParameters;
    bool blocked = false;
    uint8_t run = 0; // valid readings in a row pointing to the OTHER state

    log_line("[Ultrasonic] started");

    while (1)
    {
        const uint16_t cm = ultrasonic_read_cm(); // 0 = no valid reading

        if (cm != 0)
        {
            const uint16_t limit = blocked
                ? ULTRASONIC_OBSTACLE_THRESHOLD_CM + ULTRASONIC_CLEAR_MARGIN_CM
                : ULTRASONIC_OBSTACLE_THRESHOLD_CM;
            const bool near = cm < limit;

            if (near != blocked)
            {
                run++;
            }
            else
            {
                run = 0; // a valid reading confirming the current state
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

        vTaskDelay(pdMS_TO_TICKS(ULTRASONIC_TASK_PERIOD_MS));
    }
}
