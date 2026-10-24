#ifndef MESSAGES_H
#define MESSAGES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Command types
typedef enum {
    CMD_SET_SPEED,
    CMD_SET_STEER,
    CMD_BRAKE_NOW,
    CMD_STOP,
    CMD_SYS_ARM,
    CMD_SYS_DISARM,
    CMD_LIGHTS_ON,
    CMD_LIGHTS_OFF,
    CMD_LIGHTS_AUTO,
    CMD_UNKNOWN
} command_type_t;

// Topics/channels
typedef enum {
    TOPIC_MOTOR,
    TOPIC_STEER,
    TOPIC_LIGHTS,
    TOPIC_SYSTEM,
    TOPIC_EMERGENCY,
    TOPIC_CONTROL,
    TOPIC_MANAGEMENT
} topic_t;

// System states
typedef enum {
    STATE_DISARMED,
    STATE_ARMED,
    STATE_RUNNING,
    STATE_FAULT
} system_state_t;

// UART channel prefixes
#define CHANNEL_EMERGENCY 'E'
#define CHANNEL_CONTROL 'C'
#define CHANNEL_MANAGEMENT 'M'

#ifdef __cplusplus
}
#endif

#endif // MESSAGES_H



