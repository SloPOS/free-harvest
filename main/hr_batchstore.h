/*
 * hr_batchstore - the device half of the logbook: flash, NVS, and the clock.
 *
 * hr_batchlog.c decides what a batch record contains; this decides where it
 * lives. Split that way so the record format stays host-testable and nothing
 * about SPIFFS or NVS leaks into it.
 */
#ifndef HR_BATCHSTORE_H
#define HR_BATCHSTORE_H

#include "hr_batchlog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mount and read the segment state. Safe to call before SPIFFS is up: the
 * store simply reports itself unready and every call becomes a no-op, which
 * matters because the capture partition is mounted 8 seconds off the boot path
 * to keep flash work away from USB enumeration.
 */
void hr_batchstore_init(void);
bool hr_batchstore_ready(void);

/* Append one completed batch. Rotates segments when the active one fills. */
bool hr_batchstore_append(const hr_batch_t *b);

/* Total bytes held across both segments, for the UI and for capacity checks. */
size_t hr_batchstore_bytes(void);

/*
 * Streaming read, oldest segment first. Same shape as the capture download so
 * the HTTP layer can chunk it without ever holding the whole file.
 */
void  *hr_batchstore_open(void);
int    hr_batchstore_read(void *handle, char *buf, size_t cap);
void   hr_batchstore_close(void *handle);

/* Delete everything. */
bool hr_batchstore_clear(void);

/* ---- in-progress batch, across reboots ---------------------------------- */

/*
 * The open batch lives in NVS as a single record, rewritten as the run
 * proceeds - one record rather than a growing list, so NVS usage stays flat in
 * a 24 KB partition already shared with WiFi, MQTT and recipes.
 */
bool hr_batchstore_save_open(const hr_batch_t *b);
bool hr_batchstore_load_open(hr_batch_t *out);
void hr_batchstore_clear_open(void);

/* ---- clock -------------------------------------------------------------- */

/*
 * The adapter has no RTC and does not use SNTP, so the browser tells it the
 * time - the same way SETDATE already tells the dryer. Set on every page load.
 *
 * hr_time_now() returns 0 until the clock has been set at least once. Zero
 * means "unknown" everywhere downstream and is rendered that way, rather than
 * being turned into a 1970 date that reads as real.
 */
/* Returns false when the value is implausible and was ignored. */
bool     hr_time_set(uint32_t epoch);
uint32_t hr_time_now(void);
bool     hr_time_known(void);

#ifdef __cplusplus
}
#endif

#endif /* HR_BATCHSTORE_H */
