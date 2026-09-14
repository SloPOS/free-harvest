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

typedef struct {
    int count;
    char last[80];
    char why[32];
    size_t n;
} rejects_t;

static void on_reject(const char *bytes, size_t n, const char *why, void *user)
{
    rejects_t *r = (rejects_t *)user;
    r->count++;
    r->n = n;
    size_t keep = n < sizeof(r->last) - 1 ? n : sizeof(r->last) - 1;
    memcpy(r->last, bytes, keep);
    r->last[keep] = '\0';
    snprintf(r->why, sizeof(r->why), "%s", why);
}

static void test_rejected_lines_reach_the_observer(void)
{
    /*
     * Lines the parser refuses used to vanish into frames_bad. A reply
     * without a comma that is 24+ characters long ("Thanks for Beeping!" is
     * 19; longer ones are plausible), a frame with more than HR_MAX_FIELDS
     * fields, or an oversized line are precisely the parts of the protocol
     * nobody has decoded yet, so they must be observable.
     */
    TEST_CASE("rejected lines reach the observer");
    hr_stream_t s;
    capture_t c = {0};
    rejects_t r = {0};
    hr_stream_init(&s);
    hr_stream_set_reject_cb(&s, on_reject, &r);

    /* verb longer than HR_MAX_VERB, no comma */
    const char *longverb = "ThisReplyHasNoCommaAndIsFarTooLongToBeAVerb\r";
    hr_stream_feed(&s, longverb, strlen(longverb), on_frame, &c);
    CHECK_INT(c.count, 0);
    CHECK_INT(r.count, 1);
    CHECK_STR(r.why, "unparsable");
    CHECK_STR(r.last, "ThisReplyHasNoCommaAndIsFarTooLongToBeAVerb");
    CHECK_INT(r.n, strlen(longverb) - 1);

    /* oversized: the observer gets the head that fit */
    char *huge = malloc(HR_MAX_FRAME * 2);
    memset(huge, 'Y', HR_MAX_FRAME * 2);
    hr_stream_feed(&s, huge, HR_MAX_FRAME * 2, on_frame, &c);
    free(huge);
    hr_stream_feed(&s, "\r", 1, on_frame, &c);
    CHECK_INT(r.count, 2);
    CHECK_STR(r.why, "too long");
    CHECK_INT(r.n, HR_MAX_FRAME - 1);
    CHECK_INT(s.frames_bad, 2);

    /* a good frame still parses and does not touch the observer */
    hr_stream_feed(&s, "STAT,1\r", 7, on_frame, &c);
    CHECK_INT(c.count, 1);
    CHECK_INT(r.count, 2);

    /* clearing the observer stops the calls but keeps counting */
    hr_stream_set_reject_cb(&s, NULL, NULL);
    hr_stream_feed(&s, longverb, strlen(longverb), on_frame, &c);
    CHECK_INT(r.count, 2);
    CHECK_INT(s.frames_bad, 3);
}

int main(void)
{
    test_rejected_lines_reach_the_observer();
    test_reassembles_frame_split_across_chunks();
    test_delivers_multiple_frames_in_one_chunk();
    test_ignores_empty_frames_and_lf();
    test_drops_oversized_frame_and_resyncs();
    return TEST_REPORT();
}
