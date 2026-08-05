#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdint.h>
#include <stdbool.h>
#include <Arduino.h>

#ifdef __cplusplus
extern "C"
{
#endif

// ---------------------------------------------------------------------------
// TODOS los valores de este archivo se pueden sobrescribir desde
// Brain-Aura/hardware.env: scripts/hardware_env.py los inyecta como -D antes de
// compilar y, gracias a los #ifndef, lo que venga de ahi GANA. Lo de aca abajo
// es el respaldo, para poder compilar el firmware sin tener Brain-Aura al lado.
//
// Los #ifndef NO son decoracion. Un #define sin envolver redefine el macro y le
// gana al -D, con lo cual hardware.env quedaria ignorado en silencio y el
// firmware saldria con un numero distinto del que cree Brain-Aura. El script
// comprueba eso y CORTA la compilacion si pasa, asi que si agregas un #define
// aca, envolvelo.
//
// Para cambiar un valor se edita hardware.env y se recompila; editarlo solo aca
// hace que los dos lados dejen de coincidir.
// ---------------------------------------------------------------------------

// GPIO Pin definitions
#ifndef GPIO_MOTOR_IN3
#define GPIO_MOTOR_IN3 14
#endif
#ifndef GPIO_MOTOR_IN4
#define GPIO_MOTOR_IN4 12
#endif
#ifndef GPIO_MOTOR_ENB
#define GPIO_MOTOR_ENB 13
#endif
#ifndef GPIO_SERVO
#define GPIO_SERVO 25
#endif
#ifndef GPIO_HEADLIGHTS
#define GPIO_HEADLIGHTS 32
#endif
#ifndef GPIO_REVERSE_LIGHTS
#define GPIO_REVERSE_LIGHTS 33
#endif
#ifndef GPIO_LDR
#define GPIO_LDR 35
#endif
#ifndef GPIO_ESTOP
#define GPIO_ESTOP 4
#endif
#ifndef GPIO_LED_BUILTIN
#define GPIO_LED_BUILTIN 2
#endif
#ifndef GPIO_ULTRASONIC_TRIG
#define GPIO_ULTRASONIC_TRIG 26 // HC-SR04 Trigger pin
#endif
#ifndef GPIO_ULTRASONIC_ECHO
#define GPIO_ULTRASONIC_ECHO 27 // HC-SR04 Echo pin
#endif


// Servo configuration
#ifndef SERVO_CENTER
#define SERVO_CENTER 105
#endif
#ifndef SERVO_LEFT
#define SERVO_LEFT 50
#endif
#ifndef SERVO_RIGHT
#define SERVO_RIGHT 160
#endif
#ifndef SERVO_PWM_FREQ_HZ
#define SERVO_PWM_FREQ_HZ 50
#endif

// Motor configuration
#ifndef MOTOR_SPEED_MAX
#define MOTOR_SPEED_MAX 255
#endif

// UART configuration
#ifndef UART_BAUD_RATE
#define UART_BAUD_RATE 921600
#endif
// A-11: GPIO 9/10 are wired to the module's internal SPI flash (SD_DATA2/SD_DATA3)
// on the ESP32-WROOM-32 and are not even exposed on the DevKit v1 header.
// 16/17 are the standard free choice on this board.
// NOTE: on ESP32-WROVER modules GPIO 16/17 are taken by the PSRAM - if the board
// is ever swapped for a WROVER variant, move these to 18/19.
#ifndef UART_TX_PIN
#define UART_TX_PIN 17
#endif
#ifndef UART_RX_PIN
#define UART_RX_PIN 16
#endif
#ifndef UART_BUF_SIZE
#define UART_BUF_SIZE 1024
#endif

// LDR threshold
#ifndef LDR_THRESHOLD
#define LDR_THRESHOLD 3500
#endif

// Ultrasonic sensor (HC-SR04) configuration
#ifndef ULTRASONIC_MAX_DISTANCE_CM
#define ULTRASONIC_MAX_DISTANCE_CM 400      // Maximum range ~4m
#endif
#ifndef ULTRASONIC_MIN_DISTANCE_CM
#define ULTRASONIC_MIN_DISTANCE_CM 2        // Minimum range ~2cm
#endif
#ifndef ULTRASONIC_OBSTACLE_THRESHOLD_CM
#define ULTRASONIC_OBSTACLE_THRESHOLD_CM 30 // Trigger emergency if object closer than 30cm
#endif

// Link timing - single source of truth (A-3).
// The hierarchy that must always hold is:
//   emission period  <  control command TTL  <  watchdog timeout
#ifndef LINK_EXPECTED_PERIOD_MS
#define LINK_EXPECTED_PERIOD_MS 100                      // 10 Hz, what we recommend to the Brain team
#endif
#ifndef CONTROL_CMD_TTL_MS
#define CONTROL_CMD_TTL_MS (LINK_EXPECTED_PERIOD_MS * 2) // 200 ms
#endif
#ifndef WATCHDOG_TIMEOUT_AUTO_MS
#define WATCHDOG_TIMEOUT_AUTO_MS (LINK_EXPECTED_PERIOD_MS * 3) // 300 ms, always > TTL
#endif
#ifndef WATCHDOG_TIMEOUT_MANUAL_MS
#define WATCHDOG_TIMEOUT_MANUAL_MS 1000 // looser: a human operator is in the loop
#endif

// La jerarquia de arriba deja de ser un comentario y pasa a comprobarse.
// Si hardware.env pone un watchdog por debajo del TTL, el enlace entra en
// FAULT en cada ciclo y el auto no se mueve: mejor no compilar que salir a
// buscar eso con el auto en el piso.
#if WATCHDOG_TIMEOUT_AUTO_MS <= CONTROL_CMD_TTL_MS
#error "WATCHDOG_TIMEOUT_AUTO_MS debe ser > CONTROL_CMD_TTL_MS (revisa BRAIN_WATCHDOG_AUTO_MS en hardware.env)"
#endif
#if WATCHDOG_TIMEOUT_MANUAL_MS <= CONTROL_CMD_TTL_MS
#error "WATCHDOG_TIMEOUT_MANUAL_MS debe ser > CONTROL_CMD_TTL_MS (revisa BRAIN_WATCHDOG_MANUAL_MS en hardware.env)"
#endif

// Los topes del servo tienen que ser coherentes o la direccion queda invertida
// o clavada contra un extremo.
#if !(SERVO_LEFT < SERVO_CENTER && SERVO_CENTER < SERVO_RIGHT)
#error "Debe cumplirse SERVO_LEFT < SERVO_CENTER < SERVO_RIGHT (revisa hardware.env)"
#endif

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
