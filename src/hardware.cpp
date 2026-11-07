#include <Arduino.h>
#include "hardware.h"
#include "serial_out.h"
#include <ESP32Servo.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// HC-SR04 echo measurement.
//
// Default: an interrupt on both edges of ECHO timestamps the pulse, and the
// reading task SLEEPS until the falling edge (or the timeout). pulseIn() busy-
// waited up to 30 ms at priority 5 on core 0, holding ControlTask off the CPU
// for that long on every reading without an echo.
//
// -DULTRASONIC_USE_PULSEIN goes back to pulseIn(), in case the interrupt
// version misbehaves on some board.
//
// "Nothing in range" vs "sensor not answering". When the sensor fires but no
// echo comes back, ECHO still goes HIGH and stays HIGH for a long time: ~38 ms
// on the datasheet, ~200 ms on some clones (the bench logs show readings
// failing in groups of 3 at the 80 ms cadence). So:
//   rising edge seen, no falling edge within the timeout -> US_READ_NO_ECHO
//   no rising edge at all                                -> US_READ_NO_RESPONSE
// A sensor without power, or with TRIG or ECHO loose, never raises ECHO.
//
// While ECHO is still HIGH from the previous no-echo pulse the sensor ignores
// TRIG; triggering then is wasted and, on the bench, the first trigger after
// that long pulse came back as a phantom ~6 cm (368 us) echo. So we wait for
// ECHO to go LOW and give the sensor ULTRASONIC_REARM_MS after the falling
// edge before triggering again.
#ifndef ULTRASONIC_ECHO_TIMEOUT_MS
#define ULTRASONIC_ECHO_TIMEOUT_MS 30 // ~5 m round trip; past the 4 m range
#endif
#ifndef ULTRASONIC_BUSY_MAX_MS
#define ULTRASONIC_BUSY_MAX_MS 300 // longest no-echo pulse we wait out; then STUCK_HIGH
#endif
#ifndef ULTRASONIC_REARM_MS
#define ULTRASONIC_REARM_MS 20 // quiet time after a falling edge before the next trigger
#endif

static volatile uint32_t echo_fall_us = 0; // last falling edge, waited for or not
static volatile bool echo_fall_known = false;

#ifndef ULTRASONIC_USE_PULSEIN
static volatile uint32_t echo_rise_us = 0;
static volatile uint32_t echo_width_us = 0;
static volatile bool echo_rising_seen = false;
static volatile bool echo_waiting = false; // a reading is waiting for its edge
static TaskHandle_t volatile echo_waiter = NULL;

static void ARDUINO_ISR_ATTR echo_isr(void)
{
    const uint32_t t = micros();
    if (digitalRead(GPIO_ULTRASONIC_ECHO))
    {
        echo_rise_us = t;
        echo_rising_seen = true;
        return;
    }
    echo_fall_us = t;
    echo_fall_known = true;
    // Ignore the falling edge if nobody is waiting (the reading already timed
    // out, e.g. the long "nothing in range" pulse) or if its rising edge
    // belongs to an earlier trigger.
    if (!echo_waiting || !echo_rising_seen)
    {
        return;
    }
    echo_width_us = t - echo_rise_us;
    echo_waiting = false;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(echo_waiter, &woken);
    if (woken)
    {
        portYIELD_FROM_ISR();
    }
}
#endif

static Servo steerServo;

void hardware_init(void) {
    // GPIO configuration for outputs
    pinMode(GPIO_MOTOR_IN3, OUTPUT);
    pinMode(GPIO_MOTOR_IN4, OUTPUT);
    pinMode(GPIO_MOTOR_ENB, OUTPUT);
    pinMode(GPIO_LED_BUILTIN, OUTPUT);
    pinMode(GPIO_ESTOP, INPUT_PULLUP);
    
    // HC-SR04 Ultrasonic sensor
    pinMode(GPIO_ULTRASONIC_TRIG, OUTPUT);
    // Pulldown: with the ECHO wire loose the pin reads LOW (US_READ_NO_RESPONSE)
    // instead of floating, so noise cannot fake the rising edge of a no-echo
    // reading, which may release the obstacle brake.
    pinMode(GPIO_ULTRASONIC_ECHO, INPUT_PULLDOWN);
    digitalWrite(GPIO_ULTRASONIC_TRIG, LOW);
#ifndef ULTRASONIC_USE_PULSEIN
    attachInterrupt(digitalPinToInterrupt(GPIO_ULTRASONIC_ECHO), echo_isr, CHANGE);
#endif

    // Initialize GPIO states
    digitalWrite(GPIO_MOTOR_IN3, LOW);
    digitalWrite(GPIO_MOTOR_IN4, LOW);
    digitalWrite(GPIO_LED_BUILTIN, LOW);
    
    // Servo, centered
    steerServo.setPeriodHertz(SERVO_PWM_FREQ_HZ);
    steerServo.attach(GPIO_SERVO, 500, 2500);
    steerServo.write(SERVO_CENTER);

    // UART to the Jetson. 921600 baud fills the default 256-byte RX buffer in
    // under 3 ms, so give it room for a burst between two 10 ms polls.
    Serial1.setRxBufferSize(1024);
    Serial1.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    log_line("Hardware initialized");
}

void motor_set_speed(uint8_t speed) {
#if MOTOR_SPEED_MAX < 255 // hardware.env may lower the cap
    if (speed > MOTOR_SPEED_MAX) {
        speed = MOTOR_SPEED_MAX;
    }
#endif
    analogWrite(GPIO_MOTOR_ENB, speed);
}

void motor_set_direction(bool forward) {
    if (forward) {
        digitalWrite(GPIO_MOTOR_IN3, HIGH);
        digitalWrite(GPIO_MOTOR_IN4, LOW);
    } else {
        digitalWrite(GPIO_MOTOR_IN3, LOW);
        digitalWrite(GPIO_MOTOR_IN4, HIGH);
    }
}

void motor_stop(void) {
    digitalWrite(GPIO_MOTOR_IN3, LOW);
    digitalWrite(GPIO_MOTOR_IN4, LOW);
    analogWrite(GPIO_MOTOR_ENB, 0);
}

void steer_set_angle(uint16_t angle) {
    steerServo.write(angle);
}

bool estop_is_triggered(void) {
    // E-STOP is active low (pulled up, triggers when grounded)
    return digitalRead(GPIO_ESTOP) == LOW;
}

static volatile uint32_t last_echo_us = 0; // debug only: width of the last echo
static volatile ultrasonic_status_t last_status = US_READ_NO_RESPONSE;

uint32_t ultrasonic_last_echo_us(void) { return last_echo_us; }
ultrasonic_status_t ultrasonic_last_status(void) { return last_status; }

const char *ultrasonic_status_name(ultrasonic_status_t s)
{
    switch (s)
    {
    case US_READ_OK: return "ok";
    case US_READ_NO_ECHO: return "sin eco, nada en rango";
    case US_READ_TOO_FAR: return "fuera de rango lejos";
    case US_READ_TOO_CLOSE: return "demasiado cerca";
    case US_READ_NO_RESPONSE: return "sin respuesta, sin flanco de subida";
    case US_READ_STUCK_HIGH: return "ECHO trabado en alto";
    }
    return "?";
}

// Wait out a pulse still in progress, then the re-arm time after its end.
// false: ECHO never came down.
static bool wait_sensor_ready(void)
{
    const TickType_t start = xTaskGetTickCount();
    bool was_high = false;
    while (digitalRead(GPIO_ULTRASONIC_ECHO))
    {
        was_high = true;
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= ULTRASONIC_BUSY_MAX_MS)
        {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2) > 0 ? pdMS_TO_TICKS(2) : 1);
    }
    if (was_high && !echo_fall_known) // pulseIn driver: no ISR timestamps
    {
        echo_fall_us = micros();
        echo_fall_known = true;
    }
    if (echo_fall_known)
    {
        const uint32_t since_fall_us = micros() - echo_fall_us;
        if (since_fall_us < ULTRASONIC_REARM_MS * 1000UL)
        {
            const uint32_t left_ms = (ULTRASONIC_REARM_MS * 1000UL - since_fall_us) / 1000 + 1;
            vTaskDelay(pdMS_TO_TICKS(left_ms) > 0 ? pdMS_TO_TICKS(left_ms) : 1);
        }
    }
    return true;
}

uint16_t ultrasonic_read_cm(void) {
    last_echo_us = 0;
    if (!wait_sensor_ready())
    {
        last_status = US_READ_STUCK_HIGH;
        return 0;
    }
#ifdef ULTRASONIC_USE_PULSEIN
    echo_fall_known = false; // only set again after waiting out a HIGH pulse
#else
    // Only the ultrasonic task calls this. Clear any stale notification, arm
    // the ISR, then trigger.
    echo_waiter = xTaskGetCurrentTaskHandle();
    ulTaskNotifyTake(pdTRUE, 0);
    echo_rising_seen = false;
    echo_width_us = 0;
    echo_waiting = true;
#endif

    // Trigger pulse (10 us HIGH)
    digitalWrite(GPIO_ULTRASONIC_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(GPIO_ULTRASONIC_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(GPIO_ULTRASONIC_TRIG, LOW);

#ifndef ULTRASONIC_USE_PULSEIN
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ULTRASONIC_ECHO_TIMEOUT_MS)) == 0)
    {
        echo_waiting = false; // timeout; a late falling edge is ignored
        // The sensor fired (ECHO rose) but nothing came back: nothing in range.
        // No rising edge: it never fired, so this proves nothing.
        last_status = echo_rising_seen ? US_READ_NO_ECHO : US_READ_NO_RESPONSE;
        return 0;
    }
    const uint32_t duration = echo_width_us;
#else
    const uint32_t duration = pulseIn(GPIO_ULTRASONIC_ECHO, HIGH, ULTRASONIC_ECHO_TIMEOUT_MS * 1000UL);
    if (duration == 0)
    {
        // pulseIn() does not say which edge was missing. ECHO still HIGH after
        // the timeout means it rose and is in its long no-echo pulse.
        last_status = digitalRead(GPIO_ULTRASONIC_ECHO) ? US_READ_NO_ECHO : US_READ_NO_RESPONSE;
        return 0;
    }
#endif
    last_echo_us = duration;
    if (duration == 0) {
        last_status = US_READ_NO_RESPONSE;
        return 0;
    }

    // distance = duration * speed_of_sound / 2 = duration_us / 58 (343 m/s)
    const uint32_t distance_cm = duration / 58;
    if (distance_cm < ULTRASONIC_MIN_DISTANCE_CM) {
        last_status = US_READ_TOO_CLOSE;
        return 0;
    }
    if (distance_cm > ULTRASONIC_MAX_DISTANCE_CM) {
        last_status = US_READ_TOO_FAR;
        return 0;
    }
    last_status = US_READ_OK;
    return (uint16_t)distance_cm;
}
