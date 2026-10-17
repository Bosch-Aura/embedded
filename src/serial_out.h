#ifndef SERIAL_OUT_H
#define SERIAL_OUT_H

#ifdef __cplusplus
extern "C" {
#endif

// Single exit point for every line written to Serial (USB).
//
// Several tasks on both cores print to the same port. Serial.println() is TWO
// writes (the text, then "\r\n"), so another task could slip its own line in
// between and the Brain received two events glued together, e.g.
//   EVENT:CMD_EXECUTED:EMERGENCY_BRAKEEVENT:STATE_CHANGED:FAULT
// which it can only throw away. Here each line is formatted first and written
// whole while holding a mutex, so lines never interleave.

// Call once, right after Serial.begin() and before any task exists.
void serial_out_init(void);

// Writes `text` followed by "\r\n" as one atomic line.
void serial_line(const char *text);

// printf-style version of serial_line(). Lines longer than the internal
// buffer are truncated, never split.
void serial_linef(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif // SERIAL_OUT_H
