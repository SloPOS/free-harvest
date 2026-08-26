/*
 * In-app log capture.
 *
 * Hooks ESP-IDF's logging so recent log lines can be read from the web UI
 * (/api/log). This is how the user diagnoses things like "MQTT won't connect"
 * without attaching a serial cable.
 *
 * Ring buffer of the most recent lines; oldest are overwritten.
 */
#ifndef HR_LOG_H
#define HR_LOG_H

#include <stdbool.h>
#include <stddef.h>

/* Install the log hook. Call early in app_main. */
void hr_log_init(void);

/*
 * Copy the captured log into `out` as a JSON array of strings, newest last.
 * Returns bytes written.
 */
size_t hr_log_json(char *out, size_t cap);

/*
 * Longest single captured line. Measured average is ~65 chars; the widest
 * regular line is the ten-second status heartbeat.
 */
#define HR_LOG_LINE_MAX 144

/* How many lines are currently held, oldest first. */
size_t hr_log_count(void);

/*
 * Copy line `i` (0 = oldest) into `out`. False if the index is out of range.
 *
 * Exists so /api/log can stream the buffer in chunks instead of building the
 * whole JSON document in one allocation - the previous 8KB response buffer
 * capped the log at roughly 80 lines no matter how large the ring was.
 */
bool hr_log_line(size_t i, char *out, size_t cap);

/* Escape a line into a JSON string body (no surrounding quotes). */
size_t hr_log_escape(const char *in, char *out, size_t cap);

/* Append a line directly (for explicit diagnostics). */
void hr_log_addf(const char *fmt, ...);

#endif /* HR_LOG_H */
