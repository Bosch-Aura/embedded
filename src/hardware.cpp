#include <Arduino.h>
#include "hardware.h"
#include "serial_out.h"
#include <ESP32Servo.h>

static const char *TAG = "hardware";
static Servo steerServo;

void hardware_init(void) {
    // GPIO configuration for outputs
    pinMode(GPIO_MOTOR_IN3, OUTPUT);
    pinMode(GPIO_MOTOR_IN4, OUTPUT);
    pinMode(GPIO_MOTOR_ENB, OUTPUT);
    pinMode(GPIO_HEADLIGHTS, OUTPUT);
    pinMode(GPIO_REVERSE_LIGHTS, OUTPUT);
    pinMode(GPIO_LED_BUILTIN, OUTPUT);
    pinMode(GPIO_ESTOP, INPUT_PULLUP);
    
    // HC-SR04 Ultrasonic sensor
    pinMode(GPIO_ULTRASONIC_TRIG, OUTPUT);
    pinMode(GPIO_ULTRASONIC_ECHO, INPUT);
    digitalWrite(GPIO_ULTRASONIC_TRIG, LOW);
    
    // LDR is analog input, no pinMode needed for GPIO 35
    
    // Initialize GPIO states
    digitalWrite(GPIO_MOTOR_IN3, LOW);
    digitalWrite(GPIO_MOTOR_IN4, LOW);
    digitalWrite(GPIO_HEADLIGHTS, LOW);
    digitalWrite(GPIO_REVERSE_LIGHTS, LOW);
    digitalWrite(GPIO_LED_BUILTIN, LOW);
    
    // Initialize servo
    steerServo.setPeriodHertz(SERVO_PWM_FREQ_HZ);
    steerServo.attach(GPIO_SERVO, 500, 2500);
    steerServo.write(SERVO_CENTER);
    
    // Initialize Serial1 for UART communication
    Serial1.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    
    // Center servo on startup
    steer_set_angle(SERVO_CENTER);
    
    serial_line("Hardware initialized");
}

void motor_set_speed(uint8_t speed) {
    if (speed > MOTOR_SPEED_MAX) {
        speed = MOTOR_SPEED_MAX;
    }
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

void lights_set_headlights(bool on) {
    digitalWrite(GPIO_HEADLIGHTS, on ? HIGH : LOW);
}

void lights_set_reverse(bool on) {
    digitalWrite(GPIO_REVERSE_LIGHTS, on ? HIGH : LOW);
}

uint16_t ldr_read(void) {
    return analogRead(GPIO_LDR);
}

bool estop_is_triggered(void) {
    // E-STOP is active low (pulled up, triggers when grounded)
    return digitalRead(GPIO_ESTOP) == LOW;
}

uint16_t ultrasonic_read_cm(void) {
    // Send trigger pulse (10us HIGH)
    digitalWrite(GPIO_ULTRASONIC_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(GPIO_ULTRASONIC_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(GPIO_ULTRASONIC_TRIG, LOW);
    
    // Read echo pulse duration
    uint32_t duration = pulseIn(GPIO_ULTRASONIC_ECHO, HIGH, 30000); // 30ms timeout (~5m max)
    
    if (duration == 0) {
        // Timeout or no echo
        return 0;
    }
    
    // Calculate distance: distance = (duration * speed_of_sound) / 2
    // speed_of_sound = 343 m/s = 0.0343 cm/us
    // distance_cm = (duration_us * 0.0343) / 2 = duration_us / 58.2
    uint16_t distance_cm = duration / 58;
    
    // Validate range
    if (distance_cm < ULTRASONIC_MIN_DISTANCE_CM || distance_cm > ULTRASONIC_MAX_DISTANCE_CM) {
        return 0; // Invalid reading
    }
    
    return distance_cm;
}
