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
 * Invoked for every line the reassembler could NOT turn into a frame: too
 * long for the buffer (`bytes` is the head that did fit), verb missing or
 * over HR_MAX_VERB, more than HR_MAX_FIELDS fields. `bytes` is not
 * NUL-terminated and may contain anything. `why` is a short constant string.
 *
 * For a protocol still being decoded, the lines the parser rejects are the
 * ones worth recording; without this hook they only bumped frames_bad.
 */
typedef void (*hr_reject_cb)(const char *bytes, size_t n, const char *why,
                             void *user);

/* ------------------------------------------------------------------ */
/* Encoded transport (dryer firmware 6.0.644170, after "UNIQUE lH")    */
/* ------------------------------------------------------------------ */
/*
 * Once a 6.0.644170 dryer has been sent "UNIQUE lH" it stops writing the
 * CR-terminated plaintext frames above and switches its OUTBOUND side to a
 * length-prefixed framing (observed on a real machine, 2026-09-15):
 *
 *     ')' 'S' <L1> <L2> <payload ...>          no terminator at all
 *
 * L1 and L2 are two base-64 digits in the alphabet '#'..'b' (0x23..0x62,
 * values 0..63), most significant first, and give the TOTAL frame length in
 * characters INCLUDING the four header characters:
 *
 *     "#7" -> 20    "#G" -> 36    "$+" -> 72    "$3" -> 80    "$C" -> 96
 *
 * so the payload is (length - 4) characters, all printable (0x21..0x62 seen).
 * The payload is opaque here: this layer only frames it, counts it and hands
 * it on verbatim. Nothing in it is interpreted or transformed.
 *
 * The plaintext parser is untouched by this: a frame is only treated as
 * encoded when its first four bytes are a valid header, which no plaintext
 * verb can produce.
 */
#define HR_ENC_HDR        4
#define HR_ENC_DIGIT_MIN  '#'
#define HR_ENC_DIGIT_MAX  'b'
/* Longest encoded frame accepted (total, incl. header). The header can
 * declare up to 4095; anything above this is not a frame we can hold and is
 * discarded. Observed maximum is 96. */
#define HR_ENC_MAX_FRAME  (HR_MAX_FRAME - 1)

/*
 * Decode the four-byte header. Returns the declared total length (>= 4), or
 * -1 if `hdr` is not a ")S" header with two digits in the alphabet.
 */
int hr_enc_decode_len(const char *hdr);

/* Invoked once per complete encoded frame. `frame` is the whole frame -
 * header and payload, `len` bytes, NUL-terminated for convenience. */
typedef void (*hr_enc_cb)(const char *frame, size_t len, void *user);

/* ------------------------------------------------------------------ */
/* Big frames (file blocks)                                            */
/* ------------------------------------------------------------------ */
/*
 * FDFILEBLOCK - the dryer's answer to FILEREAD (hr_files.h) - is the one
 * frame that does not fit the rules above: up to 1024 bytes of file data
 * follow the header, carrying LF, BEL (the dryer's stand-in for CR) and
 * commas, and the whole thing runs past HR_MAX_FRAME. Fed to the line rules
 * it shredded into one bogus frame per CSV line and a stray "A1" - the
 * checksum - which is exactly what the upstream author saw.
 *
 * So a caller that wants blocks lends the stream a SIDE BUFFER. When a
 * plaintext line turns out to be an FDFILEBLOCK header (the fifth comma has
 * arrived and the numbers parse), the stream moves the header there and
 * collects exactly <nbytes> + 2 more bytes regardless of what they are; when
 * an encoded ")S" header declares more than HR_ENC_MAX_FRAME, the frame is
 * collected there instead of being abandoned. Either way the whole frame is
 * handed to `hr_big_cb` (encoded = which kind) and the ordinary path never
 * sees it.
 *
 * With NO side buffer a block's data bytes are swallowed and counted in
 * big_dropped, so they cannot masquerade as frames - safe by default.
 */
typedef void (*hr_big_cb)(const char *frame, size_t len, bool encoded,
                          void *user);

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
    unsigned long noise_bytes;/* non-printable bytes discarded, never framed */
    hr_reject_cb reject;      /* optional; see hr_reject_cb */
    void *reject_user;

    /*
     * Encoded transport state - see the block above. While enc_need is
     * non-zero, `buf` holds the head of an encoded frame and the plaintext
     * rules are suspended until exactly enc_need bytes have arrived.
     */
    size_t enc_need;          /* declared total length being collected; 0 = plaintext */
    bool enc_seen;            /* at least one valid header has been seen */
    unsigned long enc_frames; /* complete encoded frames delivered */
    unsigned long enc_bytes;  /* bytes in them, headers included */
    unsigned long enc_bad;    /* encoded frames abandoned (partial, cut, oversize) */
    hr_enc_cb enc;            /* optional; see hr_enc_cb */
    void *enc_user;

    /*
     * Big-frame side buffer - see hr_big_cb. `big` is the caller's memory;
     * while big_need is non-zero the frame in progress lives there and
     * every other rule is suspended. skip_need swallows a block nobody can
     * hold.
     */
    char *big;                /* NULL: blocks are swallowed */
    size_t big_cap;
    size_t big_len;
    size_t big_need;          /* total length being collected; 0 = not */
    bool big_enc;             /* the frame in `big` is a ")S" frame */
    size_t skip_need;         /* bytes still to swallow */
    unsigned long big_frames; /* whole big frames delivered */
    unsigned long big_dropped;/* blocks swallowed or abandoned */
    hr_big_cb big_cb;
    void *big_user;
} hr_stream_t;

void hr_stream_init(hr_stream_t *s);

/* Register (or clear, with NULL) the rejected-line observer. */
void hr_stream_set_reject_cb(hr_stream_t *s, hr_reject_cb cb, void *user);

/* Register (or clear, with NULL) the encoded-frame observer. */
void hr_stream_set_enc_cb(hr_stream_t *s, hr_enc_cb cb, void *user);

/*
 * Lend the stream a side buffer for big frames (hr_big_cb). `cap` must be
 * at least HR_MAX_FRAME; HR_FILES_BIGBUF (hr_files.h) holds a full block on
 * either transport. Pass NULL to take it back (blocks are swallowed again).
 */
void hr_stream_set_big(hr_stream_t *s, char *buf, size_t cap, hr_big_cb cb,
                       void *user);

/*
 * Throw away a frame that began but never got its terminator, reporting it
 * to the observer as `why`. The caller decides WHEN a partial frame is dead
 * (the stream has no clock): the dryer writes each frame in one go, so bytes
 * that have sat unterminated for more than a second are not the start of
 * anything - they are whatever a host sent before it began speaking the
 * protocol, and without this they were glued to the front of the first
 * real frame. Returns true if something was pending.
 *
 * An encoded frame whose declared length never arrived is dropped by the same
 * call: it is reported to the observer as "enc partial", counted in enc_bad
 * rather than frames_bad, and the next ")S" header starts clean.
 */
bool hr_stream_discard_partial(hr_stream_t *s, const char *why);

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
