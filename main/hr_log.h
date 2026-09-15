/*
 * In-app log capture.
 *
 * Hooks ESP-IDF's logging so recent log lines can be read from the web UI
 * (/api/log). This is how the user diagnoses things like "MQTT won't connect"
 * without attaching a serial cable.
 *
 * Packed ring buffer (see hr_logring.h) of the most recent lines; oldest are
 * dropped first. Size: CONFIG_HR_LOG_RING_KB.
 */
#ifndef HR_LOG_H
#define HR_LOG_H

#include "hr_logring.h"

#include <stdbool.h>
#include <stddef.h>

/* Install the log hook. Call early in app_main. */
void hr_log_init(void);

/*
 * Optional second destination for every formatted log line (without the
 * trailing newline stripped). For boards whose serial console is not
 * reachable - e.g. one whose only USB port TinyUSB owns - a build can mirror
 * the log to a second CDC interface. The sink is called on the logging
 * task, possibly the TinyUSB task itself, so it MUST NOT block. NULL clears.
 */
typedef void (*hr_log_sink_t)(const char *line, size_t len);
void hr_log_set_sink(hr_log_sink_t sink);

/*
 * Copy the captured log into `out` as a JSON array of strings, newest last.
 * Returns bytes written.
 */
size_t hr_log_json(char *out, size_t cap);

/*
 * Longest single captured line. Measured average is ~65 chars; the widest
 * regular line is the ten-second status heartbeat.
 */
#define HR_LOG_LINE_MAX HR_LOGRING_LINE_MAX

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
