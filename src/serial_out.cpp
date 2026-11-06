#include "serial_out.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SERIAL_LINE_MAX 160

static SemaphoreHandle_t serial_mutex = NULL;

void serial_out_init(void)
{
    if (serial_mutex == NULL)
    {
        serial_mutex = xSemaphoreCreateMutex();
    }
}

// A mutex, not a critical section: priority inheritance lifts whoever is
// printing instead of leaving a high-priority task stuck behind it.
static void write_line(const char *buf, size_t len, bool to_uart)
{
    if (serial_mutex != NULL)
    {
        xSemaphoreTake(serial_mutex, portMAX_DELAY);
    }
    Serial.write((const uint8_t *)buf, len);
    if (to_uart)
    {
        Serial1.write((const uint8_t *)buf, len);
    }
    if (serial_mutex != NULL)
    {
        xSemaphoreGive(serial_mutex);
    }
}

static void emit(bool to_uart, const char *fmt, va_list args)
{
    char buf[SERIAL_LINE_MAX];
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    if (n < 0)
    {
        return;
    }
    if ((size_t)n > sizeof(buf) - 3)
    {
        n = sizeof(buf) - 3; // truncated, never split
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    write_line(buf, (size_t)n, to_uart);
}

void log_linef(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    emit(false, fmt, args);
    va_end(args);
}

void event_linef(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    emit(true, fmt, args);
    va_end(args);
}

void log_line(const char *text) { log_linef("%s", text); }
void event_line(const char *text) { event_linef("%s", text); }
