#include "serial_out.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Longest line any task prints today is ~90 characters.
#define SERIAL_LINE_MAX 160

static SemaphoreHandle_t serial_mutex = NULL;

void serial_out_init(void)
{
    if (serial_mutex == NULL)
    {
        serial_mutex = xSemaphoreCreateMutex();
    }
}

static void write_line(const char *buf, size_t len)
{
    // A mutex (not a critical section): it has priority inheritance, so a
    // high-priority task waiting here lifts whoever is printing instead of
    // being blocked behind a medium-priority one.
    if (serial_mutex != NULL)
    {
        xSemaphoreTake(serial_mutex, portMAX_DELAY);
    }
    Serial.write((const uint8_t *)buf, len);
    if (serial_mutex != NULL)
    {
        xSemaphoreGive(serial_mutex);
    }
}

void serial_line(const char *text)
{
    char buf[SERIAL_LINE_MAX];
    size_t n = strnlen(text, sizeof(buf) - 3);
    memcpy(buf, text, n);
    buf[n++] = '\r';
    buf[n++] = '\n';
    write_line(buf, n);
}

void serial_linef(const char *fmt, ...)
{
    char buf[SERIAL_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);
    if (n < 0)
    {
        return;
    }
    if ((size_t)n > sizeof(buf) - 3)
    {
        n = sizeof(buf) - 3; // truncated by vsnprintf: keep what fits
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    write_line(buf, (size_t)n);
}
