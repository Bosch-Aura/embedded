#ifndef ULTRASONIC_TASK_H
#define ULTRASONIC_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

void ultrasonic_task(void *pvParameters);

// M:OBSTACLE_RESET:0 (LinkRxTask): release an obstacle hold now. Handled on the
// next reading; the hold comes back if the obstacle is still there.
void ultrasonic_request_reset(void);

// Sensor health as reported by EVENT:ULTRASONIC_SILENT / _OK / _FAULT /
// _RECOVERED (FAULT wins over SILENT). Read by M:STATUS.
typedef enum
{
    US_HEALTH_OK = 0, // valid readings arriving
    US_HEALTH_SILENT, // no valid reading for ULTRASONIC_SILENT_MS (open space or dead)
    US_HEALTH_FAULT,  // the sensor is not firing: no obstacle protection
} ultrasonic_health_t;
ultrasonic_health_t ultrasonic_health(void);
const char *ultrasonic_health_name(ultrasonic_health_t h);

#ifdef __cplusplus
}
#endif

#endif // ULTRASONIC_TASK_H

