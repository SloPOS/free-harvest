#include "hr_encring.h"

#include <string.h>

void hr_encring_init(hr_encring_t *r)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
}

void hr_encring_push(hr_encring_t *r, uint32_t t_ms, const char *frame,
                     size_t len)
{
    if (r == NULL || frame == NULL || len == 0) {
        return;
    }
    hr_enc_rec_t *rec = &r->rec[r->head];
    size_t keep = len < HR_ENCRING_RAW ? len : HR_ENCRING_RAW;
    rec->t_ms = t_ms;
    rec->len = (uint16_t)(len > 0xffff ? 0xffff : len);
    rec->kept = (uint16_t)keep;
    memcpy(rec->raw, frame, keep);
    rec->raw[keep] = '\0';

    r->head = (r->head + 1u) % HR_ENCRING_N;
    if (r->count < HR_ENCRING_N) {
        r->count++;
    }
    r->total_frames++;
    r->total_bytes += len;
}

unsigned hr_encring_count(const hr_encring_t *r)
{
    return r == NULL ? 0 : r->count;
}

bool hr_encring_get(const hr_encring_t *r, unsigned i, hr_enc_rec_t *out)
{
    if (r == NULL || out == NULL || i >= r->count) {
        return false;
    }
    /* Oldest is `count` slots behind head, wrapping. */
    unsigned oldest = (r->head + HR_ENCRING_N - r->count) % HR_ENCRING_N;
    *out = r->rec[(oldest + i) % HR_ENCRING_N];
    return true;
}
