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
}

void hr_stream_feed(hr_stream_t *s, const void *data, size_t n, hr_frame_cb cb,
                    void *user)
{
    if (s == NULL || data == NULL) {
        return;
    }

    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < n; i++) {
        char ch = (char)p[i];

        if (ch != '\r' && ch != '\n') {
            if (s->len < HR_MAX_FRAME - 1) {
                s->buf[s->len++] = ch;
            } else {
                /* Frame is longer than we can hold; discard it at the
                 * terminator rather than emitting a truncated command. */
                s->overflowed = true;
            }
            continue;
        }

        /* Terminator reached: close out whatever we accumulated. */
        if (s->overflowed) {
            s->frames_bad++;
        } else if (s->len > 0) {
            s->buf[s->len] = '\0';
            hr_frame_t frame;
            if (hr_frame_parse(s->buf, &frame)) {
                s->frames_ok++;
                if (cb != NULL) {
                    cb(&frame, user);
                }
            } else {
                s->frames_bad++;
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
 *     WIFIINFO 1 0 "" 0 HR_3cdc75f95aac 0 0 161
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
     *     WIFIINFO 1 0 "" 0 HR_3cdc75f95aac 0 0 347
     *                  ^^ empty SSID, quoted
     *
     * This never came up with comma separators, where an empty field is just
     * two adjacent commas.
     */
    if (s == NULL || s[0] == '\0') {
        build_append(b, "\"\"", 2);
        return;
    }
    build_append(b, s, strlen(s));
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
