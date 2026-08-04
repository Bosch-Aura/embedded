#include "link_rx_task.h"
#include "hardware.h"
#include "mailbox.h"
#include "messages.h"
#include "motor_task.h"
#include "supervisor_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

#define UART_RX_TIMEOUT_MS 100

static mailbox_t *motor_mb = NULL;
static mailbox_t *steer_mb = NULL;
static mailbox_t *lights_mb = NULL;
static mailbox_t *supervisor_mb = NULL;

// Parse UART message format: CHANNEL:COMMAND[:VALUE]
// M-14: works on a stack buffer, no strdup/malloc in the hot path (this is the
// task that also handles the emergency brake).
// A-9: returns the value both as raw text and as integer, so callers can tell
// "AUTO" from a number instead of silently getting atoi("AUTO") == 0.
static bool parse_uart_message(const char *msg, char *channel, char *cmd,
                               char *value_str, size_t value_str_len, int32_t *value) {
    char buf[UART_BUF_SIZE];
    size_t n = strlen(msg);
    if (n == 0 || n >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, msg, n + 1);

    // Remove newline/carriage return
    char *p = strchr(buf, '\n');
    if (p) *p = '\0';
    p = strchr(buf, '\r');
    if (p) *p = '\0';

    // Parse channel (single character)
    if (strlen(buf) < 3) {
        return false;
    }
    *channel = buf[0];

    // Find command start
    char *cmd_start = strchr(buf, ':');
    if (cmd_start == NULL) {
        return false;
    }
    cmd_start++;

    // Find value separator
    char *value_start = strchr(cmd_start, ':');
    if (value_start != NULL) {
        *value_start = '\0';
        value_start++;
        *value = atoi(value_start);
        strncpy(value_str, value_start, value_str_len - 1);
        value_str[value_str_len - 1] = '\0';
    } else {
        *value = 0;
        value_str[0] = '\0';
    }

    strncpy(cmd, cmd_start, 32);
    cmd[31] = '\0';

    return true;
}

// A-6: line accumulator with state that PERSISTS across polls.
// The previous version returned whatever bytes happened to have arrived, so a
// message split across two polls (very likely at 921600 baud with a 10 ms poll)
// was silently mangled into two unparseable fragments.
typedef struct {
    char buf[UART_BUF_SIZE];
    int  len;
} line_accum_t;

// Returns true and leaves a complete line (terminator stripped) in acc->buf
static bool read_line_accum(Stream &stream, line_accum_t *acc) {
    while (stream.available() > 0) {
        char c = (char)stream.read();
        if (c == '\n' || c == '\r') {
            if (acc->len == 0) {
                continue; // ignore stray terminators / empty lines
            }
            acc->buf[acc->len] = '\0';
            acc->len = 0;
            return true; // complete line
        }
        if (acc->len < (int)sizeof(acc->buf) - 1) {
            acc->buf[acc->len++] = c;
        } else {
            acc->len = 0; // line too long: discard it
            Serial.println("[LinkRxTask] Line overflow, discarded");
        }
    }
    return false; // still incomplete, keep the bytes for the next poll
}

void link_rx_task(void *pvParameters) {
    link_rx_params_t *params = (link_rx_params_t *)pvParameters;
    motor_mb = params->motor_mailbox;
    steer_mb = params->steer_mailbox;
    lights_mb = params->lights_mailbox;
    supervisor_mb = params->supervisor_mailbox;
    
    // A-6: one accumulator per stream. They must not be shared, or a partial
    // line on one link would be corrupted by bytes from the other.
    static line_accum_t acc_usb;
    static line_accum_t acc_uart1;
    acc_usb.len = 0;
    acc_uart1.len = 0;

    const char *data = NULL;

    Serial.println("[LinkRxTask] LinkRx task started");

    while (1) {
        // Read UART data from USB Serial first (testing over single USB cable), then from Serial1
        data = NULL;
        if (read_line_accum(Serial, &acc_usb)) {
            data = acc_usb.buf;
        } else if (read_line_accum(Serial1, &acc_uart1)) {
            data = acc_uart1.buf;
        }

        if (data != NULL) {
            char channel;
            char cmd[32];
            char value_str[32];
            int32_t value = 0;
            // A-6: only a command that is actually dispatched to a known channel
            // feeds the watchdog. A stream of garbage must not keep it alive.
            bool command_dispatched = false;

            if (parse_uart_message(data, &channel, cmd, value_str, sizeof(value_str), &value)) {
                // Route based on channel
                switch (channel) {
                    case CHANNEL_EMERGENCY:
                        if (strcmp(cmd, "BRAKE_NOW") == 0 || strcmp(cmd, "STOP") == 0) {
                            // Emergency: send notification to MotorTask
                            Serial.println("EVENT:CMD_RECEIVED:BRAKE_NOW");
                            Serial.flush();
                            motor_task_trigger_emergency();
                            command_dispatched = true;
                            Serial.println("[LinkRxTask] Emergency brake triggered via UART");
                        }
                        break;
                        
                    case CHANNEL_CONTROL: {
                        // Always send commands to mailboxes - tasks will validate state before execution
                        if (strcmp(cmd, "SET_SPEED") == 0) {
                            Serial.print("EVENT:CMD_RECEIVED:SET_SPEED:");
                            Serial.println(value);
                            Serial.flush();
                            if (motor_mb != NULL) {
                                mailbox_write(motor_mb, TOPIC_MOTOR, CMD_SET_SPEED, value, CONTROL_CMD_TTL_MS);
                                command_dispatched = true;
                            }
                        } else if (strcmp(cmd, "SET_STEER") == 0) {
                            Serial.print("EVENT:CMD_RECEIVED:SET_STEER:");
                            Serial.println(value);
                            Serial.flush();
                            if (steer_mb != NULL) {
                                mailbox_write(steer_mb, TOPIC_STEER, CMD_SET_STEER, value, CONTROL_CMD_TTL_MS);
                                command_dispatched = true;
                            }
                        }
                        break;
                    }
                        
                    case CHANNEL_MANAGEMENT:
                        if (strcmp(cmd, "SYS_ARM") == 0) {
                            if (supervisor_mb != NULL) {
                                Serial.println("EVENT:CMD_RECEIVED:SYS_ARM");
                                Serial.flush();
                                mailbox_write(supervisor_mb, TOPIC_SYSTEM, CMD_SYS_ARM, 0, 5000);
                                command_dispatched = true;
                                Serial.println("[LinkRxTask] SYS_ARM command");
                            }
                        } else if (strcmp(cmd, "SYS_DISARM") == 0) {
                            if (supervisor_mb != NULL) {
                                Serial.println("EVENT:CMD_RECEIVED:SYS_DISARM");
                                Serial.flush();
                                mailbox_write(supervisor_mb, TOPIC_SYSTEM, CMD_SYS_DISARM, 0, 5000);
                                command_dispatched = true;
                                Serial.println("[LinkRxTask] SYS_DISARM command");
                            }
                        } else if (strcmp(cmd, "SYS_MODE") == 0) {
                            if (supervisor_mb != NULL) {
                                // A-9: compare the RAW TEXT. The old code used atoi(), which
                                // returns 0 for any non-numeric string, so "M:SYS_MODE:AUTO"
                                // silently selected MANUAL - the exact opposite of the request,
                                // leaving the system without the AUTO watchdog.
                                int32_t mode;
                                if (strcmp(value_str, "AUTO") == 0 || strcmp(value_str, "1") == 0) {
                                    mode = MODE_AUTO;
                                } else if (strcmp(value_str, "MANUAL") == 0 || strcmp(value_str, "0") == 0) {
                                    mode = MODE_MANUAL;
                                } else {
                                    // Never assume a mode for an unknown value
                                    Serial.println("EVENT:CMD_REJECTED:SYS_MODE:BAD_VALUE");
                                    Serial.flush();
                                    break;
                                }
                                Serial.print("EVENT:CMD_RECEIVED:SYS_MODE:");
                                Serial.println(mode == MODE_AUTO ? "AUTO" : "MANUAL");
                                Serial.flush();
                                mailbox_write(supervisor_mb, TOPIC_SYSTEM, CMD_SYS_MODE, mode, 5000);
                                command_dispatched = true;
                                Serial.print("[LinkRxTask] SYS_MODE: ");
                                Serial.println(mode == MODE_AUTO ? "AUTO" : "MANUAL");
                            }
                        } else if (strcmp(cmd, "LIGHTS_ON") == 0) {
                            if (lights_mb != NULL) {
                                Serial.println("EVENT:CMD_RECEIVED:LIGHTS_ON");
                                Serial.flush();
                                mailbox_write(lights_mb, TOPIC_LIGHTS, CMD_LIGHTS_ON, 0, 1000);
                                command_dispatched = true;
                                Serial.println("[LinkRxTask] LIGHTS_ON command");
                            }
                        } else if (strcmp(cmd, "LIGHTS_OFF") == 0) {
                            if (lights_mb != NULL) {
                                Serial.println("EVENT:CMD_RECEIVED:LIGHTS_OFF");
                                Serial.flush();
                                mailbox_write(lights_mb, TOPIC_LIGHTS, CMD_LIGHTS_OFF, 0, 1000);
                                command_dispatched = true;
                                Serial.println("[LinkRxTask] LIGHTS_OFF command");
                            }
                        } else if (strcmp(cmd, "LIGHTS_AUTO") == 0) {
                            if (lights_mb != NULL) {
                                Serial.println("EVENT:CMD_RECEIVED:LIGHTS_AUTO");
                                Serial.flush();
                                mailbox_write(lights_mb, TOPIC_LIGHTS, CMD_LIGHTS_AUTO, 0, 1000);
                                command_dispatched = true;
                                Serial.println("[LinkRxTask] LIGHTS_AUTO command");
                            }
                        }
                        break;
                        
                    default:
                        Serial.print("[LinkRxTask] Unknown channel: ");
                        Serial.println(channel);
                        break;
                }
            } else {
                Serial.print("[LinkRxTask] Failed to parse message: ");
                Serial.println(data);
            }

            // A-6: feed the watchdog only for commands we actually understood and
            // routed. Previously any parseable garbage kept the watchdog alive.
            if (command_dispatched) {
                supervisor_update_heartbeat();
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay to avoid busy waiting
    }
}
