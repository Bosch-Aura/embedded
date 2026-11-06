#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hardware.h"
#include "control_task.h"
#include "link_rx_task.h"
#include "supervisor_task.h"
#include "ultrasonic_task.h"
#include "serial_out.h"

#define STACK_4K 4096
#define STACK_8K 8192
#define SERIAL_TX_BUFFER_SIZE 1024

// A missing task means no watchdog or no brake: never boot without one.
static void create_task_or_die(TaskFunction_t fn, const char *name, uint32_t stack,
                               UBaseType_t prio, BaseType_t core)
{
    if (xTaskCreatePinnedToCore(fn, name, stack, NULL, prio, NULL, core) != pdPASS)
    {
        event_linef("EVENT:FATAL:TASK_CREATE_FAILED:%s", name);
        Serial.flush();
        motor_stop();
        delay(100);
        esp_restart();
    }
    log_linef("[main] %s on core %d, priority %d", name, (int)core, (int)prio);
}

void setup(void)
{
    // TX buffers so write() copies and returns. With the default (0) it blocks
    // until the bytes fit in the 128-byte hardware FIFO, which stalls
    // ControlTask for milliseconds whenever it emits an event.
    Serial.setTxBufferSize(SERIAL_TX_BUFFER_SIZE);
    Serial1.setTxBufferSize(SERIAL_TX_BUFFER_SIZE);
    Serial.begin(SERIAL_BAUD);
    serial_out_init(); // before anything prints

    // Hardware to a SAFE state before anything that can block: until then the
    // H-bridge pins float and the motor may spin.
    hardware_init();

#ifdef WAIT_FOR_SERIAL_MONITOR
    delay(3000); // development builds only; hardware is already safe
#endif

    // Core 0: actuation and the safety sensor. Core 1: link and supervision.
    create_task_or_die(ultrasonic_task, "Ultrasonic", STACK_4K, 5, 0);
    create_task_or_die(control_task, "Control", STACK_4K, 4, 0);
    create_task_or_die(link_rx_task, "LinkRx", STACK_8K, 4, 1);
    create_task_or_die(supervisor_task, "Supervisor", STACK_4K, 2, 1);

    digitalWrite(GPIO_LED_BUILTIN, HIGH); // booted, every task running
    event_line("EVENT:SYSTEM_READY");
}

void loop(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000)); // everything runs in the tasks
}
