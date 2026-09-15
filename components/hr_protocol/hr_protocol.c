#include "hr_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool hr_frame_parse(const char *line, hr_frame_t *out)
{
    if (line == NULL || out == NULL) {
        return false;
    }

    size_t len = strlen(line);
    /* Strip the CR/LF terminator the wire format uses. */
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    if (len == 0 || len >= HR_MAX_FRAME) {
        return false;
    }

    memcpy(out->raw, line, len);
    out->raw[len] = '\0';
    out->nfields = 0;

    /* The verb runs up to the first comma (or end of frame). */
    char *cursor = out->raw;
    char *comma = strchr(cursor, ',');
    size_t verb_len = comma ? (size_t)(comma - cursor) : len;
    if (verb_len == 0 || verb_len >= HR_MAX_VERB) {
        return false;
    }
    memcpy(out->verb, cursor, verb_len);
    out->verb[verb_len] = '\0';

    if (comma == NULL) {
        return true; /* verb-only frame */
    }

    /*
     * Split the remainder on commas, keeping empty fields: a trailing comma
     * yields a final empty field, which the dryer uses meaningfully.
     */
    cursor = comma + 1;
    for (;;) {
        if (out->nfields >= HR_MAX_FIELDS) {
            return false;
        }
        out->field[out->nfields++] = cursor;

        comma = strchr(cursor, ',');
        if (comma == NULL) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }

    return true;
}

void hr_stream_init(hr_stream_t *s)
{
    if (s == NULL) {
        return;
    }
    s->len = 0;
    s->overflowed = false;
    s->frames_ok = 0;
    s->frames_bad = 0;
    s->noise_bytes = 0;
    s->reject = NULL;
    s->reject_user = NULL;
    s->enc_need = 0;
    s->enc_seen = false;
    s->enc_frames = 0;
    s->enc_bytes = 0;
    s->enc_bad = 0;
    s->enc = NULL;
    s->enc_user = NULL;
}

void hr_stream_set_reject_cb(hr_stream_t *s, hr_reject_cb cb, void *user)
{
    if (s == NULL) {
        return;
    }
    s->reject = cb;
    s->reject_user = user;
}

void hr_stream_set_enc_cb(hr_stream_t *s, hr_enc_cb cb, void *user)
{
    if (s == NULL) {
        return;
    }
    s->enc = cb;
    s->enc_user = user;
}

static void reject(hr_stream_t *s, const char *why)
{
    s->frames_bad++;
    if (s->reject != NULL) {
        s->reject(s->buf, s->len, why, s->reject_user);
    }
}

int hr_enc_decode_len(const char *hdr)
{
    if (hdr == NULL || hdr[0] != ')' || hdr[1] != 'S') {
        return -1;
    }
    const unsigned char d1 = (unsigned char)hdr[2];
    const unsigned char d2 = (unsigned char)hdr[3];
    if (d1 < HR_ENC_DIGIT_MIN || d1 > HR_ENC_DIGIT_MAX ||
        d2 < HR_ENC_DIGIT_MIN || d2 > HR_ENC_DIGIT_MAX) {
        return -1;
    }
    int len = (d1 - HR_ENC_DIGIT_MIN) * 64 + (d2 - HR_ENC_DIGIT_MIN);
    return len >= HR_ENC_HDR ? len : -1;
}

/*
 * Give up on the encoded frame being collected. The head that arrived goes
 * to the reject observer so it is still recorded, but it is counted in
 * enc_bad: a cut encoded frame says nothing about the plaintext parser and
 * must not be mistaken for the "frames_bad climbing" symptom of one.
 */
static void enc_abandon(hr_stream_t *s, const char *why)
{
    s->enc_bad++;
    if (s->reject != NULL && s->len > 0) {
        s->reject(s->buf, s->len, why, s->reject_user);
    }
    s->len = 0;
    s->enc_need = 0;
    s->overflowed = false;
}

static void enc_deliver(hr_stream_t *s)
{
    s->buf[s->len] = '\0';
    s->enc_frames++;
    s->enc_bytes += s->len;
    if (s->enc != NULL) {
        s->enc(s->buf, s->len, s->enc_user);
    }
    s->len = 0;
    s->enc_need = 0;
}

bool hr_stream_discard_partial(hr_stream_t *s, const char *why)
{
    if (s == NULL || (s->len == 0 && !s->overflowed)) {
        return false;
    }
    if (s->enc_need > 0) {
        enc_abandon(s, "enc partial");
        return true;
    }
    reject(s, why != NULL ? why : "discarded");
    s->len = 0;
    s->overflowed = false;
    return true;
}

void hr_stream_feed(hr_stream_t *s, const void *data, size_t n, hr_frame_cb cb,
                    void *user)
{
    if (s == NULL || data == NULL) {
        return;
    }

    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < n; i++) {
        const unsigned char uc = p[i];
        char ch = (char)uc;

        /*
         * ENCODED FRAME IN PROGRESS. The header declared how many bytes the
         * frame has; take exactly that many, then hand the whole thing on.
         * The frame has no terminator, so CR/LF here are not "end of frame"
         * - they, like any non-printable byte, are something that cannot
         * belong to the frame and mean it was cut short.
         */
        if (s->enc_need > 0) {
            if (uc < 0x20 || uc >= 0x7f) {
                enc_abandon(s, "enc interrupted");
                if (ch != '\r' && ch != '\n') {
                    s->noise_bytes++;
                }
                continue;
            }
            s->buf[s->len++] = ch;
            if (s->len == s->enc_need) {
                enc_deliver(s);
            }
            continue;
        }

        if (ch != '\r' && ch != '\n') {
            /*
             * The wire protocol is printable ASCII, comma-separated, CR-
             * terminated. A byte outside that range is not part of any
             * frame - line noise, a bootloader probe (esptool's SLIP sync
             * is 0xC0 0x00 0x08 ... 0x55 0x55), a host stack hiccup - and
             * must not be kept: with no terminator of its own it used to
             * sit in the buffer and glue itself to the front of the next
             * real frame, which then parsed as an unknown verb. On the
             * bench the frame it spoiled was the dryer's first REQINFO, so
             * WIFIINFO was never sent for it. Drop the byte, and with it
             * whatever partial frame it interrupted; the next printable
             * byte starts clean.
             */
            if (uc < 0x20 || uc >= 0x7f) {
                if (s->len > 0 || s->overflowed) {
                    reject(s, "binary noise");
                    s->len = 0;
                    s->overflowed = false;
                }
                s->noise_bytes++;
                continue;
            }
            if (s->len < HR_MAX_FRAME - 1) {
                s->buf[s->len++] = ch;
            } else {
                /* Frame is longer than we can hold; discard it at the
                 * terminator rather than emitting a truncated command. */
                s->overflowed = true;
            }

            /*
             * ENCODED HEADER AT FRAME START. Four bytes in, ")S" followed by
             * two valid digits is the 6.0.644170 encoded transport, which no
             * plaintext verb begins with. Switch to counting bytes; a
             * header-only frame (length 4) is complete already.
             */
            if (s->len == HR_ENC_HDR && s->buf[0] == ')' && s->buf[1] == 'S') {
                int need = hr_enc_decode_len(s->buf);
                if (need > HR_ENC_MAX_FRAME) {
                    s->enc_seen = true;
                    s->enc_need = (size_t)need; /* so the observer sees why */
                    enc_abandon(s, "enc too long");
                } else if (need >= HR_ENC_HDR) {
                    s->enc_seen = true;
                    s->enc_need = (size_t)need;
                    if (s->len == s->enc_need) {
                        enc_deliver(s);
                    }
                }
                continue;
            }

            /*
             * RESYNC. Once the dryer has switched to the encoded transport, a
             * ")S" arriving with bytes already pending means those bytes were
             * the tail of something whose declared length was wrong, or debris
             * from a cut frame. Drop them (observably) and let the header
             * start clean, instead of waiting for the stale timer. Gated on
             * enc_seen so a dryer that never sent an encoded frame - every
             * 6.0.641041 machine - keeps the plaintext rules exactly as they
             * were, ")S" inside a name included.
             */
            if (s->enc_seen && ch == 'S' && s->len >= 3 &&
                s->buf[s->len - 2] == ')' && !s->overflowed) {
                s->len -= 2;
                enc_abandon(s, "enc resync");
                s->buf[0] = ')';
                s->buf[1] = 'S';
                s->len = 2;
            }
            continue;
        }

        /* Terminator reached: close out whatever we accumulated. */
        if (s->overflowed) {
            reject(s, "too long");
        } else if (s->len > 0) {
            s->buf[s->len] = '\0';
            hr_frame_t frame;
            if (hr_frame_parse(s->buf, &frame)) {
                s->frames_ok++;
                if (cb != NULL) {
                    cb(&frame, user);
                }
            } else {
                reject(s, "unparsable");
            }
        }
        /* len == 0 with no overflow: empty frame, silently ignored. */
        s->len = 0;
        s->overflowed = false;
    }
}

/* Append raw bytes, reserving room for the CR terminator and NUL. */
static void build_append(hr_builder_t *b, const char *s, size_t n)
{
    if (!b->ok) {
        return;
    }
    if (b->len + n + 2 > HR_MAX_FRAME) {
        b->ok = false;
        return;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

void hr_build_begin(hr_builder_t *b, const char *verb)
{
    if (b == NULL) {
        return;
    }
    b->len = 0;
    b->buf[0] = '\0';
    b->ok = (verb != NULL && *verb != '\0');
    if (b->ok) {
        build_append(b, verb, strlen(verb));
    }
}

/*
 * Field separator for OUTBOUND frames is a SPACE, not a comma.
 *
 * The protocol is asymmetric, which we only learned by interrogating a genuine
 * HarvestRight adapter (2026-08-20). It sends:
 *
 *     STATE 1 0
 *     UNIQUE lH
 *     WIFIINFO 1 0 "" 0 HR_aabbccddeeff 0 0 161
 *
 * while the dryer's own frames TO us remain comma-delimited (STAT,1,0,0,...),
 * which is what hr_frame_parse() still splits on.
 *
 * This matters more than a cosmetic difference. If the dryer tokenised on
 * commas, "STATE 1 0" would be a single token and could never match the verb
 * STATE - so its inbound parser must split on whitespace. Which means every
 * comma-delimited command we have ever sent, including GOTIT, arrived as one
 * unrecognised token and was silently discarded ("Command not found = %s").
 */
void hr_build_str(hr_builder_t *b, const char *s)
{
    if (b == NULL) {
        return;
    }
    build_append(b, " ", 1);
    /*
     * An EMPTY field must be written as "" - two quote characters - not as
     * nothing.
     *
     * With a space separator, an empty field would otherwise collapse into a
     * run of spaces and silently vanish, shifting every field after it by one
     * position. The genuine adapter shows the convention directly:
     *
     *     WIFIINFO 1 0 "" 0 HR_aabbccddeeff 0 0 347
     *                  ^^ empty SSID, quoted
     *
     * This never came up with comma separators, where an empty field is just
     * two adjacent commas.
     */
    if (s == NULL || s[0] == '\0') {
        build_append(b, "\"\"", 2);
        return;
    }
    /*
     * A field with a space in it has to be quoted for the same reason: the
     * genuine adapter sends WIFIINFO 5 81 "My Network" ..., and SETBNAME with
     * an unquoted "My Batch" arrives as two arguments. Callers that already
     * quote (send_wifiinfo) are left alone. A stray quote inside an unquoted
     * field has no known escape on this wire, so the frame is refused rather
     * than sent in a shape the dryer might read as something else.
     */
    size_t n = strlen(s);
    bool quoted = (n >= 2 && s[0] == '"' && s[n - 1] == '"');
    if (!quoted) {
        if (memchr(s, '"', n) != NULL) {
            b->ok = false;
            return;
        }
        if (memchr(s, ' ', n) != NULL) {
            build_append(b, "\"", 1);
            build_append(b, s, n);
            build_append(b, "\"", 1);
            return;
        }
    }
    build_append(b, s, n);
}

void hr_build_int(hr_builder_t *b, long v)
{
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%ld", v);
    hr_build_str(b, tmp);
}

const char *hr_build_finish(hr_builder_t *b, size_t *out_len)
{
    if (b == NULL) {
        return NULL;
    }
    build_append(b, "\r", 1);
    if (!b->ok) {
        return NULL;
    }
    if (out_len != NULL) {
        *out_len = b->len;
    }
    return b->buf;
}

const char *hr_frame_field(const hr_frame_t *f, int idx)
{
    if (f == NULL || idx < 0 || idx >= f->nfields) {
        return NULL;
    }
    return f->field[idx];
}

size_t hr_frame_tostring(const hr_frame_t *f, char *out, size_t cap)
{
    if (f == NULL || out == NULL || cap == 0) {
        return 0;
    }

    size_t need = strlen(f->verb);
    for (int i = 0; i < f->nfields; i++) {
        need += 1 + strlen(f->field[i]); /* comma + field */
    }
    if (need + 1 > cap) {
        return 0;
    }

    size_t pos = strlen(f->verb);
    memcpy(out, f->verb, pos);
    for (int i = 0; i < f->nfields; i++) {
        out[pos++] = ',';
        size_t n = strlen(f->field[i]);
        memcpy(out + pos, f->field[i], n);
        pos += n;
    }
    out[pos] = '\0';
    return pos;
}

long hr_frame_field_int(const hr_frame_t *f, int idx, long def)
{
    const char *s = hr_frame_field(f, idx);
    if (s == NULL || *s == '\0') {
        return def;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        return def;
    }
    return v;
}
