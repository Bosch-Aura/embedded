#include "mailbox.h"
#include "freertos/FreeRTOS.h"
#include <Arduino.h>

bool mailbox_init(mailbox_t *mb) {
    mb->ts_ms = 0;
    mb->topic = TOPIC_MOTOR;
    mb->cmd = CMD_UNKNOWN;
    mb->value = 0;
    mb->seq = 0;
    mb->ttl_ms = 0;
    mb->valid = false;
    mb->mutex = xSemaphoreCreateMutex();
    if (mb->mutex == NULL) {
        // A-12: without the mutex every read/write silently returns false, i.e. a
        // permanently dead command channel. The caller must not ignore this.
        Serial.println("[Mailbox] Failed to create mailbox mutex");
        return false;
    }
    return true;
}

bool mailbox_write(mailbox_t *mb, topic_t topic, command_type_t cmd, int32_t value, uint32_t ttl_ms) {
    if (mb == NULL || mb->mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(mb->mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        mb->topic = topic;
        mb->cmd = cmd;
        mb->value = value;
        mb->ttl_ms = ttl_ms;
        mb->ts_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        mb->seq++;
        mb->valid = true;
        xSemaphoreGive(mb->mutex);
        return true;
    }
    return false;
}

bool mailbox_read(mailbox_t *mb, topic_t *topic, command_type_t *cmd, int32_t *value, uint32_t *ts_ms, bool *expired) {
    if (mb == NULL || mb->mutex == NULL) {
        return false;
    }

    bool result = false;
    // M-12: always initialise the output parameter, pessimistically. If the mutex
    // times out or the mailbox was never written, the caller must see "expired"
    // rather than an uninitialised stack value. With C-1 that means "brake".
    if (expired != NULL) {
        *expired = true;
    }
    if (xSemaphoreTake(mb->mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (mb->valid) {
            uint32_t current_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            *expired = mailbox_is_expired(mb, current_ms);
            
            if (!*expired) {
                *topic = mb->topic;
                *cmd = mb->cmd;
                *value = mb->value;
                *ts_ms = mb->ts_ms;
                result = true;
            }
        }
        xSemaphoreGive(mb->mutex);
    }
    return result;
}

bool mailbox_is_expired(const mailbox_t *mb, uint32_t current_ms) {
    if (mb == NULL || !mb->valid) {
        return true;
    }
    
    if (mb->ttl_ms == 0) {
        return false; // No expiration
    }
    
    uint32_t age_ms = current_ms - mb->ts_ms;
    return age_ms > mb->ttl_ms;
}
