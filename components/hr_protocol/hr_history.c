#include "hr_history.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void hr_history_init(hr_history_t *h)
{
    if (h == NULL) {
        return;
    }
    memset(h, 0, sizeof(*h));
}

static hr_hist_verb_t *find_or_add_verb(hr_history_t *h, const char *verb)
{
    for (int i = 0; i < h->nverbs; i++) {
        if (strcmp(h->verbs[i].verb, verb) == 0) {
            return &h->verbs[i];
        }
    }
    if (h->nverbs >= HR_HIST_VERBS) {
        return NULL; /* table full; verb won't get per-verb tracking */
    }
    hr_hist_verb_t *v = &h->verbs[h->nverbs++];
    memset(v, 0, sizeof(*v));
    snprintf(v->verb, sizeof(v->verb), "%s", verb);
    return v;
}

/* Compute a changed-field mask between two frame bodies of the same verb. */
static uint32_t diff_mask(const hr_frame_t *cur, const char *prev_body)
{
    /* Re-parse the stored previous body so we can compare field by field. */
    hr_frame_t prev;
    char tmp[HR_MAX_FRAME];
    snprintf(tmp, sizeof(tmp), "%s", prev_body);
    if (!hr_frame_parse(tmp, &prev)) {
        return 0;
    }

    uint32_t mask = 0;
    int n = cur->nfields > prev.nfields ? cur->nfields : prev.nfields;
    if (n > 32) {
        n = 32;
    }
    for (int i = 0; i < n; i++) {
        const char *a = hr_frame_field(cur, i);
        const char *b = hr_frame_field(&prev, i);
        /* A field present on one side but not the other counts as changed. */
        if ((a == NULL) != (b == NULL)) {
            mask |= (1u << i);
        } else if (a != NULL && b != NULL && strcmp(a, b) != 0) {
            mask |= (1u << i);
        }
    }
    return mask;
}

uint32_t hr_history_add(hr_history_t *h, const hr_frame_t *f, uint32_t t_ms)
{
    if (h == NULL || f == NULL) {
        return 0;
    }

    uint32_t seq = ++h->seq;
    h->total++;

    hr_hist_entry_t *e = &h->ring[(seq - 1) % HR_HIST_CAP];
    e->seq = seq;
    e->t_ms = t_ms;
    snprintf(e->verb, sizeof(e->verb), "%s", f->verb);
    hr_frame_tostring(f, e->body, sizeof(e->body));
    e->nfields = (uint8_t)(f->nfields > 255 ? 255 : f->nfields);

    hr_hist_verb_t *v = find_or_add_verb(h, f->verb);
    if (v != NULL) {
        if (v->count > 0) {
            v->changed_mask = diff_mask(f, v->last_body);
        } else {
            v->changed_mask = 0;
        }
        v->count++;
        v->last_seq = seq;
        v->nfields = e->nfields;
        hr_frame_tostring(f, v->last_body, sizeof(v->last_body));
    }

    return seq;
}

uint32_t hr_history_latest_seq(const hr_history_t *h)
{
    return h ? h->seq : 0;
}

int hr_history_since(const hr_history_t *h, uint32_t after,
                     hr_hist_entry_t *out, int max)
{
    if (h == NULL || out == NULL || max <= 0) {
        return 0;
    }

    /* Oldest sequence still retained in the ring. */
    uint32_t oldest = 1;
    if (h->seq > HR_HIST_CAP) {
        oldest = h->seq - HR_HIST_CAP + 1;
    }

    uint32_t start = after + 1;
    if (start < oldest) {
        start = oldest; /* requested entries have already been overwritten */
    }

    int n = 0;
    for (uint32_t s = start; s <= h->seq && n < max; s++) {
        const hr_hist_entry_t *e = &h->ring[(s - 1) % HR_HIST_CAP];
        if (e->seq != s) {
            continue; /* slot reused; skip defensively */
        }
        out[n++] = *e;
    }
    return n;
}

const hr_hist_verb_t *hr_history_verb(const hr_history_t *h, const char *verb)
{
    if (h == NULL || verb == NULL) {
        return NULL;
    }
    for (int i = 0; i < h->nverbs; i++) {
        if (strcmp(h->verbs[i].verb, verb) == 0) {
            return &h->verbs[i];
        }
    }
    return NULL;
}

size_t hr_json_escape(const char *in, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    size_t o = 0;
    for (size_t i = 0; in != NULL && in[i] != '\0'; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) {
                break;
            }
            out[o++] = '\\';
            out[o++] = c;
        } else if ((unsigned char)c < 0x20) {
            continue; /* drop control chars (CR/LF/TAB etc.) */
        } else {
            if (o + 1 >= cap) {
                break;
            }
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return o;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void hr_url_decode(char *s)
{
    if (s == NULL) {
        return;
    }
    char *r = s; /* read */
    char *w = s; /* write */
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%') {
            int hi = hex_val(r[1]); /* safe: r[1] is at worst the NUL */
            int lo = (hi >= 0) ? hex_val(r[2]) : -1;
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
            } else {
                *w++ = *r++; /* malformed - copy the '%' literally */
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

size_t hr_hist_entry_json(const hr_hist_entry_t *e, char *out, size_t cap)
{
    if (e == NULL || out == NULL) {
        return 0;
    }
    char body[HR_HIST_BODY * 2];
    char verb[HR_MAX_VERB * 2];
    hr_json_escape(e->body, body, sizeof(body));
    hr_json_escape(e->verb, verb, sizeof(verb));

    int n = snprintf(out, cap,
                     "{\"seq\":%" PRIu32 ",\"t\":%" PRIu32
                     ",\"verb\":\"%s\",\"body\":\"%s\",\"n\":%u}",
                     e->seq, e->t_ms, verb, body, (unsigned)e->nfields);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    return (size_t)n;
}
