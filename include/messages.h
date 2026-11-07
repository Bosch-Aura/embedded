#ifndef MESSAGES_H
#define MESSAGES_H

#ifdef __cplusplus
extern "C" {
#endif

// System states. The names travel to the Jetson as EVENT:STATE_CHANGED:<name>.
//
//   DISARMED --SYS_ARM--> RUNNING <--> STOPPED     (obstacle or SYS_STOP)
//      ^                     |            |
//      +------SYS_DISARM-----+------------+
//   any state --E-STOP / watchdog--> FAULT --SYS_ARM--> RUNNING
//   FAULT (watchdog only) --heartbeat stable 500 ms--> RUNNING / STOPPED
typedef enum {
    STATE_DISARMED,
    STATE_RUNNING,
    STATE_STOPPED, // armed but held: SYS_STOP (nothing moves, servo frozen) or
                   // obstacle (only motion toward the sensor is blocked)
    STATE_FAULT    // E-STOP or lost link. M:SYS_ARM:0 leaves it; a lost-link
                   // FAULT also clears by itself when the link is back
} system_state_t;

// Link channels: <CHANNEL>:<COMMAND>[:<VALUE>]\n
#define CHANNEL_EMERGENCY 'E'
#define CHANNEL_CONTROL 'C'
#define CHANNEL_MANAGEMENT 'M'

#ifdef __cplusplus
}
#endif

#endif // MESSAGES_H
