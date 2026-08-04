#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdint.h>
#include <stdbool.h>
#include <Arduino.h>

#ifdef __cplusplus
extern "C"
{
#endif

// GPIO Pin definitions
#define GPIO_MOTOR_IN3 14
#define GPIO_MOTOR_IN4 12
#define GPIO_MOTOR_ENB 13
#define GPIO_SERVO 25
#define GPIO_HEADLIGHTS 32
#define GPIO_REVERSE_LIGHTS 33
#define GPIO_LDR 35
#define GPIO_ESTOP 4
#define GPIO_LED_BUILTIN 2
#define GPIO_ULTRASONIC_TRIG 26 // HC-SR04 Trigger pin
#define GPIO_ULTRASONIC_ECHO 27 // HC-SR04 Echo pin

// Servo configuration
#define SERVO_CENTER 105
#define SERVO_LEFT 50
#define SERVO_RIGHT 160
#define SERVO_PWM_FREQ_HZ 50

// Motor configuration
#define MOTOR_SPEED_MAX 255

// UART configuration
#define UART_BAUD_RATE 921600
// A-11: GPIO 9/10 are wired to the module's internal SPI flash (SD_DATA2/SD_DATA3)
// on the ESP32-WROOM-32 and are not even exposed on the DevKit v1 header.
// 16/17 are the standard free choice on this board.
// NOTE: on ESP32-WROVER modules GPIO 16/17 are taken by the PSRAM - if the board
// is ever swapped for a WROVER variant, move these to 18/19.
#define UART_TX_PIN 17
#define UART_RX_PIN 16
#define UART_BUF_SIZE 1024

// LDR threshold
#define LDR_THRESHOLD 3500

// Ultrasonic sensor (HC-SR04) configuration
#define ULTRASONIC_MAX_DISTANCE_CM 400      // Maximum range ~4m
#define ULTRASONIC_MIN_DISTANCE_CM 2        // Minimum range ~2cm
#define ULTRASONIC_OBSTACLE_THRESHOLD_CM 30 // Trigger emergency if object closer than 30cm

// Link timing - single source of truth (A-3).
// The hierarchy that must always hold is:
//   emission period  <  control command TTL  <  watchdog timeout
#define LINK_EXPECTED_PERIOD_MS 100                            // 10 Hz, what we recommend to the Brain team
#define CONTROL_CMD_TTL_MS (LINK_EXPECTED_PERIOD_MS * 2)       // 200 ms
#define WATCHDOG_TIMEOUT_AUTO_MS (LINK_EXPECTED_PERIOD_MS * 3) // 300 ms, always > TTL
#define WATCHDOG_TIMEOUT_MANUAL_MS 1000                        // looser: a human operator is in the loop

    // Initialize all hardware
    void hardware_init(void);

    // Motor control
    void motor_set_speed(uint8_t speed);
    void motor_set_direction(bool forward);
    void motor_stop(void);

    // Steering control
    void steer_set_angle(uint16_t angle);

    // Lights control
    void lights_set_headlights(bool on);
    void lights_set_reverse(bool on);

    // LDR reading
    uint16_t ldr_read(void);

    // E-STOP GPIO reading
    bool estop_is_triggered(void);

    // Ultrasonic sensor (HC-SR04) reading
    uint16_t ultrasonic_read_cm(void); // Returns distance in cm, 0 if error/timeout

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_H
