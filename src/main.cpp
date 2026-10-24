#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hardware.h"
#include "mailbox.h"
#include "motor_task.h"
#include "steer_task.h"
#include "lights_task.h"
#include "link_rx_task.h"
#include "link_tx_task.h"
#include "supervisor_task.h"
#include "ultrasonic_task.h"
#include "serial_out.h"

// Mailboxes
static mailbox_t motor_mailbox;
static mailbox_t steer_mailbox;
static mailbox_t lights_mailbox;
static mailbox_t supervisor_mailbox;

// Task handles
#define STACK_SIZE_4K 4096
#define STACK_SIZE_8K 8192

// A-12: a task that fails to start must never be silently ignored. The old code
// printed "created" and then "System ready!" regardless, so the system could
// announce EVENT:SYSTEM_READY with MotorTask or SupervisorTask missing - i.e.
// with no watchdog and no E-STOP, and nothing to stop the vehicle.
static void create_task_or_die(TaskFunction_t fn, const char *name, uint32_t stack,
                               void *params, UBaseType_t prio, BaseType_t core)
{
    BaseType_t ok = xTaskCreatePinnedToCore(fn, name, stack, params, prio, NULL, core);
    if (ok != pdPASS)
    {
        serial_linef("EVENT:FATAL:TASK_CREATE_FAILED:%s", name);
        Serial.flush();
        // Safe state before restarting
        motor_stop();
        delay(100);
        esp_restart();
    }
    serial_linef("[main] %s created on Core %d, Priority %d", name, (int)core, (int)prio);
}

void setup(void)
{
    Serial.begin(115200); // Match dashboard baudrate
    serial_out_init();    // before anything prints: all lines go through it

    // M-1: hardware to a SAFE state first, before anything that can block.
    // Until hardware_init() runs, GPIO_MOTOR_IN3/IN4/ENB are in reset state
    // (floating inputs) and the H-bridge may spin the motor.
    hardware_init();

#ifdef WAIT_FOR_SERIAL_MONITOR
    // Development builds only: give the serial monitor time to attach.
    // The hardware is already in a safe state at this point.
    delay(3000);
#endif

    serial_line("========================================");
    serial_line("ESP32 RC Car FreeRTOS System Starting...");
    serial_line("========================================");

    // Initialize mailboxes. A-12: a mailbox without its mutex is a permanently
    // dead command channel - do not boot with one.
    if (!mailbox_init(&motor_mailbox) ||
        !mailbox_init(&steer_mailbox) ||
        !mailbox_init(&lights_mailbox) ||
        !mailbox_init(&supervisor_mailbox))
    {
        serial_line("EVENT:FATAL:MAILBOX_INIT_FAILED");
        Serial.flush();
        motor_stop();
        delay(100);
        esp_restart();
    }

    serial_line("[main] Mailboxes initialized");

    // A-5: create the telemetry TX queue BEFORE any task exists, so no producer
    // can find it NULL and drop the initial state/mode events.
    if (!link_tx_init())
    {
        serial_line("EVENT:FATAL:TX_QUEUE_CREATE_FAILED");
        Serial.flush();
        motor_stop();
        delay(100);
        esp_restart();
    }

    // Create tasks with core pinning and priorities as specified

    // LinkRxTask - Core 1, Priority 4
    // C-2: static -> the struct must outlive setup(); the tasks keep dereferencing it
    static link_rx_params_t link_rx_params = {
        .motor_mailbox = &motor_mailbox,
        .steer_mailbox = &steer_mailbox,
        .lights_mailbox = &lights_mailbox,
        .supervisor_mailbox = &supervisor_mailbox};
    create_task_or_die(link_rx_task, "LinkRxTask", STACK_SIZE_8K, &link_rx_params, 4, 1);

    // MotorTask - Core 0, Priority 4
    create_task_or_die(motor_task, "MotorTask", STACK_SIZE_4K, &motor_mailbox, 4, 0);

    // SteerTask - Core 0, Priority 3
    create_task_or_die(steer_task, "SteerTask", STACK_SIZE_4K, &steer_mailbox, 3, 0);

    // LightsTask - Core 1, Priority 1
    create_task_or_die(lights_task, "LightsTask", STACK_SIZE_4K, &lights_mailbox, 1, 1);

    // SupervisorTask - Core 1, Priority 2
    // C-2: static -> the struct must outlive setup()
    static supervisor_params_t supervisor_params = {
        .supervisor_mailbox = &supervisor_mailbox,
        .motor_mailbox = &motor_mailbox,
        .steer_mailbox = &steer_mailbox};
    create_task_or_die(supervisor_task, "SupervisorTask", STACK_SIZE_4K, &supervisor_params, 2, 1);

    // LinkTxTask - Core 1, Priority 2
    create_task_or_die(link_tx_task, "LinkTxTask", STACK_SIZE_4K, NULL, 2, 1);

    // UltrasonicTask - Core 0, Priority 5 (high priority for safety)
    create_task_or_die(ultrasonic_task, "UltrasonicTask", STACK_SIZE_4K, NULL, 5, 0);

    serial_line("[main] All tasks created. FreeRTOS scheduler running...");
    serial_line("[main] System ready!");
    // Built-in LED on: the board booted and every task is running.
    digitalWrite(GPIO_LED_BUILTIN, HIGH);
    serial_line("EVENT:SYSTEM_READY");
    Serial.flush();
}

void loop(void)
{
    // FreeRTOS tasks handle everything, this loop should not execute
    // But we keep it to satisfy Arduino framework requirements
    vTaskDelay(pdMS_TO_TICKS(1000));
}
