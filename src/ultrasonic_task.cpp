#include "ultrasonic_task.h"
#include "hardware.h"
#include "supervisor_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "serial_out.h"
#include <Arduino.h>

#define ULTRASONIC_TASK_PERIOD_MS 50  // 20 Hz - read sensor every 50ms
#ifndef ULTRASONIC_DEBOUNCE_COUNT
#define ULTRASONIC_DEBOUNCE_COUNT 3 // Require 3 consecutive detections before triggering
#endif

// Hysteresis: the obstacle is cleared only when the reading goes this far past
// the threshold, so an object sitting right at the edge does not toggle the
// brake on and off every few readings.
#ifndef ULTRASONIC_CLEAR_MARGIN_CM
#define ULTRASONIC_CLEAR_MARGIN_CM 5
#endif

// A reading of 0 means "no echo", and that is ambiguous: open space (nothing
// within ~4 m) gives 0, but so does an object very close to the sensor. With
// an object at 6 cm the HC-SR04 returned 6 only one reading in four and 0 the
// rest, so treating 0 as "clear" reset the debounce every time and the car
// never braked. A no-echo reading is therefore neutral, and only a run of this
// many in a row (~0.8 s) counts as "nothing there".
#ifndef ULTRASONIC_NO_ECHO_CLEAR_COUNT
#define ULTRASONIC_NO_ECHO_CLEAR_COUNT 10
#endif

// An obstacle puts the system in STOPPED while it is there: speed 0 and the
// servo frozen. It does not latch FAULT and does not disarm. FAULT made sense as a last resort, but an
// obstacle ahead is a normal driving situation (the car in front, a pedestrian,
// parking): with FAULT the vehicle stayed dead until someone re-armed it by
// hand. Whether to drive on once the way is clear is the Jetson's decision: the
// latched speed is cleared while braking, so nothing moves until a new
// C:SET_SPEED arrives.
void ultrasonic_task(void *pvParameters) {
    uint8_t near_count = 0;     // valid readings under the threshold
    uint8_t far_count = 0;      // valid readings past threshold + margin
    uint8_t no_echo_count = 0;  // consecutive readings with no echo
    bool obstacle = false;

    serial_line("[UltrasonicTask] Ultrasonic obstacle detection task started");

    while (1) {
        uint16_t distance_cm = ultrasonic_read_cm();

        if (distance_cm == 0) {
            if (no_echo_count < 255) {
                no_echo_count++;
            }
        } else {
            no_echo_count = 0;
        }

        if (!obstacle) {
            if (distance_cm > 0 && distance_cm < ULTRASONIC_OBSTACLE_THRESHOLD_CM) {
                near_count++;
                if (near_count >= ULTRASONIC_DEBOUNCE_COUNT) {
                    obstacle = true;
                    near_count = 0;
                    far_count = 0;
                    supervisor_set_obstacle(true);
                    serial_linef("EVENT:OBSTACLE_DETECTED:%u", (unsigned)distance_cm);
                    Serial.flush();
                }
            } else if (distance_cm > 0 || no_echo_count >= ULTRASONIC_NO_ECHO_CLEAR_COUNT) {
                // A valid far reading, or a long silence: nothing close
                near_count = 0;
            }
        } else {
            if (distance_cm >= ULTRASONIC_OBSTACLE_THRESHOLD_CM + ULTRASONIC_CLEAR_MARGIN_CM) {
                far_count++;
            } else if (distance_cm > 0) {
                far_count = 0; // still close
            }
            if (far_count >= ULTRASONIC_DEBOUNCE_COUNT ||
                no_echo_count >= ULTRASONIC_NO_ECHO_CLEAR_COUNT) {
                obstacle = false;
                far_count = 0;
                near_count = 0;
                supervisor_set_obstacle(false);
                serial_line("EVENT:OBSTACLE_CLEARED");
                Serial.flush();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ULTRASONIC_TASK_PERIOD_MS));
    }
}
