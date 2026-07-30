/*
 * hr_protocol - HarvestRight freeze dryer <-> WiFi adapter wire protocol.
 *
 * Framing (from decoded v6 main-app firmware):
 *   ASCII, comma-delimited fields, terminated by CR ('\r').
 *   First field is the command verb. Empty fields are meaningful and are
 *   preserved (the dryer emits e.g. "BATSUM,3,,").
 *
 * This layer is deliberately free of ESP-IDF/FreeRTOS dependencies so it can
 * be unit-tested on a host PC.
 */
#ifndef HR_PROTOCOL_H
#define HR_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HR_MAX_FRAME 512
#define HR_MAX_FIELDS 40
#define HR_MAX_VERB 24

/* A parsed frame. Owns its storage; field pointers point into `raw`. */
typedef struct {
    char raw[HR_MAX_FRAME];
    char verb[HR_MAX_VERB];
    char *field[HR_MAX_FIELDS];
    int nfields;
} hr_frame_t;

/*
 * Parse a single frame body into `out`. `line` may include a trailing CR
 * and/or LF, which are stripped. Returns false if the line is empty, too
 * long, or has no verb.
 */
bool hr_frame_parse(const char *line, hr_frame_t *out);

/* Field accessor. Returns NULL when idx is out of range. */
const char *hr_frame_field(const hr_frame_t *f, int idx);

/* Integer field accessor. Returns `def` when missing/empty/non-numeric. */
long hr_frame_field_int(const hr_frame_t *f, int idx, long def);

/*
 * Rebuild the frame body (verb + comma-joined fields, no CR) into `out`.
 * Parsing tokenises in place, so this is how callers recover the original
 * text. Returns the number of bytes written, or 0 if `cap` is too small.
 */
size_t hr_frame_tostring(const hr_frame_t *f, char *out, size_t cap);

/* ------------------------------------------------------------------ */
/* Byte-stream reassembly                                              */
/* ------------------------------------------------------------------ */

/* Invoked once per complete, successfully parsed frame. */
typedef void (*hr_frame_cb)(const hr_frame_t *frame, void *user);

/*
 * Accumulates bytes arriving in arbitrary chunk sizes (USB CDC reads do not
 * respect frame boundaries) and emits whole frames.
 */
typedef struct {
    char buf[HR_MAX_FRAME];
    size_t len;
    bool overflowed;          /* current frame exceeded the buffer */
    unsigned long frames_ok;  /* frames delivered to the callback */
    unsigned long frames_bad; /* frames dropped (oversized / unparsable) */
} hr_stream_t;

void hr_stream_init(hr_stream_t *s);

/* Feed `n` bytes; `cb` fires for each complete frame found. */
void hr_stream_feed(hr_stream_t *s, const void *data, size_t n, hr_frame_cb cb,
                    void *user);

/* ------------------------------------------------------------------ */
/* Frame construction                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char buf[HR_MAX_FRAME];
    size_t len;
    bool ok; /* cleared if the frame would overflow the buffer */
} hr_builder_t;

/* Start a frame with `verb`. Subsequent hr_build_* calls append fields. */
void hr_build_begin(hr_builder_t *b, const char *verb);

/* Append a string field (may be ""), preceded by a comma. */
void hr_build_str(hr_builder_t *b, const char *s);

/* Append a decimal integer field. */
void hr_build_int(hr_builder_t *b, long v);

/*
 * Terminate the frame with CR and return the buffer, or NULL if the frame
 * overflowed. `out_len` (optional) receives the byte count to transmit.
 */
const char *hr_build_finish(hr_builder_t *b, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* HR_PROTOCOL_H */
