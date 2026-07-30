#include "hr_protocol.h"
#include "test_util.h"

#include <stdlib.h>

/* Collector used as the stream callback. */
#define MAX_CAPTURED 16
typedef struct {
    char verb[MAX_CAPTURED][HR_MAX_VERB];
    char first[MAX_CAPTURED][64];
    int count;
} capture_t;

static void on_frame(const hr_frame_t *f, void *user)
{
    capture_t *c = (capture_t *)user;
    if (c->count >= MAX_CAPTURED) {
        return;
    }
    snprintf(c->verb[c->count], HR_MAX_VERB, "%s", f->verb);
    const char *f0 = hr_frame_field(f, 0);
    snprintf(c->first[c->count], 64, "%s", f0 ? f0 : "");
    c->count++;
}

static void test_reassembles_frame_split_across_chunks(void)
{
    TEST_CASE("reassembles frame split across chunks");
    hr_stream_t s;
    capture_t c = {0};
    hr_stream_init(&s);

    hr_stream_feed(&s, "STA", 3, on_frame, &c);
    CHECK_INT(c.count, 0); /* nothing complete yet */
    hr_stream_feed(&s, "T,42,ok\r", 8, on_frame, &c);

    CHECK_INT(c.count, 1);
    CHECK_STR(c.verb[0], "STAT");
    CHECK_STR(c.first[0], "42");
}

static void test_delivers_multiple_frames_in_one_chunk(void)
{
    TEST_CASE("delivers multiple frames in one chunk");
    hr_stream_t s;
    capture_t c = {0};
    hr_stream_init(&s);

    const char *burst = "REQINFO\rSTAT,1,a\rNTFY,9,0,x,\r";
    hr_stream_feed(&s, burst, strlen(burst), on_frame, &c);

    CHECK_INT(c.count, 3);
    CHECK_STR(c.verb[0], "REQINFO");
    CHECK_STR(c.verb[1], "STAT");
    CHECK_STR(c.verb[2], "NTFY");
    CHECK_INT(s.frames_ok, 3);
}

static void test_ignores_empty_frames_and_lf(void)
{
    /* Some hosts append LF; blank frames must not reach the callback. */
    TEST_CASE("ignores empty frames and LF");
    hr_stream_t s;
    capture_t c = {0};
    hr_stream_init(&s);

    const char *burst = "\r\r\nBEEP\r\n\r";
    hr_stream_feed(&s, burst, strlen(burst), on_frame, &c);

    CHECK_INT(c.count, 1);
    CHECK_STR(c.verb[0], "BEEP");
}

static void test_drops_oversized_frame_and_resyncs(void)
{
    TEST_CASE("drops oversized frame and resyncs");
    hr_stream_t s;
    capture_t c = {0};
    hr_stream_init(&s);

    char *huge = malloc(HR_MAX_FRAME * 2);
    memset(huge, 'X', HR_MAX_FRAME * 2);
    hr_stream_feed(&s, huge, HR_MAX_FRAME * 2, on_frame, &c);
    free(huge);
    CHECK_INT(c.count, 0);

    /* terminator closes the bad frame; the next one must parse cleanly */
    hr_stream_feed(&s, "\rSTAT,7,z\r", 10, on_frame, &c);
    CHECK_INT(c.count, 1);
    CHECK_STR(c.verb[0], "STAT");
    CHECK_STR(c.first[0], "7");
    CHECK_INT(s.frames_bad, 1);
}

int main(void)
{
    test_reassembles_frame_split_across_chunks();
    test_delivers_multiple_frames_in_one_chunk();
    test_ignores_empty_frames_and_lf();
    test_drops_oversized_frame_and_resyncs();
    return TEST_REPORT();
}
