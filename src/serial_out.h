#ifndef SERIAL_OUT_H
#define SERIAL_OUT_H

#ifdef __cplusplus
extern "C" {
#endif

// Single exit point for every line the firmware writes.
//
// Tasks on both cores print. Each line is formatted first and written whole
// under a mutex, otherwise two events can come out glued together
// ("EVENT:...BRAKE_NOWEVENT:STATE_CHANGED:FAULT") and the Jetson drops both.
//
//   log_*   -> USB only. Debug text.
//   event_* -> USB AND the UART to the Jetson. Everything the Jetson must see
//              (state changes, obstacles, watchdog) goes through here, so it
//              arrives whichever of the two ports the Jetson is plugged into.

void serial_out_init(void); // after Serial.begin(), before any task exists

void log_line(const char *text);
void log_linef(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void event_line(const char *text);
void event_linef(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif // SERIAL_OUT_H
