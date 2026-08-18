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
/*
 * Start the capture log. Returns immediately: the SPIFFS mount happens on a
 * background task after a short delay, because doing it synchronously on the
 * boot path breaks USB (see hr_capture.c for the full explanation).
 */
void hr_capture_init(void);

/* How long to wait before mounting, so USB enumeration finishes untouched. */
#define HR_CAPTURE_MOUNT_DELAY_MS 8000

/* Perform the mount synchronously. Normally only called by the mount task. */
void hr_capture_mount_now(void);

/* True when the log is mounted and writable. */
bool hr_capture_ready(void);

/*
 * Append one frame line. No-op if not ready.
 *
 * NON-BLOCKING: the line is copied onto a queue and written by a worker task.
 * This is called from the USB RX callback, and doing file I/O there is what
 * overflowed the TinyUSB task stack and panicked the chip on every frame in
 * v0.3-v0.3.3. Nothing on this path may touch flash directly, ever.
 *
 * If the queue is full the line is DROPPED rather than blocking the USB task;
 * hr_capture_dropped() reports how often, so the loss is visible instead of
 * silent.
 */
void hr_capture_append(uint32_t t_ms, const char *body);

/* Frame lines discarded because the write queue was full. */
unsigned long hr_capture_dropped(void);

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
