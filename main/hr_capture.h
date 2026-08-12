/*
 * Persistent capture log.
 *
 * The in-RAM history ring only holds ~16 minutes of frames, which is far too
 * short to record a full freeze-drying cycle (24h+). This module appends every
 * inbound frame to a file in a dedicated SPIFFS partition so a whole cycle can
 * be downloaded afterwards - and survives reboots and power cuts.
 *
 * Format: one line per frame, "<millis>\t<frame body>\n" - the same shape the
 * existing /api/capture download produces, so tooling keeps working.
 */
#ifndef HR_CAPTURE_H
#define HR_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Mount the capture partition. Safe to call once at startup. */
void hr_capture_init(void);

/* True when the log is mounted and writable. */
bool hr_capture_ready(void);

/* Append one frame line. No-op if not ready. */
void hr_capture_append(uint32_t t_ms, const char *body);

/* Bytes currently stored, and the partition's total capacity. */
size_t hr_capture_size(void);
size_t hr_capture_capacity(void);

/* Erase the log. Returns false on failure. */
bool hr_capture_clear(void);

/*
 * Open the log for reading. Returns NULL if unavailable. Caller must call
 * hr_capture_close(). Kept opaque so the HTTP layer can stream it.
 */
void *hr_capture_open(void);
int hr_capture_read(void *handle, char *buf, size_t cap);
void hr_capture_close(void *handle);

#endif /* HR_CAPTURE_H */
