/*
 * hr_encring - the last N encoded frames, for /api/enc.
 *
 * The 6.0.644170 encoded transport (hr_protocol.h) is being captured, not
 * decoded: what matters is that every complete frame can be pulled off the
 * device intact, with when it arrived and how long it said it was. The
 * capture log has all of them; this ring is the live view of the most
 * recent ones, so a browser can watch frames come in without downloading
 * the whole log.
 *
 * Frames are stored verbatim (header included) up to HR_ENCRING_RAW bytes;
 * a longer frame is truncated HERE ONLY and says so (kept < len). The
 * capture log holds the whole frame regardless.
 *
 * No ESP-IDF dependencies; unit-tested on the host.
 */
#ifndef HR_ENCRING_H
#define HR_ENCRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frames kept, and bytes of each kept in RAM. The longest frame seen on a
 * real machine is 96 characters; the common ones are 20 and 80. 32 frames
 * covers ~4 minutes of the idle cadence (one 20-char every 10 s, one 80-char
 * every 15 s) - enough to see the pattern, small enough not to matter. */
#define HR_ENCRING_N   32
#define HR_ENCRING_RAW 160

typedef struct {
    uint32_t t_ms;              /* adapter uptime when the frame completed */
    uint16_t len;               /* declared total length (= bytes received) */
    uint16_t kept;              /* bytes of raw[] that are valid, <= len */
    char     raw[HR_ENCRING_RAW + 1]; /* NUL-terminated */
} hr_enc_rec_t;

typedef struct {
    hr_enc_rec_t  rec[HR_ENCRING_N];
    unsigned      head;         /* slot the NEXT frame goes into */
    unsigned      count;        /* valid slots, <= HR_ENCRING_N */
    unsigned long total_frames; /* every frame ever pushed, ring or not */
    unsigned long total_bytes;
} hr_encring_t;

void hr_encring_init(hr_encring_t *r);

/* Record one complete frame. `frame` need not be NUL-terminated. */
void hr_encring_push(hr_encring_t *r, uint32_t t_ms, const char *frame,
                     size_t len);

/* Frames currently held (0..HR_ENCRING_N). */
unsigned hr_encring_count(const hr_encring_t *r);

/*
 * Copy out frame `i`, where 0 is the OLDEST held and count-1 the newest.
 * Returns false when `i` is out of range.
 */
bool hr_encring_get(const hr_encring_t *r, unsigned i, hr_enc_rec_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HR_ENCRING_H */
