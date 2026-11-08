#ifndef CONTROL_TASK_H
#define CONTROL_TASK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ControlTask: the only task that touches the H-bridge and the servo.
// Runs at 100 Hz, or immediately when woken by control_wake().
void control_task(void *pvParameters);

// Setters for LinkRxTask. Each command is applied once, on the next cycle, and
// only if the system state allows it. Values are clamped by ControlTask.
void control_set_speed(int32_t speed);   // C:SET_SPEED, latched until changed
void control_set_steer(int32_t angle);   // C:SET_STEER
void control_set_direction(bool forward); // C:SET_DIR, applied with dead time
void control_brake(void);                // E:BRAKE_NOW: brake now, clear latched speed, confirm it
void control_drop_speed(void);           // same without the event (DISARM, FAULT)
// M:OBSTACLE_RESET: forget the speed an obstacle hold would resume at, so a
// manual release never restarts the car by itself.
void control_forget_held_speed(void);

// As of the last control cycle, for M:STATUS: the latched speed (PWM) and the
// speed an obstacle hold will resume to (0 = none).
uint8_t control_latched_speed(void);
uint8_t control_held_speed(void);

// Run a control cycle now (state changed, brake requested).
void control_wake(void);

#ifdef __cplusplus
}
#endif

#endif // CONTROL_TASK_H
