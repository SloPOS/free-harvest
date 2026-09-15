#include "hr_logring.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void push_str(hr_logring_t *r, const char *s)
{
    hr_logring_push(r, s, strlen(s));
}

static void test_fifo_order(void)
{
    TEST_CASE("lines come back oldest first");
    char store[512];
    hr_logring_t r;
    hr_logring_init(&r, store, sizeof(store));
    CHECK_INT(hr_logring_count(&r), 0);

    push_str(&r, "one");
    push_str(&r, "two");
    push_str(&r, "three");
    CHECK_INT(hr_logring_count(&r), 3);

    char out[32];
    CHECK(hr_logring_get(&r, 0, out, sizeof(out)));
    CHECK_STR(out, "one");
    CHECK(hr_logring_get(&r, 1, out, sizeof(out)));
    CHECK_STR(out, "two");
    CHECK(hr_logring_get(&r, 2, out, sizeof(out)));
    CHECK_STR(out, "three");
    CHECK(!hr_logring_get(&r, 3, out, sizeof(out)));
    CHECK_STR(out, "");
}

static void test_drops_oldest_whole(void)
{
    TEST_CASE("a full ring drops whole oldest lines");
    /* One maximal record plus exactly three 8-byte records (6 chars + 2
     * header): pushing the maximal line after the three must evict at
     * least one of them, and evict it whole. */
    char big[HR_LOGRING_LINE_MAX + 2 + 3 * 8];
    hr_logring_t r;
    hr_logring_init(&r, big, sizeof(big));
    char line[HR_LOGRING_LINE_MAX];
    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    push_str(&r, "aaaaaa");
    push_str(&r, "bbbbbb");
    push_str(&r, "cccccc");
    CHECK_INT(hr_logring_count(&r), 3);
    /* A maximal line (143 chars + 2) leaves 24 bytes: only the three small
     * records fit alongside it, but head must wrap, so at least one goes. */
    push_str(&r, line);
    CHECK(hr_logring_count(&r) >= 1);
    char out[HR_LOGRING_LINE_MAX];
    size_t last = hr_logring_count(&r) - 1;
    CHECK(hr_logring_get(&r, last, out, sizeof(out)));
    CHECK_INT(strlen(out), HR_LOGRING_LINE_MAX - 1);
    /* Everything still readable and in order. */
    for (size_t i = 0; i < last; i++) {
        CHECK(hr_logring_get(&r, i, out, sizeof(out)));
        CHECK_INT(strlen(out), 6);
    }
}

static void test_wraps_and_survives_many_pushes(void)
{
    TEST_CASE("wrap-around keeps count and order over thousands of lines");
    char store[1024];
    hr_logring_t r;
    hr_logring_init(&r, store, sizeof(store));
    char msg[64], out[64];
    for (int i = 0; i < 5000; i++) {
        int n = snprintf(msg, sizeof(msg), "line %d %.*s", i, i % 23,
                         "......................");
        hr_logring_push(&r, msg, (size_t)n);
        CHECK(r.used <= r.cap);
    }
    size_t cnt = hr_logring_count(&r);
    CHECK(cnt > 10);
    /* The newest line is the last one pushed; the oldest is contiguous. */
    CHECK(hr_logring_get(&r, cnt - 1, out, sizeof(out)));
    CHECK(strncmp(out, "line 4999", 9) == 0);
    int prev = -1;
    for (size_t i = 0; i < cnt; i++) {
        CHECK(hr_logring_get(&r, i, out, sizeof(out)));
        int v = atoi(out + 5);
        if (prev >= 0) {
            CHECK_INT(v, prev + 1);
        }
        prev = v;
    }
    /* Iterator agrees with the indexer. */
    size_t pos = r.tail, left = r.count, seen = 0;
    while (hr_logring_next(&r, &pos, &left, out, sizeof(out))) {
        seen++;
    }
    CHECK_INT(seen, cnt);
}

static void test_truncates_long_lines(void)
{
    TEST_CASE("over-long lines are truncated, not split or dropped");
    char store[2048];
    hr_logring_t r;
    hr_logring_init(&r, store, sizeof(store));
    char line[400];
    memset(line, 'y', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    push_str(&r, line);
    push_str(&r, "after");
    char out[512];
    CHECK(hr_logring_get(&r, 0, out, sizeof(out)));
    CHECK_INT(strlen(out), HR_LOGRING_LINE_MAX - 1);
    CHECK(hr_logring_get(&r, 1, out, sizeof(out)));
    CHECK_STR(out, "after");
    /* A small reader buffer gets a NUL-terminated prefix. */
    char small[8];
    CHECK(hr_logring_get(&r, 0, small, sizeof(small)));
    CHECK_INT(strlen(small), 7);
}

static void test_refuses_tiny_store(void)
{
    TEST_CASE("a store too small for one line is left alone");
    char store[64];
    hr_logring_t r;
    hr_logring_init(&r, store, sizeof(store));
    push_str(&r, "x");
    CHECK_INT(hr_logring_count(&r), 0);
}

int main(void)
{
    test_fifo_order();
    test_drops_oldest_whole();
    test_wraps_and_survives_many_pushes();
    test_truncates_long_lines();
    test_refuses_tiny_store();
    return TEST_REPORT();
}
