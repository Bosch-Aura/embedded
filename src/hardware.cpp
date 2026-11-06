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
#ifndef ULTRASONIC_ECHO_TIMEOUT_MS
#define ULTRASONIC_ECHO_TIMEOUT_MS 30 // ~5 m round trip; past the 4 m range
#endif

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
    // Falling edge. Ignore it if nobody is waiting (the reading already timed
    // out, e.g. the ~38 ms "nothing in range" pulse of the HC-SR04) or if its
    // rising edge belongs to an earlier trigger.
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
    pinMode(GPIO_ULTRASONIC_ECHO, INPUT);
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

uint32_t ultrasonic_last_echo_us(void) { return last_echo_us; }

uint16_t ultrasonic_read_cm(void) {
    last_echo_us = 0;
#ifndef ULTRASONIC_USE_PULSEIN
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
        echo_waiting = false; // timeout: no echo; a late edge is ignored
        return 0;
    }
    const uint32_t duration = echo_width_us;
#else
    const uint32_t duration = pulseIn(GPIO_ULTRASONIC_ECHO, HIGH, ULTRASONIC_ECHO_TIMEOUT_MS * 1000UL);
#endif
    last_echo_us = duration;
    if (duration == 0) {
        return 0; // no echo
    }

    // distance = duration * speed_of_sound / 2 = duration_us / 58 (343 m/s)
    const uint32_t distance_cm = duration / 58;
    if (distance_cm < ULTRASONIC_MIN_DISTANCE_CM || distance_cm > ULTRASONIC_MAX_DISTANCE_CM) {
        return 0; // out of range: not a valid reading
    }
    return (uint16_t)distance_cm;
}
