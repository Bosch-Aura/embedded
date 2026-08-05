#include "ultrasonic_task.h"
#include "hardware.h"
#include "motor_task.h"
#include "supervisor_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>

#define ULTRASONIC_TASK_PERIOD_MS 50  // 20 Hz - read sensor every 50ms
#ifndef ULTRASONIC_DEBOUNCE_COUNT
#define ULTRASONIC_DEBOUNCE_COUNT 3 // Require 3 consecutive detections before triggering
#endif

void ultrasonic_task(void *pvParameters) {
    uint8_t obstacle_detected_count = 0;

    Serial.println("[UltrasonicTask] Ultrasonic obstacle detection task started");

    while (1) {
        uint16_t distance_cm = ultrasonic_read_cm();


        if (distance_cm > 0 && distance_cm < ULTRASONIC_OBSTACLE_THRESHOLD_CM) {
            // Object detected within threshold
            obstacle_detected_count++;
            
            if (obstacle_detected_count >= ULTRASONIC_DEBOUNCE_COUNT) {
                // Trigger emergency brake
                Serial.print("[UltrasonicTask] Obstacle detected at ");
                Serial.print(distance_cm);
                Serial.println(" cm - Emergency brake triggered!");
                // ALTO-1: brake AND latch FAULT, like the watchdog and the
                // E-STOP already do. Braking alone left the system in RUNNING,
                // so a transient obstacle (someone walking past) stopped the
                // car and it drove off again on its own 5 s later.
                motor_task_trigger_emergency();
                supervisor_trigger_fault("ULTRASONIC_OBSTACLE");
                obstacle_detected_count = 0; // Reset counter
            }
        } else {
            // No obstacle or reading error - reset counter
            obstacle_detected_count = 0;
        }
        
        vTaskDelay(pdMS_TO_TICKS(ULTRASONIC_TASK_PERIOD_MS));
    }
}

