/*
 * hr_history - bounded record of recent frames for the decoding web UI.
 *
 * Two things the UI needs, both maintained here so they can be unit-tested
 * without a browser or hardware:
 *
 *   1. A ring buffer of the most recent N frames, each stamped with a
 *      monotonically increasing sequence number so a client using
 *      Server-Sent Events can ask "give me everything after seq X".
 *
 *   2. A per-verb table: the latest raw body seen for each verb, how many
 *      times it has appeared, and - for repeating frames like STAT - which
 *      field positions changed between the last two occurrences (the
 *      "field diff" that makes decoding fast).
 *
 * No ESP-IDF / FreeRTOS dependencies. Callers are responsible for locking if
 * used from multiple tasks.
 */
#ifndef HR_HISTORY_H
#define HR_HISTORY_H

#include "hr_protocol.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HR_HIST_CAP 64      /* ring buffer depth (power of two preferred) */
#define HR_HIST_VERBS 48    /* distinct verbs tracked */
#define HR_HIST_BODY 160    /* stored bytes per frame body (truncated) */

typedef struct {
    uint32_t seq;                 /* 1-based; 0 means "empty slot" */
    uint32_t t_ms;                /* arrival time */
    char verb[HR_MAX_VERB];
    char body[HR_HIST_BODY];      /* verb + comma-joined fields, no CR */
    uint8_t nfields;
} hr_hist_entry_t;

typedef struct {
    char verb[HR_MAX_VERB];
    char last_body[HR_HIST_BODY];
    uint32_t count;
    uint32_t last_seq;
    uint8_t nfields;
    /* Bit i set => field i differs from the previous occurrence of this verb.
     * Only the first 32 fields are tracked. */
    uint32_t changed_mask;
} hr_hist_verb_t;

typedef struct {
    hr_hist_entry_t ring[HR_HIST_CAP];
    hr_hist_verb_t verbs[HR_HIST_VERBS];
    int nverbs;
    uint32_t seq;        /* last assigned sequence number */
    uint32_t total;      /* total frames recorded */
} hr_history_t;

void hr_history_init(hr_history_t *h);

/* Record one parsed frame at time t_ms. Assigns and returns its seq. */
uint32_t hr_history_add(hr_history_t *h, const hr_frame_t *f, uint32_t t_ms);

/* Latest sequence number assigned (0 if none). */
uint32_t hr_history_latest_seq(const hr_history_t *h);

/*
 * Copy entries with seq > `after` into `out` (up to `max`), oldest first.
 * Returns the number copied. Entries older than the ring depth are lost.
 */
int hr_history_since(const hr_history_t *h, uint32_t after,
                     hr_hist_entry_t *out, int max);

/* Look up the per-verb record, or NULL if that verb hasn't been seen. */
const hr_hist_verb_t *hr_history_verb(const hr_history_t *h, const char *verb);

/*
 * Escape `in` into `out` as the body of a JSON string (no surrounding
 * quotes). Handles " and \ and drops control characters. Always NUL
 * terminates. Returns bytes written (excluding the NUL).
 */
size_t hr_json_escape(const char *in, char *out, size_t cap);

/*
 * Serialise one history entry as a JSON object into `out`:
 *   {"seq":N,"t":M,"verb":"..","body":"..","n":F}
 * Returns bytes written, or 0 if it would not fit.
 */
size_t hr_hist_entry_json(const hr_hist_entry_t *e, char *out, size_t cap);

/*
 * Decode application/x-www-form-urlencoded text in place: "+" becomes space
 * and "%XX" becomes the byte 0xXX. Malformed "%" sequences are left as-is.
 * NUL terminates. Safe for use on the buffer returned by an HTTP form field
 * (needed so WiFi passwords with %-escaped characters are stored correctly).
 */
void hr_url_decode(char *s);

#ifdef __cplusplus
}
#endif

#endif /* HR_HISTORY_H */
