/*
 * The 6.0.644170 encoded transport: ")S" + two length digits + payload, no
 * terminator. Framing only - nothing here knows or asks what the payload is.
 *
 * Fixtures are frames captured from a real machine on 2026-09-15
 * (bench/2026-09-15-dryer-2/50-frame-analysis.txt in the workspace repo),
 * verbatim. Every declared length there matched the bytes that followed.
 */
#include "hr_encring.h"
#include "hr_protocol.h"
#include "test_util.h"

#include <stdlib.h>

/* --- fixtures ----------------------------------------------------------- */

/* 20 chars: the frame that replaced the plaintext REQINFO slot (every 10 s). */
static const char FX_20[] = ")S#7':CP`QB*MBJKUZab";
/* 36 chars: after FDNAME. Note the "!!" tail. */
static const char FX_36[] = ")S#GSIY84F/ObJG2E6:MW\\QO^]H'bJB5@M!!";
/* 72 chars: after REQCFG. */
static const char FX_72[] =
    ")S$+4KM@%(KO/4)8^EAb@8R$0MSG5J^PK=bW]#-4)-M<=5KOM7AA*T-3@[X-`@'?/6A5EQRS";
/* 80 chars: the periodic (15.02 s) frame, and the answer to STATUS. */
static const char FX_80[] =
    ")S$32H&MA:<Z9(R+\\@T*COR(GSO3\\]YNUIPCA@>9,,3/>D>7>QI,GTLSYYZ-3>-K"
    "0123456789ABCDEF";
/* 96 chars: the answer to "UNIQUE lH" - the first encoded frame ever seen. */
static const char FX_96[] =
    ")S$C+]QWO\\WD%)EH(B-\\%DXF;F%+/CGLM8.DT0\\RF/)?)5CQTM2<?a/MJIIE:D<F"
    "0123456789ABCDEF0123456789ABCDEF";

/*
 * The old recorder cut every burst at 64 characters, so of the 72-char frame
 * only its first 28 are on record (it followed the 36-char one in a 188-byte
 * burst) and of the 80- and 96-char frames the first 64. The tails above are
 * filled in with a printable run to the declared length. The 20- and 36-char
 * frames are complete captures. The framer cares about the length, and the
 * lengths are what the capture proved - 100+ frames, no exceptions.
 */

/* --- collectors --------------------------------------------------------- */

#define MAX_ENC 16
typedef struct {
    int count;
    size_t len[MAX_ENC];
    char raw[MAX_ENC][HR_MAX_FRAME];
} enc_log_t;

static void on_enc(const char *frame, size_t len, void *user)
{
    enc_log_t *e = (enc_log_t *)user;
    if (e->count >= MAX_ENC) {
        return;
    }
    e->len[e->count] = len;
    memcpy(e->raw[e->count], frame, len);
    e->raw[e->count][len] = '\0';
    e->count++;
}

typedef struct {
    int count;
    char verb[8][HR_MAX_VERB];
} frames_t;

static void on_frame(const hr_frame_t *f, void *user)
{
    frames_t *c = (frames_t *)user;
    if (c->count < 8) {
        snprintf(c->verb[c->count], HR_MAX_VERB, "%s", f->verb);
    }
    c->count++;
}

typedef struct {
    int count;
    char why[32];
    char last[HR_MAX_FRAME];
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

static void stream_with(hr_stream_t *s, enc_log_t *e, rejects_t *r)
{
    hr_stream_init(s);
    hr_stream_set_enc_cb(s, on_enc, e);
    hr_stream_set_reject_cb(s, on_reject, r);
}

/* --- tests -------------------------------------------------------------- */

static void test_length_table_from_the_capture(void)
{
    /*
     * Two base-64 digits, alphabet '#'..'b', MSB first, total length incl.
     * the 4-char header. Every pair seen on the dryer, and both ends of the
     * alphabet.
     */
    TEST_CASE("length header decodes the five captured pairs");
    CHECK_INT(hr_enc_decode_len(")S#7"), 20);
    CHECK_INT(hr_enc_decode_len(")S#G"), 36);
    CHECK_INT(hr_enc_decode_len(")S$+"), 72);
    CHECK_INT(hr_enc_decode_len(")S$3"), 80);
    CHECK_INT(hr_enc_decode_len(")S$C"), 96);
    CHECK_INT(hr_enc_decode_len(")S##"), -1);   /* 0: shorter than its header */
    CHECK_INT(hr_enc_decode_len(")S#'"), 4);    /* header-only frame */
    CHECK_INT(hr_enc_decode_len(")Sbb"), 63 * 64 + 63);

    /* not a header: wrong magic, digit outside the alphabet */
    CHECK_INT(hr_enc_decode_len("STAT"), -1);
    CHECK_INT(hr_enc_decode_len(")S!7"), -1);   /* '!' is below '#' */
    CHECK_INT(hr_enc_decode_len(")S\"7"), -1);
    CHECK_INT(hr_enc_decode_len(")Sc7"), -1);   /* 'c' is above 'b' */
    CHECK_INT(hr_enc_decode_len(")S#{"), -1);
    CHECK_INT(hr_enc_decode_len(NULL), -1);

    /* the fixtures really are their declared lengths */
    CHECK_INT(strlen(FX_20), 20);
    CHECK_INT(strlen(FX_36), 36);
    CHECK_INT(strlen(FX_72), 72);
    CHECK_INT(strlen(FX_80), 80);
    CHECK_INT(strlen(FX_96), 96);
}

static void test_each_captured_frame_is_delivered_whole(void)
{
    TEST_CASE("each captured frame is delivered whole and verbatim");
    hr_stream_t s;
    enc_log_t e = {0};
    rejects_t r = {0};
    frames_t c = {0};
    stream_with(&s, &e, &r);

    const char *fx[] = {FX_96, FX_36, FX_72, FX_80, FX_20};
    for (size_t i = 0; i < 5; i++) {
        hr_stream_feed(&s, fx[i], strlen(fx[i]), on_frame, &c);
    }
    CHECK_INT(e.count, 5);
    CHECK_INT(e.len[0], 96);
    CHECK_STR(e.raw[0], FX_96);
    CHECK_INT(e.len[1], 36);
    CHECK_STR(e.raw[1], FX_36);
    CHECK_INT(e.len[2], 72);
    CHECK_STR(e.raw[2], FX_72);
    CHECK_INT(e.len[3], 80);
    CHECK_STR(e.raw[3], FX_80);
    CHECK_INT(e.len[4], 20);
    CHECK_STR(e.raw[4], FX_20);

    /* counted as encoded, never as bad or noise, never as plaintext */
    CHECK_INT(s.enc_frames, 5);
    CHECK_INT(s.enc_bytes, 96 + 36 + 72 + 80 + 20);
    CHECK_INT(s.enc_bad, 0);
    CHECK_INT(s.frames_bad, 0);
    CHECK_INT(s.frames_ok, 0);
    CHECK_INT(s.noise_bytes, 0);
    CHECK_INT(r.count, 0);
    CHECK_INT(c.count, 0);
    CHECK_INT((int)s.len, 0);
    CHECK(s.enc_seen);
}

static void test_burst_of_glued_frames_is_split_by_length(void)
{
    /*
     * The dryer answers a whole handshake in one USB burst: 96 + 36 + 72 +
     * 80 + 20 = 304 bytes with no separator - which is exactly what the
     * capture recorded as "stale 304". Only the lengths tell them apart.
     */
    TEST_CASE("a 304-byte burst splits into its five frames");
    hr_stream_t s;
    enc_log_t e = {0};
    rejects_t r = {0};
    stream_with(&s, &e, &r);

    char burst[400];
    snprintf(burst, sizeof(burst), "%s%s%s%s%s", FX_96, FX_36, FX_72, FX_80,
             FX_20);
    CHECK_INT(strlen(burst), 304);
    hr_stream_feed(&s, burst, strlen(burst), NULL, NULL);

    CHECK_INT(e.count, 5);
    CHECK_INT(e.len[0], 96);
    CHECK_INT(e.len[1], 36);
    CHECK_INT(e.len[2], 72);
    CHECK_INT(e.len[3], 80);
    CHECK_INT(e.len[4], 20);
    CHECK_STR(e.raw[4], FX_20);
    CHECK_INT(s.enc_bad, 0);
    CHECK_INT(r.count, 0);

    /* the 20+80 "100-byte" pairs seen every 30 s */
    snprintf(burst, sizeof(burst), "%s%s", FX_20, FX_80);
    hr_stream_feed(&s, burst, strlen(burst), NULL, NULL);
    CHECK_INT(e.count, 7);
    CHECK_INT(e.len[5], 20);
    CHECK_INT(e.len[6], 80);
}

static void test_frame_split_across_chunks_including_the_header(void)
{
    /*
     * USB CDC reads do not respect frame boundaries: the header itself can
     * arrive one byte at a time. Nothing is decided until four bytes are in.
     */
    TEST_CASE("a frame arrives byte by byte, header included");
    hr_stream_t s;
    enc_log_t e = {0};
    rejects_t r = {0};
    stream_with(&s, &e, &r);

    for (size_t i = 0; i < strlen(FX_80); i++) {
        hr_stream_feed(&s, FX_80 + i, 1, NULL, NULL);
        if (i < 79) {
            CHECK_INT(e.count, 0);
        }
    }
    CHECK_INT(e.count, 1);
    CHECK_INT(e.len[0], 80);
    CHECK_STR(e.raw[0], FX_80);

    /* and in awkward chunk sizes: 3 (header incomplete), then 60, then rest */
    hr_stream_feed(&s, FX_96, 3, NULL, NULL);
    CHECK_INT((int)s.enc_need, 0);          /* still undecided */
    hr_stream_feed(&s, FX_96 + 3, 60, NULL, NULL);
    CHECK_INT((int)s.enc_need, 96);
    CHECK_INT(e.count, 1);
    hr_stream_feed(&s, FX_96 + 63, 33, NULL, NULL);
    CHECK_INT(e.count, 2);
    CHECK_STR(e.raw[1], FX_96);
    CHECK_INT(r.count, 0);
}

static void test_partial_frame_is_discarded_and_the_next_header_resyncs(void)
{
    /*
     * The declared length never arrives (dryer reset, USB hiccup). The
     * session's stale timer calls hr_stream_discard_partial(); the head goes
     * to the observer as "enc partial", is counted in enc_bad - NOT in
     * frames_bad - and the next ")S" starts clean.
     */
    TEST_CASE("partial encoded frame: discarded on timeout, next header clean");
    hr_stream_t s;
    enc_log_t e = {0};
    rejects_t r = {0};
    stream_with(&s, &e, &r);

    hr_stream_feed(&s, FX_80, 50, NULL, NULL);      /* 30 short */
    CHECK_INT(e.count, 0);
    CHECK_INT((int)s.enc_need, 80);
    CHECK((int)s.len == 50);

    CHECK(hr_stream_discard_partial(&s, "stale"));
    CHECK_INT(r.count, 1);
    CHECK_STR(r.why, "enc partial");
    CHECK_INT(r.n, 50);
    CHECK(strncmp(r.last, FX_80, 50) == 0);
    CHECK_INT(s.enc_bad, 1);
    CHECK_INT(s.frames_bad, 0);
    CHECK_INT((int)s.enc_need, 0);
    CHECK_INT((int)s.len, 0);
    CHECK(!hr_stream_discard_partial(&s, "stale"));    /* nothing left */

    hr_stream_feed(&s, FX_20, 20, NULL, NULL);
    CHECK_INT(e.count, 1);
    CHECK_STR(e.raw[0], FX_20);
    CHECK_INT(s.enc_frames, 1);
}

static void test_debris_before_a_header_is_dropped_once_encoded_mode_is_seen(void)
{
    /*
     * A frame whose declared length was SHORTER than what the dryer sent
     * leaves a tail with no header; the next real ")S" must not be glued to
     * it and lost. Once encoded mode has been seen, a ")S" arriving with
     * bytes pending flushes them as "enc resync" and starts the header
     * clean - without waiting for the stale timer.
     */
    TEST_CASE("debris before a header is flushed as enc resync");
    hr_stream_t s;
    enc_log_t e = {0};
    rejects_t r = {0};
    stream_with(&s, &e, &r);

    hr_stream_feed(&s, FX_20, 20, NULL, NULL);          /* enc_seen */
    CHECK_INT(e.count, 1);

    char glued[200];
    snprintf(glued, sizeof(glued), "XYZW%s", FX_36);
    hr_stream_feed(&s, glued, strlen(glued), NULL, NULL);
    CHECK_INT(e.count, 2);
    CHECK_STR(e.raw[1], FX_36);
    CHECK_INT(r.count, 1);
    CHECK_STR(r.why, "enc resync");
    CHECK_STR(r.last, "XYZW");
    CHECK_INT(s.enc_bad, 1);
    CHECK_INT(s.frames_bad, 0);

    /* a frame cut by a non-printable byte is abandoned, observably, and the
     * next frame is intact */
    hr_stream_feed(&s, FX_80, 30, NULL, NULL);
    hr_stream_feed(&s, "\x00", 1, NULL, NULL);
    CHECK_INT(r.count, 2);
    CHECK_STR(r.why, "enc interrupted");
    CHECK_INT(r.n, 30);
    CHECK_INT(s.enc_bad, 2);
    CHECK_INT(s.noise_bytes, 1);
    hr_stream_feed(&s, FX_72, 72, NULL, NULL);
    CHECK_INT(e.count, 3);
    CHECK_STR(e.raw[2], FX_72);

    /* a CR inside an encoded frame is not a terminator - it is a cut */
    hr_stream_feed(&s, FX_80, 10, NULL, NULL);
    hr_stream_feed(&s, "\r", 1, NULL, NULL);
    CHECK_INT(r.count, 3);
    CHECK_STR(r.why, "enc interrupted");
    CHECK_INT(s.noise_bytes, 1);        /* CR is not noise, just a cut */
    CHECK_INT(s.enc_bad, 3);
    CHECK_INT(s.frames_bad, 0);
}

static void test_oversized_declared_length_is_refused_and_bounded(void)
{
    /*
     * The header can declare up to 4095 bytes; the buffer holds 511. A
     * header beyond that is abandoned at once (4 bytes, "enc too long") and
     * nothing is accumulated for it, so a hostile or corrupt header cannot
     * grow anything.
     */
    TEST_CASE("declared length beyond the buffer is refused at the header");
    hr_stream_t s;
    enc_log_t e = {0};
    rejects_t r = {0};
    stream_with(&s, &e, &r);

    hr_stream_feed(&s, ")Sbb", 4, NULL, NULL);          /* 4095 */
    CHECK_INT(r.count, 1);
    CHECK_STR(r.why, "enc too long");
    CHECK_INT(r.n, 4);
    CHECK_INT(s.enc_bad, 1);
    CHECK_INT((int)s.enc_need, 0);
    CHECK_INT((int)s.len, 0);

    /* the largest that fits is accepted and filled to exactly that */
    char hdr[5];
    int max = HR_ENC_MAX_FRAME;                          /* 511 */
    hdr[0] = ')'; hdr[1] = 'S';
    hdr[2] = (char)('#' + max / 64);
    hdr[3] = (char)('#' + max % 64);
    hdr[4] = '\0';
    CHECK_INT(hr_enc_decode_len(hdr), max);
    char *big = malloc((size_t)max + 1);
    memset(big, 'A', (size_t)max);
    memcpy(big, hdr, 4);
    big[max] = '\0';
    hr_stream_feed(&s, big, (size_t)max, NULL, NULL);
    CHECK_INT(e.count, 1);
    CHECK_INT(e.len[0], max);
    CHECK_STR(e.raw[0], big);
    free(big);

    /* one over is refused */
    hdr[2] = (char)('#' + (max + 1) / 64);
    hdr[3] = (char)('#' + (max + 1) % 64);
    hr_stream_feed(&s, hdr, 4, NULL, NULL);
    CHECK_INT(r.count, 2);
    CHECK_STR(r.why, "enc too long");
}

static void test_plaintext_frames_still_parse_unchanged(void)
{
    /*
     * The 6.0.641041 protocol - CR-terminated plaintext - must be exactly as
     * before: same frames, same counters, same rejects, whether or not an
     * encoded frame has ever been seen. A ")S" INSIDE a plaintext frame is
     * plaintext while the stream has never seen the encoded transport.
     */
    TEST_CASE("plaintext frames parse exactly as before");
    hr_stream_t s;
    enc_log_t e = {0};
    rejects_t r = {0};
    frames_t c = {0};
    stream_with(&s, &e, &r);

    const char *plain = "REQINFO,\rUID,0-33393338,5,6.0.641041,0,\rSTAT,1,0,0\r"
                        "SNM,My )S Dryer,\r";
    hr_stream_feed(&s, plain, strlen(plain), on_frame, &c);
    CHECK_INT(c.count, 4);
    CHECK_STR(c.verb[0], "REQINFO");
    CHECK_STR(c.verb[1], "UID");
    CHECK_STR(c.verb[2], "STAT");
    CHECK_STR(c.verb[3], "SNM");
    CHECK_INT(s.frames_ok, 4);
    CHECK_INT(e.count, 0);
    CHECK_INT(s.enc_frames, 0);
    CHECK_INT(s.enc_bad, 0);
    CHECK_INT(r.count, 0);
    CHECK(!s.enc_seen);

    /* a plaintext frame that merely STARTS with ")S" but has no valid digits
     * is still plaintext (and unparsable as such, as it always was) */
    hr_stream_feed(&s, ")S!!,x\r", 7, on_frame, &c);
    CHECK_INT(c.count, 5);                  /* verb ")S!!" - odd, but a verb */
    CHECK_INT(e.count, 0);
    CHECK(!s.enc_seen);

    /* the two transports interleave: plaintext keeps working after encoded
     * frames, and the encoded ones never reach the plaintext callback */
    hr_stream_feed(&s, FX_20, 20, on_frame, &c);
    hr_stream_feed(&s, "REQINFO,\r", 9, on_frame, &c);
    hr_stream_feed(&s, FX_80, 80, on_frame, &c);
    CHECK_INT(c.count, 6);
    CHECK_STR(c.verb[5], "REQINFO");
    CHECK_INT(e.count, 2);
    CHECK_INT(s.frames_ok, 6);
    CHECK_INT(s.frames_bad, 0);
}

static void test_ring_keeps_the_last_n_oldest_first(void)
{
    TEST_CASE("enc ring: last N frames, oldest first, truncation flagged");
    hr_encring_t *r = malloc(sizeof(*r));
    hr_encring_init(r);
    CHECK_INT(hr_encring_count(r), 0);
    hr_enc_rec_t rec;
    CHECK(!hr_encring_get(r, 0, &rec));

    for (unsigned i = 0; i < HR_ENCRING_N + 5; i++) {
        char f[24];
        snprintf(f, sizeof(f), ")S#7%016u", i);
        hr_encring_push(r, 1000u * i, f, 20);
    }
    CHECK_INT(hr_encring_count(r), HR_ENCRING_N);
    CHECK_INT(r->total_frames, HR_ENCRING_N + 5);
    CHECK_INT(r->total_bytes, 20 * (HR_ENCRING_N + 5));

    CHECK(hr_encring_get(r, 0, &rec));
    CHECK_INT(rec.t_ms, 5000);                      /* frames 0..4 rotated out */
    CHECK_STR(rec.raw, ")S#70000000000000005");
    CHECK_INT(rec.len, 20);
    CHECK_INT(rec.kept, 20);
    CHECK(hr_encring_get(r, HR_ENCRING_N - 1, &rec));
    CHECK_INT(rec.t_ms, 1000u * (HR_ENCRING_N + 4));
    CHECK(!hr_encring_get(r, HR_ENCRING_N, &rec));

    /* longer than the slot: kept < len, and the capture log is the source */
    char *big = malloc(400);
    memset(big, 'Q', 399);
    big[399] = '\0';
    hr_encring_push(r, 99, big, 399);
    CHECK(hr_encring_get(r, HR_ENCRING_N - 1, &rec));
    CHECK_INT(rec.len, 399);
    CHECK_INT(rec.kept, HR_ENCRING_RAW);
    CHECK_INT(strlen(rec.raw), HR_ENCRING_RAW);
    free(big);
    free(r);
}

int main(void)
{
    test_length_table_from_the_capture();
    test_each_captured_frame_is_delivered_whole();
    test_burst_of_glued_frames_is_split_by_length();
    test_frame_split_across_chunks_including_the_header();
    test_partial_frame_is_discarded_and_the_next_header_resyncs();
    test_debris_before_a_header_is_dropped_once_encoded_mode_is_seen();
    test_oversized_declared_length_is_refused_and_bounded();
    test_plaintext_frames_still_parse_unchanged();
    test_ring_keeps_the_last_n_oldest_first();
    return TEST_REPORT();
}
