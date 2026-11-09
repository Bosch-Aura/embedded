#include "link_rx_task.h"
#include "control_task.h"
#include "hardware.h"
#include "messages.h"
#include "serial_out.h"
#include "supervisor_task.h"
#include "ultrasonic_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#define LINK_RX_POLL_MS 10

// Line accumulator whose state PERSISTS across polls: at 921600 baud a line is
// often split between two polls, and parsing whatever happened to arrive
// mangles it into two fragments.
typedef struct {
    char buf[UART_BUF_SIZE];
    int len;
    bool overflowed; // swallowing the rest of an over-long line
} line_accum_t;

// Returns true with a complete line (terminator stripped) in acc->buf.
static bool read_line(Stream &stream, line_accum_t *acc)
{
    while (stream.available() > 0)
    {
        const char c = (char)stream.read();
        if (c == '\n' || c == '\r')
        {
            const bool complete = !acc->overflowed && acc->len > 0;
            acc->buf[acc->len] = '\0';
            acc->len = 0;
            acc->overflowed = false;
            if (complete)
            {
                return true;
            }
            continue; // empty line, or the end of an over-long one: drop it
        }
        if (acc->overflowed)
        {
            continue;
        }
        if (acc->len < (int)sizeof(acc->buf) - 1)
        {
            acc->buf[acc->len++] = c;
        }
        else
        {
            // Log once and drop the WHOLE line, so its tail is not parsed as
            // a fresh command.
            acc->overflowed = true;
            acc->len = 0;
            log_line("[LinkRx] line overflow, discarding until end of line");
        }
    }
    return false;
}

// Parses "<CH>:<CMD>[:<VALUE>]" in place and runs it. Returns true if the
// command was understood: only those feed the watchdog, a stream of garbage
// must not keep the link "alive".
static bool dispatch(char *line)
{
    if (strlen(line) < 3 || line[1] != ':')
    {
        return false;
    }
    const char channel = line[0];
    char *cmd = line + 2;
    char *sep = strchr(cmd, ':');
    int32_t value = 0;
    if (sep != NULL)
    {
        *sep = '\0';
        value = atoi(sep + 1);
    }

    switch (channel)
    {
    case CHANNEL_EMERGENCY:
        if (strcmp(cmd, "BRAKE_NOW") == 0 || strcmp(cmd, "STOP") == 0)
        {
            control_brake();
            return true;
        }
        break;

    case CHANNEL_CONTROL:
        if (strcmp(cmd, "SET_SPEED") == 0)
        {
            control_set_speed(value);
            return true;
        }
        if (strcmp(cmd, "SET_STEER") == 0)
        {
            control_set_steer(value);
            return true;
        }
        if (strcmp(cmd, "SET_DIR") == 0) // 1 forward, 0 backward
        {
            control_set_direction(value != 0);
            return true;
        }
        break;

    case CHANNEL_MANAGEMENT:
        if (strcmp(cmd, "PING") == 0) // liveness only
        {
            return true;
        }
        if (strcmp(cmd, "STATUS") == 0)
        {
            // Snapshot for a client that just connected and cannot rely on
            // the board rebooting (and replaying its events) when it opens
            // the port.
            event_linef("EVENT:STATUS:%s:SYS_STOP=%d:OBSTACLE=%d:SPEED=%u:HELD=%u:ULTRASONIC=%s",
                        supervisor_state_name(supervisor_get_state()),
                        supervisor_stop_latched() ? 1 : 0,
                        supervisor_obstacle_active() ? 1 : 0,
                        (unsigned)control_latched_speed(),
                        (unsigned)control_held_speed(),
                        ultrasonic_health_name(ultrasonic_health()));
            return true;
        }
        if (strcmp(cmd, "SYS_ARM") == 0)
        {
            supervisor_request_arm();
            return true;
        }
        if (strcmp(cmd, "SYS_DISARM") == 0)
        {
            supervisor_request_disarm();
            return true;
        }
        if (strcmp(cmd, "SYS_STOP") == 0)
        {
            supervisor_latch_stop();
            event_line("EVENT:CMD_EXECUTED:SYS_STOP"); // also if already latched
            return true;
        }
        if (strcmp(cmd, "OBSTACLE_RESET") == 0)
        {
            control_forget_held_speed(); // BEFORE the release (see ControlTask)
            ultrasonic_request_reset();
            return true;
        }
        if (strcmp(cmd, "SYS_RESUME") == 0)
        {
            // Releases the latch only. Nothing latched resumes: the car moves
            // again with the next C:SET_SPEED, and stays STOPPED while an
            // obstacle is still ahead.
            supervisor_release_stop();
            event_line("EVENT:CMD_EXECUTED:SYS_RESUME"); // also if nothing was latched
            return true;
        }
        break;

    default:
        break;
    }
    return false;
}

static void poll(Stream &stream, line_accum_t *acc)
{
    while (read_line(stream, acc))
    {
        if (dispatch(acc->buf))
        {
            supervisor_feed_watchdog();
        }
        else
        {
            log_line("[LinkRx] unknown command, ignored");
        }
    }
}

void link_rx_task(void *pvParameters)
{
    (void)pvParameters;
    // One accumulator per port: sharing one would mix partial lines.
    static line_accum_t acc_usb;
    static line_accum_t acc_uart;

    log_line("[LinkRx] started");

    while (1)
    {
        poll(Serial, &acc_usb);  // USB (also used for bench testing)
        poll(Serial1, &acc_uart); // UART to the Jetson
        vTaskDelay(pdMS_TO_TICKS(LINK_RX_POLL_MS));
    }
}
