#include "hr_logring.h"

#include <string.h>

#define HDR 2u

void hr_logring_init(hr_logring_t *r, char *buf, size_t cap)
{
    r->buf = buf;
    r->cap = cap;
    r->head = r->tail = r->used = r->count = 0;
}

static void put(hr_logring_t *r, const char *src, size_t n)
{
    size_t first = r->cap - r->head;
    if (first > n) {
        first = n;
    }
    memcpy(r->buf + r->head, src, first);
    if (n > first) {
        memcpy(r->buf, src + first, n - first);
    }
    r->head = (r->head + n) % r->cap;
}

static void take(const hr_logring_t *r, size_t at, char *dst, size_t n)
{
    size_t first = r->cap - at;
    if (first > n) {
        first = n;
    }
    memcpy(dst, r->buf + at, first);
    if (n > first) {
        memcpy(dst + first, r->buf, n - first);
    }
}

static size_t rec_len(const hr_logring_t *r, size_t at)
{
    unsigned char h[HDR];
    take(r, at, (char *)h, HDR);
    return ((size_t)h[0] << 8) | h[1];
}

static void drop_oldest(hr_logring_t *r)
{
    size_t n = rec_len(r, r->tail);
    r->tail = (r->tail + HDR + n) % r->cap;
    r->used -= HDR + n;
    r->count--;
}

void hr_logring_push(hr_logring_t *r, const char *text, size_t n)
{
    if (r->buf == NULL || r->cap < HR_LOGRING_LINE_MAX + HDR) {
        return;
    }
    if (n > HR_LOGRING_LINE_MAX - 1) {
        n = HR_LOGRING_LINE_MAX - 1;
    }
    while (r->count > 0 && r->used + HDR + n > r->cap) {
        drop_oldest(r);
    }
    unsigned char h[HDR] = {(unsigned char)(n >> 8), (unsigned char)(n & 0xff)};
    put(r, (const char *)h, HDR);
    put(r, text, n);
    r->used += HDR + n;
    r->count++;
}

size_t hr_logring_count(const hr_logring_t *r)
{
    return r->count;
}

bool hr_logring_next(const hr_logring_t *r, size_t *pos, size_t *remaining,
                     char *out, size_t cap)
{
    if (*remaining == 0 || out == NULL || cap == 0) {
        return false;
    }
    size_t n = rec_len(r, *pos);
    size_t copy = n < cap - 1 ? n : cap - 1;
    take(r, (*pos + HDR) % r->cap, out, copy);
    out[copy] = '\0';
    *pos = (*pos + HDR + n) % r->cap;
    (*remaining)--;
    return true;
}

bool hr_logring_get(const hr_logring_t *r, size_t i, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (i >= r->count) {
        return false;
    }
    size_t pos = r->tail;
    for (size_t k = 0; k < i; k++) {
        pos = (pos + HDR + rec_len(r, pos)) % r->cap;
    }
    size_t remaining = 1;
    return hr_logring_next(r, &pos, &remaining, out, cap);
}
