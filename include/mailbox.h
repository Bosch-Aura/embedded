#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdint.h>
#include <stdbool.h>
#include "messages.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mailbox structure for last-writer-wins pattern
typedef struct {
    uint32_t ts_ms;          // Timestamp in milliseconds
    topic_t topic;            // Message topic
    command_type_t cmd;       // Command type
    int32_t value;            // Command value (speed, angle, etc.)
    uint32_t seq;             // Sequence number
    uint32_t ttl_ms;          // Time to live in milliseconds
    bool valid;               // Whether this mailbox entry is valid
    SemaphoreHandle_t mutex; // Mutex for atomic operations
} mailbox_t;

// Initialize a mailbox
// Returns false if the mutex could not be created (mailbox permanently unusable).
bool mailbox_init(mailbox_t *mb);

// Write to mailbox (atomic, last-writer-wins)
bool mailbox_write(mailbox_t *mb, topic_t topic, command_type_t cmd, int32_t value, uint32_t ttl_ms);

// Read from mailbox (atomic)
bool mailbox_read(mailbox_t *mb, topic_t *topic, command_type_t *cmd, int32_t *value, uint32_t *ts_ms, bool *expired);

// Check if mailbox data is expired
bool mailbox_is_expired(const mailbox_t *mb, uint32_t current_ms);

#ifdef __cplusplus
}
#endif

#endif // MAILBOX_H



