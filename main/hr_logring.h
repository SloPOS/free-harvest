/*
 * Packed ring of text lines, for the in-app log.
 *
 * Pure C, no ESP-IDF dependency, so it is unit-tested on the host. The
 * caller supplies the storage and does its own locking.
 *
 * Each record is [len_hi][len_lo][text]; a line only costs what it is long,
 * so 24 KB holds roughly 350 lines of the ~65-character average instead of
 * 170 fixed 144-byte slots. When the ring is full the oldest lines go first,
 * whole. Lines longer than HR_LOGRING_LINE_MAX are truncated, never split.
 */
#ifndef HR_LOGRING_H
#define HR_LOGRING_H

#include <stdbool.h>
#include <stddef.h>

/* Longest single line kept, including the NUL the reader gets back. */
#define HR_LOGRING_LINE_MAX 144

typedef struct {
    char   *buf;
    size_t  cap;
    size_t  head;   /* next byte to write */
    size_t  tail;   /* first byte of the oldest record */
    size_t  used;   /* bytes occupied by records */
    size_t  count;  /* records held */
} hr_logring_t;

/* `buf` must outlive the ring; `cap` must be at least HR_LOGRING_LINE_MAX+2. */
void hr_logring_init(hr_logring_t *r, char *buf, size_t cap);

/* Append `n` bytes of text (no NUL needed), truncating to the line maximum
 * and dropping the oldest lines until it fits. */
void hr_logring_push(hr_logring_t *r, const char *text, size_t n);

size_t hr_logring_count(const hr_logring_t *r);

/* Copy line `i` (0 = oldest) into `out`, NUL-terminated. False if `i` is out
 * of range. O(i) - fine for a few hundred lines served once in a while. */
bool hr_logring_get(const hr_logring_t *r, size_t i, char *out, size_t cap);

/* Iteration without the O(i) walk: `pos` starts at r->tail and is advanced
 * by each call. Returns false when there are no more records. */
bool hr_logring_next(const hr_logring_t *r, size_t *pos, size_t *remaining,
                     char *out, size_t cap);

#endif /* HR_LOGRING_H */
