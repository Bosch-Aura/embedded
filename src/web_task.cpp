#include "web_task.h"
#include "hardware.h"
#include "mailbox.h"
#include "messages.h"
#include "supervisor_task.h"
#include "motor_task.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <string.h>
#include "webpage.h"

#define WIFI_AP_SSID "RC-Car-ESP32"
#define WIFI_AP_PASSWORD ""  // Open AP

static mailbox_t *motor_mb = NULL;
static mailbox_t *steer_mb = NULL;
static mailbox_t *lights_mb = NULL;
static mailbox_t *supervisor_mb = NULL;
static WebServer server(80);

void init_wifi_ap(void) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    
    Serial.print("[WebTask] Wi-Fi AP started. SSID: ");
    Serial.print(WIFI_AP_SSID);
    Serial.print(", IP: ");
    Serial.println(WiFi.softAPIP());
    
    // Turn on built-in LED when WiFi AP is ready
    digitalWrite(GPIO_LED_BUILTIN, HIGH);
    Serial.println("[WebTask] Built-in LED turned ON (WiFi AP ready)");
}

void web_task(void *pvParameters) {
    web_task_params_t *params = (web_task_params_t *)pvParameters;
    motor_mb = params->motor_mailbox;
    steer_mb = params->steer_mailbox;
    lights_mb = params->lights_mailbox;
    supervisor_mb = params->supervisor_mailbox;
    
    // Initialize Wi-Fi AP
    init_wifi_ap();
    
    // Root handler
    server.on("/", []() {
        server.send(200, "text/html", webpage);
    });
    
    // Every control handler below calls supervisor_update_heartbeat(): C-4 enables the
    // watchdog in MANUAL mode too, and browser control must feed it or the vehicle
    // would fault while being driven from the web page.

    // Motor control
    server.on("/forward", []() {
        supervisor_update_heartbeat();
        if (motor_mb != NULL) {
            mailbox_write(motor_mb, TOPIC_MOTOR, CMD_SET_SPEED, MOTOR_SPEED_MAX, 100);
            motor_set_direction(true);
        }
        server.send(200, "text/plain", "forward");
    });
    
    server.on("/back", []() {
        supervisor_update_heartbeat();
        if (motor_mb != NULL) {
            mailbox_write(motor_mb, TOPIC_MOTOR, CMD_SET_SPEED, MOTOR_SPEED_MAX, 100);
            motor_set_direction(false);
        }
        server.send(200, "text/plain", "back");
    });
    
    server.on("/driveStop", []() {
        supervisor_update_heartbeat();
        if (motor_mb != NULL) {
            mailbox_write(motor_mb, TOPIC_MOTOR, CMD_STOP, 0, 100);
        }
        server.send(200, "text/plain", "driveStop");
    });
    
    // Steering control with degrees
    server.on("/steer", []() {
        supervisor_update_heartbeat();
        String angle_str = server.arg("angle");
        if (steer_mb != NULL && angle_str.length() > 0) {
            int angle = angle_str.toInt();
            // Clamp angle to valid range (50-135)
            if (angle < SERVO_LEFT) angle = SERVO_LEFT;
            if (angle > SERVO_RIGHT) angle = SERVO_RIGHT;
            mailbox_write(steer_mb, TOPIC_STEER, CMD_SET_STEER, angle, 200);
        }
        server.send(200, "text/plain", "OK");
    });
    
    // Legacy endpoints for backward compatibility
    server.on("/left", []() {
        supervisor_update_heartbeat();
        if (steer_mb != NULL) {
            mailbox_write(steer_mb, TOPIC_STEER, CMD_SET_STEER, SERVO_LEFT, 100);
        }
        server.send(200, "text/plain", "left");
    });
    
    server.on("/right", []() {
        supervisor_update_heartbeat();
        if (steer_mb != NULL) {
            mailbox_write(steer_mb, TOPIC_STEER, CMD_SET_STEER, SERVO_RIGHT, 100);
        }
        server.send(200, "text/plain", "right");
    });
    
    server.on("/steerStop", []() {
        supervisor_update_heartbeat();
        if (steer_mb != NULL) {
            mailbox_write(steer_mb, TOPIC_STEER, CMD_SET_STEER, SERVO_CENTER, 200);
        }
        server.send(200, "text/plain", "steerStop");
    });
    
    // Lights control
    server.on("/LightsOn", []() {
        supervisor_update_heartbeat();
        if (lights_mb != NULL) {
            mailbox_write(lights_mb, TOPIC_LIGHTS, CMD_LIGHTS_ON, 0, 1000);
        }
        server.send(200, "text/plain", "Luces bajas encendidas");
    });
    
    server.on("/LightsOff", []() {
        supervisor_update_heartbeat();
        if (lights_mb != NULL) {
            mailbox_write(lights_mb, TOPIC_LIGHTS, CMD_LIGHTS_OFF, 0, 1000);
        }
        server.send(200, "text/plain", "Luces bajas apagadas");
    });
    
    server.on("/LightsAuto", []() {
        supervisor_update_heartbeat();
        if (lights_mb != NULL) {
            mailbox_write(lights_mb, TOPIC_LIGHTS, CMD_LIGHTS_AUTO, 0, 1000);
        }
        server.send(200, "text/plain", "Luces bajas automaticas");
    });
    
    // Speed control with direction
    server.on("/changeSpeed", []() {
        supervisor_update_heartbeat();
        String speed_str = server.arg("speed");
        String direction_str = server.arg("direction");
        
        if (speed_str.length() > 0) {
            int speed = speed_str.toInt();
            if (speed >= 0 && speed <= MOTOR_SPEED_MAX) {
                if (motor_mb != NULL) {
                    if (speed == 0) {
                        // Stop
                        mailbox_write(motor_mb, TOPIC_MOTOR, CMD_STOP, 0, 200);
                    } else {
                        // Set speed and direction
                        mailbox_write(motor_mb, TOPIC_MOTOR, CMD_SET_SPEED, speed, 200);
                        if (direction_str == "backward") {
                            motor_set_direction(false);
                        } else {
                            motor_set_direction(true); // forward or default
                        }
                    }
                }
                server.send(200, "text/plain", "OK");
                return;
            }
        }
        server.send(400, "text/plain", "Invalid speed value");
    });
    
    // System control
    server.on("/mode", []() {
        supervisor_update_heartbeat();
        String value_str = server.arg("value");
        if (supervisor_mb != NULL && value_str.length() > 0) {
            int32_t mode = (value_str == "AUTO") ? MODE_AUTO : MODE_MANUAL;
            mailbox_write(supervisor_mb, TOPIC_SYSTEM, CMD_SYS_MODE, mode, 5000);
        }
        server.send(200, "text/plain", "OK");
    });
    
    server.on("/arm", []() {
        supervisor_update_heartbeat();
        if (supervisor_mb != NULL) {
            mailbox_write(supervisor_mb, TOPIC_SYSTEM, CMD_SYS_ARM, 0, 5000);
        }
        server.send(200, "text/plain", "ARMED");
    });
    
    server.on("/disarm", []() {
        supervisor_update_heartbeat();
        if (supervisor_mb != NULL) {
            mailbox_write(supervisor_mb, TOPIC_SYSTEM, CMD_SYS_DISARM, 0, 5000);
        }
        server.send(200, "text/plain", "DISARMED");
    });
    
    server.on("/brake", []() {
        supervisor_update_heartbeat();
        motor_task_trigger_emergency();
        server.send(200, "text/plain", "BRAKE");
    });
    
    server.on("/status", []() {
        system_mode_t mode = supervisor_get_mode();
        system_state_t state = supervisor_get_state();
        
        String json = "{\"mode\":\"" + String(mode == MODE_AUTO ? "AUTO" : "MANUAL") + 
                      "\",\"state\":\"" + 
                      String(state == STATE_DISARMED ? "DISARMED" :
                             state == STATE_ARMED ? "ARMED" :
                             state == STATE_RUNNING ? "RUNNING" : "FAULT") + "\"}";
        server.send(200, "application/json", json);
    });
    
    server.onNotFound([]() {
        server.send(404, "text/plain", "Not Found");
    });
    
    server.begin();
    Serial.println("[WebTask] HTTP server started");
    
    // Task loop - handle clients
    while (1) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay
    }
}
