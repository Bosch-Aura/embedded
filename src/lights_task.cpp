#include "lights_task.h"
#include "hardware.h"
#include "mailbox.h"
#include "messages.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "serial_out.h"
#include <Arduino.h>

#define LIGHTS_TASK_PERIOD_MS 1000 // 1 Hz for auto-mode checks
#define LDR_CHECK_PERIOD_MS 1000

typedef enum
{
    LIGHTS_MODE_OFF,
    LIGHTS_MODE_ON,
    LIGHTS_MODE_AUTO
} lights_mode_t;

static mailbox_t *lights_mailbox = NULL;
static lights_mode_t current_mode = LIGHTS_MODE_OFF;
static uint32_t last_ldr_check = 0;

void lights_task(void *pvParameters)
{
    lights_mailbox = (mailbox_t *)pvParameters;

    serial_line("[LightsTask] Lights task started");

    while (1)
    {
        // Read mailbox for lights commands
        topic_t topic;
        command_type_t cmd;
        int32_t value;
        uint32_t ts_ms;
        bool expired;

        if (mailbox_read(lights_mailbox, &topic, &cmd, &value, &ts_ms, &expired))
        {
            if (!expired)
            {
                switch (cmd)
                {
                case CMD_LIGHTS_ON:
                    // Always execute command, but only print if mode changed (like motors)
                    if (current_mode != LIGHTS_MODE_ON)
                    {
                        current_mode = LIGHTS_MODE_ON;
                        lights_set_headlights(true);
                        serial_line("EVENT:CMD_EXECUTED:LIGHTS_ON");
                        Serial.flush();
                    } else {
                        // Mode didn't change, but still ensure lights are on
                        lights_set_headlights(true);
                    }
                    break;

                case CMD_LIGHTS_OFF:
                    // Always execute command, but only print if mode changed (like motors)
                    if (current_mode != LIGHTS_MODE_OFF)
                    {
                        current_mode = LIGHTS_MODE_OFF;
                        lights_set_headlights(false);
                        serial_line("EVENT:CMD_EXECUTED:LIGHTS_OFF");
                        Serial.flush();
                    } else {
                        // Mode didn't change, but still ensure lights are off
                        lights_set_headlights(false);
                    }
                    break;

                case CMD_LIGHTS_AUTO:
                    // Always execute command, but only print if mode changed (like motors)
                    if (current_mode != LIGHTS_MODE_AUTO)
                    {
                        current_mode = LIGHTS_MODE_AUTO;
                        serial_line("EVENT:CMD_EXECUTED:LIGHTS_AUTO");
                        Serial.flush();
                    }
                    break;

                default:
                    break;
                }
            }
        }

        // Handle auto mode
        if (current_mode == LIGHTS_MODE_AUTO)
        {
            uint32_t current_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (current_ms - last_ldr_check >= LDR_CHECK_PERIOD_MS)
            {
                uint16_t ldr_value = ldr_read();

                if (ldr_value < LDR_THRESHOLD)
                {
                    // High light intensity
                    lights_set_headlights(false);
                }
                else
                {
                    // Low light intensity
                    lights_set_headlights(true);
                }

                last_ldr_check = current_ms;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(LIGHTS_TASK_PERIOD_MS));
    }
}
