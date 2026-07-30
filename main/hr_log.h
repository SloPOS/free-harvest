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

#include <stddef.h>

/* Install the log hook. Call early in app_main. */
void hr_log_init(void);

/*
 * Copy the captured log into `out` as a JSON array of strings, newest last.
 * Returns bytes written.
 */
size_t hr_log_json(char *out, size_t cap);

/* Append a line directly (for explicit diagnostics). */
void hr_log_addf(const char *fmt, ...);

#endif /* HR_LOG_H */
