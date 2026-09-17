/*
 * hr_files - the dryer's file protocol: FDFILELIST / FDFILEBLOCK parsing, the
 * stream's big-frame side buffer on both transports, the transfer state
 * machine, and the batch CSV parser.
 *
 * Two kinds of fixture:
 *
 *   REAL (2026-09-17, dryer on 6.0.644170, bench/2026-09-17-batch-history/):
 *   the FDFILELIST replies as captured, the first and last FDFILEBLOCK of
 *   42838.2026-01-13_04.37.csv rebuilt byte for byte from the file that
 *   round-tripped (the frame's own trailer "F8" was seen on the wire before
 *   the parser could read it - the same shape as the lone "A1" in
 *   PROTOCOL_NOTES.md), the header and rows of a real 50-hour batch log,
 *   and the 11-byte data-flash record HRTempFC.txt.
 *
 *   SYNTHETIC: make_block() builds frames in the confirmed shape from any
 *   payload, for the state-machine and side-buffer tests; enc_frame() is a
 *   test-only ")S" encoder for the path a 6.0.644170 machine would take if
 *   it ever encoded a block (it does not - blocks arrive in plaintext - but
 *   the framer must survive it).
 */
#include "hr_enc.h"
#include "hr_files.h"
#include "hr_session.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Synthetic block builder - the dryer's sendFileBlock() in C            */
/* ------------------------------------------------------------------ */
/*
 * "FDFILEBLOCK,%s,%d,%d,%ld," + data (0x0D -> 0x07) + "%02X\r". Returns the
 * length written INCLUDING the CR, so callers can feed the wire form.
 */
static size_t make_block(char *out, size_t cap, const char *name, long block,
                         const char *data, size_t n, long size, int corrupt)
{
    /* (name, bytes, block, size) - the order confirmed live 2026-09-17 */
    int h = snprintf(out, cap, "FDFILEBLOCK,%s,%ld,%ld,%ld,", name, (long)n,
                     block, size);
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\r') {
            c = 0x07;
        }
        out[(size_t)h + i] = (char)c;
        sum += c;
    }
    if (corrupt) {
        sum ^= 0x5a;
    }
    int t = snprintf(out + h + n, cap - (size_t)h - n, "%02X\r", sum & 0xffu);
    return (size_t)h + n + (size_t)t;
}

/* ------------------------------------------------------------------ */
/* A test-only ENCODER for the ")S" transport, the mirror of hr_enc.c    */
/* ------------------------------------------------------------------ */
typedef struct { uint32_t s0, s1, s2, s3; } cipher_t;

static void cipher_init(cipher_t *c, uint32_t nonce)
{
    c->s0 = 2u ^ nonce;
    c->s1 = 1u;
    c->s2 = (~nonce) ^ 4u;
    c->s3 = 3u;
}

static uint8_t cipher_next(cipher_t *c)
{
    uint32_t s0 = c->s0, s1 = c->s1, s2 = c->s2, s3 = c->s3;
    uint32_t t   = s0 ^ (s0 << 23);
    uint32_t s1n = s1 ^ ((s1 << 23) | (s0 >> 9));
    uint32_t nw  = ((s2 >> 26) | (s3 << 6)) ^ s2 ^ t ^ ((t >> 17) | (s1n << 15));
    uint32_t v9  = s3 ^ (s3 >> 26) ^ s1n;
    uint32_t v10 = v9 ^ (s1n >> 17);
    uint8_t ks = (uint8_t)(nw + s2);
    c->s0 = s2; c->s1 = s3; c->s2 = nw; c->s3 = v10;
    return ks;
}

/* Encode `plain` (which must end in '\r') into a ")S" frame. */
static size_t enc_frame(char *out, size_t cap, const char *plain, size_t n,
                        uint32_t nonce)
{
    uint8_t body[2048];
    size_t bn = 0;
    body[bn++] = (uint8_t)(nonce >> 16);
    body[bn++] = (uint8_t)(nonce >> 8);
    body[bn++] = (uint8_t)nonce;
    cipher_t c;
    cipher_init(&c, nonce);
    for (size_t i = 0; i < n; i++) {
        body[bn++] = (uint8_t)plain[i] ^ cipher_next(&c);
    }
    size_t groups = (bn + 2) / 3;
    size_t total = 4 + groups * 4;
    if (total + 1 > cap) {
        return 0;
    }
    out[0] = ')';
    out[1] = 'S';
    out[2] = (char)(0x23 + (total / 64));
    out[3] = (char)(0x23 + (total % 64));
    size_t o = 4;
    for (size_t g = 0; g < groups; g++) {
        uint32_t v = 0;
        int have = 0;
        for (int k = 0; k < 3; k++) {
            size_t idx = g * 3 + (size_t)k;
            v <<= 8;
            if (idx < bn) {
                v |= body[idx];
                have++;
            }
        }
        char q[4];
        q[0] = (char)(0x23 + ((v >> 18) & 63));
        q[1] = (char)(0x23 + ((v >> 12) & 63));
        q[2] = (char)(0x23 + ((v >> 6) & 63));
        q[3] = (char)(0x23 + (v & 63));
        /* '!' pads the chars that carry no input byte */
        if (have < 3) q[3] = '!';
        if (have < 2) q[2] = '!';
        memcpy(out + o, q, 4);
        o += 4;
    }
    out[o] = '\0';
    return o;
}

/* Something that looks like a batch CSV, for block payloads. */
static const char CSV_TEXT[] =
    "PSTF0000000001,TStamp,mTorr,HtrReq,HtrOn,Top-J20,Mid-J17,Bot-J19,Room-J18,Process\r\n"
    "0,9/15/2026 10:30,500000,0,0,68,69,68,72,Loading,\r\n"
    "1,9/15/2026 10:31,500000,0,0,60,58,61,72,Freezing,1/\r\n"
    "2,9/15/2026 10:46,500000,0,0,12,9,14,73,Freezing,1/\r\n"
    "3,9/15/2026 13:02,452,120,1,-8,-10,-6,74,Drying,2/\r\n"
    "4,9/15/2026 13:17,435,120,1,4,2,5,74,D+,Drying,2/\r\n"
    "5,9/15/2026 16:00,204,125,0,41,40,42,75,Final,3/\r\n";

/* ------------------------------------------------------------------ */
/* FDFILELIST                                                          */
/* ------------------------------------------------------------------ */
static void test_fdlist_parse(void)
{
    TEST_CASE("FDFILELIST entry");
    hr_frame_t f;
    hr_fdlist_entry_t e;
    /* the upstream author's real capture, verbatim */
    CHECK(hr_frame_parse("FDFILELIST,HH.37935.dat,0,4341", &f));
    CHECK(hr_fdlist_parse(&f, &e));
    CHECK_STR(e.name, "HH.37935.dat");
    CHECK_INT(e.index, 0);
    CHECK_INT(e.size, 4341);
    CHECK(!e.end);

    CHECK(hr_frame_parse("FDFILELIST,FactoryTest.log,0,596\r", &f));
    CHECK(hr_fdlist_parse(&f, &e));
    CHECK_STR(e.name, "FactoryTest.log");

    /* synthetic: a batch CSV as the image names them */
    CHECK(hr_frame_parse("FDFILELIST,00042.26-09-15_10.30.csv,3,61440", &f));
    CHECK(hr_fdlist_parse(&f, &e));
    CHECK_STR(e.name, "00042.26-09-15_10.30.csv");
    CHECK_INT(e.index, 3);
    CHECK_INT(e.size, 61440);

    TEST_CASE("FDFILELIST end marker");
    CHECK(hr_frame_parse("FDFILELIST,NULL,7,0", &f));
    CHECK(hr_fdlist_parse(&f, &e));
    CHECK(e.end);
    CHECK_INT(e.index, 7);

    TEST_CASE("FDFILELIST rejects");
    CHECK(hr_frame_parse("FDFILELIST,x,1", &f));
    CHECK(!hr_fdlist_parse(&f, &e));
    CHECK(hr_frame_parse("STAT,1,0,0,0,69,151637,0,0,38,1,1,Auto,v6.4,,", &f));
    CHECK(!hr_fdlist_parse(&f, &e));
    CHECK(hr_frame_parse("FDFILELIST,x,a,5", &f));
    CHECK(!hr_fdlist_parse(&f, &e));
}

/* ------------------------------------------------------------------ */
/* FDFILEBLOCK                                                         */
/* ------------------------------------------------------------------ */
static void test_fdblock_header_len(void)
{
    TEST_CASE("block header, incremental");
    const char *hdr = "FDFILEBLOCK,00042.26-09-15_10.30.csv,1024,0,61440,";
    size_t full = strlen(hdr);
    long n = -1;
    /* every proper prefix is "need more", the whole thing is complete */
    for (size_t i = 1; i < full; i++) {
        CHECK_INT(hr_fdblock_header_len(hdr, i, &n), 0);
    }
    CHECK_INT(hr_fdblock_header_len(hdr, full, &n), (long)full);
    CHECK_INT(n, 1024);

    TEST_CASE("block header, not a block");
    CHECK_INT(hr_fdblock_header_len("FDFILELIST,a,0,1,", 17, &n), -1);
    CHECK_INT(hr_fdblock_header_len("STAT,1,0,0,0,", 13, &n), -1);
    CHECK_INT(hr_fdblock_header_len("F", 1, &n), 0);
    CHECK_INT(hr_fdblock_header_len("FD", 2, &n), 0);
    CHECK_INT(hr_fdblock_header_len("FDN", 3, &n), -1);
    /* letters where numbers belong */
    CHECK_INT(hr_fdblock_header_len("FDFILEBLOCK,a,x,1,2,", 20, &n), -1);
    /* a block bigger than the dryer ever sends */
    CHECK_INT(hr_fdblock_header_len("FDFILEBLOCK,a,1025,0,9,", 23, &n), -1);
    /* the author's empty reply */
    CHECK_INT(hr_fdblock_header_len("FDFILEBLOCK,,0,0,0,", 19, &n), 19);
    CHECK_INT(n, 0);
}

static void test_fdblock_parse(void)
{
    char wire[1400];
    hr_fdblock_t b;

    TEST_CASE("block parse, CSV payload");
    size_t wl = make_block(wire, sizeof(wire), "00042.26-09-15_10.30.csv", 0,
                           CSV_TEXT, strlen(CSV_TEXT), 61440, 0);
    CHECK(wire[wl - 1] == '\r');
    CHECK(hr_fdblock_parse(wire, wl - 1, &b));   /* without the CR */
    CHECK_STR(b.name, "00042.26-09-15_10.30.csv");
    CHECK_INT(b.block, 0);
    CHECK_INT(b.nbytes, (long)strlen(CSV_TEXT));
    CHECK_INT(b.size, 61440);
    CHECK(b.sum_ok);
    /* CRs arrived as BEL and are restored by unbel */
    CHECK(memchr(b.data, '\r', (size_t)b.nbytes) == NULL);
    CHECK(memchr(b.data, '\a', (size_t)b.nbytes) != NULL);
    char copy[1200];
    memcpy(copy, b.data, (size_t)b.nbytes);
    hr_fdblock_unbel(copy, (size_t)b.nbytes);
    CHECK(memcmp(copy, CSV_TEXT, strlen(CSV_TEXT)) == 0);

    TEST_CASE("block parse, full 1024-byte block");
    char data[1024];
    for (int i = 0; i < 1024; i++) {
        data[i] = (char)("0123456789,\r\n"[i % 13]);
    }
    wl = make_block(wire, sizeof(wire), "HB.3793501.dat", 3, data, 1024,
                    5000, 0);
    CHECK(hr_fdblock_parse(wire, wl - 1, &b));
    CHECK_INT(b.block, 3);
    CHECK_INT(b.nbytes, 1024);
    CHECK(b.sum_ok);

    TEST_CASE("block parse, bad checksum is reported not refused");
    wl = make_block(wire, sizeof(wire), "HB.3793501.dat", 3, data, 1024,
                    5000, 1);
    CHECK(hr_fdblock_parse(wire, wl - 1, &b));
    CHECK(!b.sum_ok);

    TEST_CASE("block parse, the author's empty reply");
    const char *empty = "FDFILEBLOCK,,0,0,0,00";
    CHECK(hr_fdblock_parse(empty, strlen(empty), &b));
    CHECK_STR(b.name, "");
    CHECK_INT(b.nbytes, 0);
    CHECK_INT(b.size, 0);
    CHECK(b.sum_ok);

    TEST_CASE("block parse, length mismatch");
    wl = make_block(wire, sizeof(wire), "x.csv", 0, "abc", 3, 3, 0);
    CHECK(!hr_fdblock_parse(wire, wl - 2, &b));  /* one byte short */
    CHECK(!hr_fdblock_parse(wire, wl, &b));      /* CR included */
    CHECK(!hr_fdblock_parse("FDFILEBLOCK,x,3,0,3,abcZZ", 24, &b)); /* bad hex */
}

/* ------------------------------------------------------------------ */
/* Stream + session: the side buffer                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    int blocks;
    size_t last_len;
    char last[1400];
    int frames;
    char verbs[16][HR_MAX_VERB];
} sink_t;

static void on_block(const char *frame, size_t len, void *user)
{
    sink_t *k = (sink_t *)user;
    k->blocks++;
    k->last_len = len;
    memcpy(k->last, frame, len < sizeof(k->last) ? len : sizeof(k->last) - 1);
    k->last[len < sizeof(k->last) ? len : sizeof(k->last) - 1] = '\0';
}

static void on_obs(const hr_frame_t *f, void *user)
{
    sink_t *k = (sink_t *)user;
    if (k->frames < 16) {
        snprintf(k->verbs[k->frames], HR_MAX_VERB, "%s", f->verb);
    }
    k->frames++;
}

static bool tx_drop(const char *d, size_t n, void *u)
{
    (void)d; (void)n; (void)u;
    return true;
}

static void feed_bytewise(hr_session_t *s, const char *d, size_t n,
                          unsigned long t)
{
    for (size_t i = 0; i < n; i++) {
        hr_session_rx(s, d + i, 1, t);
    }
}

static void test_stream_plain_block(void)
{
    static hr_session_t s;
    static char big[HR_FILES_BIGBUF];
    sink_t k;
    char wire[1400];

    TEST_CASE("plaintext block collected whole, byte by byte");
    memset(&k, 0, sizeof(k));
    hr_session_init(&s, tx_drop, NULL);
    hr_session_set_observer(&s, on_obs, &k);
    hr_session_set_block_sink(&s, big, sizeof(big), on_block, &k);

    char data[1024];
    for (int i = 0; i < 1024; i++) {
        data[i] = (char)("abc,def\r\n"[i % 9]);   /* commas, CR, LF inside */
    }
    size_t wl = make_block(wire, sizeof(wire), "00042.26-09-15_10.30.csv", 0,
                           data, 1024, 61440, 0);
    /* a STAT before, the block, a STAT after - all in one byte stream */
    const char *stat = "STAT,1,0,0,0,69,151637,0,0,38,1,1,Auto,v6.4,,\r";
    feed_bytewise(&s, stat, strlen(stat), 1000);
    feed_bytewise(&s, wire, wl, 1001);
    feed_bytewise(&s, stat, strlen(stat), 1002);

    CHECK_INT(k.blocks, 1);
    CHECK_INT(k.last_len, wl - 1);
    hr_fdblock_t b;
    CHECK(hr_fdblock_parse(k.last, k.last_len, &b));
    CHECK(b.sum_ok);
    CHECK_INT(b.nbytes, 1024);
    /* the two STATs and nothing else reached the frame path */
    CHECK_INT(k.frames, 2);
    CHECK_STR(k.verbs[0], "STAT");
    CHECK_STR(k.verbs[1], "STAT");
    CHECK_INT(s.stream.frames_bad, 0);
    CHECK_INT(s.stream.noise_bytes, 0);
    CHECK_INT(s.unknown_verbs, 0);
    CHECK_INT(s.stream.big_frames, 1);
    CHECK_INT(s.blocks_in, 1);
    CHECK(s.link == HR_LINK_UP);

    TEST_CASE("plaintext block in one chunk, then the empty reply");
    memset(&k, 0, sizeof(k));
    hr_session_rx(&s, wire, wl, 2000);
    const char *empty = "FDFILEBLOCK,,0,0,0,00\r";
    hr_session_rx(&s, empty, strlen(empty), 2001);
    CHECK_INT(k.blocks, 2);
    CHECK_STR(k.last, "FDFILEBLOCK,,0,0,0,00");

    TEST_CASE("without a side buffer the block is swallowed, not shredded");
    memset(&k, 0, sizeof(k));
    hr_session_init(&s, tx_drop, NULL);
    hr_session_set_observer(&s, on_obs, &k);
    feed_bytewise(&s, wire, wl, 3000);
    feed_bytewise(&s, stat, strlen(stat), 3001);
    CHECK_INT(k.blocks, 0);
    CHECK_INT(k.frames, 1);               /* the STAT only */
    CHECK_STR(k.verbs[0], "STAT");
    CHECK_INT(s.unknown_verbs, 0);        /* no "A1", no CSV lines as verbs */
    CHECK_INT(s.stream.noise_bytes, 0);   /* BEL bytes were data, not noise */
    CHECK_INT(s.stream.big_dropped, 1);
    CHECK_INT(s.stream.frames_bad, 1);    /* one "block unheld" note */

    TEST_CASE("a stale half block is discarded");
    hr_session_set_block_sink(&s, big, sizeof(big), on_block, &k);
    memset(&k, 0, sizeof(k));
    hr_session_rx(&s, wire, 300, 4000);            /* head only */
    CHECK(s.stream.big_need > 0);
    hr_session_rx(&s, stat, strlen(stat), 4000 + HR_PARTIAL_STALE_MS + 1);
    CHECK_INT(s.stream.big_need, 0);
    CHECK_INT(k.blocks, 0);
    CHECK_INT(k.frames, 1);
    CHECK_INT(s.stream.big_dropped, 2);
}

static void test_stream_encoded_block(void)
{
    static hr_session_t s;
    static char big[HR_FILES_BIGBUF];
    sink_t k;
    char wire[1400];
    static char env[2048];

    TEST_CASE("encoded block: envelope over 511 collected and decoded in place");
    memset(&k, 0, sizeof(k));
    hr_session_init(&s, tx_drop, NULL);
    hr_session_set_observer(&s, on_obs, &k);
    hr_session_set_block_sink(&s, big, sizeof(big), on_block, &k);

    size_t wl = make_block(wire, sizeof(wire), "00042.26-09-15_10.30.csv", 5,
                           CSV_TEXT, strlen(CSV_TEXT), 61440, 0);
    size_t el = enc_frame(env, sizeof(env), wire, wl, 0x1234u);
    CHECK(el > 511);
    CHECK(el < HR_FILES_BIGBUF);
    /* sanity: our encoder round-trips through the real decoder */
    {
        char out[1400];
        int pn = hr_enc_decode(env, el, out, sizeof(out));
        CHECK_INT(pn, (long)(wl - 1));
        CHECK(pn > 0 && memcmp(out, wire, (size_t)pn) == 0);
    }
    feed_bytewise(&s, env, el, 5000);
    CHECK_INT(k.blocks, 1);
    CHECK_INT(k.last_len, wl - 1);
    CHECK(memcmp(k.last, wire, wl - 1) == 0);
    CHECK_INT(s.enc_decoded, 1);
    CHECK_INT(s.enc_undecoded, 0);
    CHECK_INT(s.stream.enc_bad, 0);
    CHECK_INT(s.stream.big_frames, 1);
    CHECK_INT(k.frames, 0);

    TEST_CASE("encoded block: an ordinary short frame still takes the old path");
    const char *reqinfo = ")S#7M<,,(OV)449.H8K_";   /* real REQINFO frame */
    hr_session_rx(&s, reqinfo, strlen(reqinfo), 5001);
    CHECK_INT(k.frames, 1);
    CHECK_STR(k.verbs[0], "REQINFO");
    CHECK_INT(s.enc_decoded, 2);

    TEST_CASE("encoded block: a full 1024-byte block fits the side buffer");
    char data[1024];
    for (int i = 0; i < 1024; i++) {
        data[i] = (char)(0x20 + (i % 95));
        if (data[i] == '\r') data[i] = 'x';
    }
    memset(&k, 0, sizeof(k));
    wl = make_block(wire, sizeof(wire), "00042.26-09-15_10.30.csv", 6, data,
                    1024, 61440, 0);
    el = enc_frame(env, sizeof(env), wire, wl, 0x00abcdu);
    CHECK(el < HR_FILES_BIGBUF);
    hr_session_rx(&s, env, el, 5002);
    CHECK_INT(k.blocks, 1);
    hr_fdblock_t b;
    CHECK(hr_fdblock_parse(k.last, k.last_len, &b));
    CHECK(b.sum_ok);
    CHECK_INT(b.nbytes, 1024);
    CHECK_INT(b.block, 6);

    TEST_CASE("encoded block: no side buffer -> abandoned as before");
    hr_session_init(&s, tx_drop, NULL);
    hr_session_set_observer(&s, on_obs, &k);
    memset(&k, 0, sizeof(k));
    hr_session_rx(&s, env, el, 6000);
    CHECK_INT(k.blocks, 0);
    CHECK_INT(s.stream.enc_bad, 1);
}

/* ------------------------------------------------------------------ */
/* State machine                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    int sends;
    char verb[16];
    char a1[64];
    char a2[16];
    bool refuse;
    int sink_calls;
    int sink_verdict;
    long sink_bytes;
    int list_calls;
    int list_ends;
    hr_fdlist_entry_t last_entry;
    int done_calls;
    hr_files_state_t done_state;
    hr_files_err_t done_err;
} harness_t;

static bool h_send(const char *verb, const char *a1, const char *a2, void *u)
{
    harness_t *h = (harness_t *)u;
    h->sends++;
    snprintf(h->verb, sizeof(h->verb), "%s", verb);
    snprintf(h->a1, sizeof(h->a1), "%s", a1 ? a1 : "");
    snprintf(h->a2, sizeof(h->a2), "%s", a2 ? a2 : "");
    return !h->refuse;
}

static int h_sink(const hr_fdblock_t *b, void *u)
{
    harness_t *h = (harness_t *)u;
    h->sink_calls++;
    h->sink_bytes += b->nbytes;
    return h->sink_verdict;
}

static void h_list(const hr_fdlist_entry_t *e, void *u)
{
    harness_t *h = (harness_t *)u;
    if (e->end) {
        h->list_ends++;
    } else {
        h->list_calls++;
    }
    h->last_entry = *e;
}

static void h_done(hr_files_state_t st, hr_files_err_t err, void *u)
{
    harness_t *h = (harness_t *)u;
    h->done_calls++;
    h->done_state = st;
    h->done_err = err;
}

static void setup(hr_files_t *fs, harness_t *h)
{
    memset(h, 0, sizeof(*h));
    hr_files_init(fs, h_send, h);
    hr_files_set_sink(fs, h_sink, h);
    hr_files_set_list_cb(fs, h_list, h);
    hr_files_set_done_cb(fs, h_done, h);
}

static void list_reply(hr_files_t *fs, const char *name, long idx, long size,
                       unsigned long t)
{
    char line[128];
    snprintf(line, sizeof(line), "FDFILELIST,%s,%ld,%ld", name, idx, size);
    hr_frame_t f;
    hr_frame_parse(line, &f);
    hr_files_on_frame(fs, &f, t);
}

static void block_reply(hr_files_t *fs, const char *name, long blk,
                        const char *data, size_t n, long size, int corrupt,
                        unsigned long t)
{
    char wire[1400];
    size_t wl = make_block(wire, sizeof(wire), name, blk, data, n, size,
                           corrupt);
    hr_files_on_block(fs, wire, wl - 1, t);
}

static void test_sm_list(void)
{
    hr_files_t fs;
    harness_t h;
    setup(&fs, &h);

    TEST_CASE("listing: one FDFILES per entry, ends on NULL");
    CHECK(hr_files_start_list(&fs, ".csv", 100));
    CHECK(hr_files_busy(&fs));
    CHECK_INT(h.sends, 0);               /* nothing until tick */
    hr_files_tick(&fs, 101, true);
    CHECK_INT(h.sends, 1);
    CHECK_STR(h.verb, "FDFILES");
    CHECK_STR(h.a1, ".csv");
    CHECK_STR(h.a2, "0");
    list_reply(&fs, "00041.26-09-10_08.00.csv", 0, 50000, 150);
    CHECK_INT(h.list_calls, 1);
    CHECK_STR(h.last_entry.name, "00041.26-09-10_08.00.csv");
    hr_files_tick(&fs, 200, true);
    CHECK_INT(h.sends, 2);
    CHECK_STR(h.a2, "1");
    list_reply(&fs, "00042.26-09-15_10.30.csv", 1, 61440, 250);
    hr_files_tick(&fs, 300, true);
    CHECK_STR(h.a2, "2");
    list_reply(&fs, "NULL", 2, 0, 350);
    CHECK_INT(h.list_calls, 2);
    CHECK_INT(h.list_ends, 1);
    CHECK_INT(h.done_calls, 1);
    CHECK(h.done_state == HR_FILES_DONE);
    CHECK(!hr_files_busy(&fs));
    CHECK_INT(fs.entries, 2);

    TEST_CASE("listing: a reply for another index ends the list");
    setup(&fs, &h);
    CHECK(hr_files_start_list(&fs, ".dat", 100));
    hr_files_tick(&fs, 101, true);
    list_reply(&fs, "HH.37935.dat", 5, 4341, 150);   /* asked 0, got 5 */
    CHECK_INT(h.list_calls, 0);
    CHECK_INT(h.list_ends, 1);
    CHECK(h.done_state == HR_FILES_DONE);

    TEST_CASE("listing: unsolicited FDFILELIST is ignored");
    setup(&fs, &h);
    list_reply(&fs, "x.csv", 0, 1, 10);
    CHECK_INT(h.list_calls, 0);
    CHECK(fs.state == HR_FILES_IDLE);

    TEST_CASE("listing: bad pattern refused");
    CHECK(!hr_files_start_list(&fs, "has space", 10));
    CHECK(!hr_files_start_list(&fs, "\"q\"", 10));
    CHECK(!hr_files_start_list(&fs, "waytoolongpatternxx", 10));
}

static void test_sm_read(void)
{
    hr_files_t fs;
    harness_t h;
    char d1[1024], d2[1024];
    memset(d1, 'A', sizeof(d1));
    memset(d2, 'B', sizeof(d2));
    const char *name = "00042.26-09-15_10.30.csv";

    TEST_CASE("read: three blocks, sink OK, size from the dryer");
    setup(&fs, &h);
    h.sink_verdict = HR_FILES_SINK_OK;
    CHECK(hr_files_start_read(&fs, name, 2300, 100));
    hr_files_tick(&fs, 101, true);
    CHECK_STR(h.verb, "FILEREAD");
    CHECK_STR(h.a1, name);
    CHECK_STR(h.a2, "0");
    block_reply(&fs, name, 0, d1, 1024, 2300, 0, 150);
    CHECK_INT(h.sink_calls, 1);
    CHECK_INT(fs.received, 1024);
    hr_files_tick(&fs, 200, true);
    CHECK_STR(h.a2, "1");
    block_reply(&fs, name, 1, d2, 1024, 2300, 0, 250);
    hr_files_tick(&fs, 300, true);
    CHECK_STR(h.a2, "2");
    CHECK_INT(hr_files_progress_pct(&fs), 2048 * 100 / 2300);
    block_reply(&fs, name, 2, "tail", 252, 2300, 0, 350);
    CHECK_INT(h.sink_calls, 3);
    CHECK_INT(h.sink_bytes, 2300);
    CHECK_INT(h.done_calls, 1);
    CHECK(h.done_state == HR_FILES_DONE);
    CHECK(h.done_err == HR_FILES_ERR_NONE);
    CHECK_INT(h.sends, 3);
    CHECK_INT(fs.blocks_ok, 3);

    TEST_CASE("read: exact multiple of 1024 ends without an extra request");
    setup(&fs, &h);
    CHECK(hr_files_start_read(&fs, name, 2048, 100));
    hr_files_tick(&fs, 101, true);
    block_reply(&fs, name, 0, d1, 1024, 2048, 0, 150);
    hr_files_tick(&fs, 200, true);
    block_reply(&fs, name, 1, d2, 1024, 2048, 0, 250);
    CHECK(h.done_state == HR_FILES_DONE);
    CHECK_INT(h.sends, 2);

    TEST_CASE("read: inline sending asks for the next block from the reply");
    setup(&fs, &h);
    fs.send_inline = true;
    CHECK(hr_files_start_read(&fs, name, 2300, 100));
    CHECK_INT(h.sends, 0);               /* the first one still waits for tick */
    hr_files_tick(&fs, 101, true);
    CHECK_INT(h.sends, 1);
    block_reply(&fs, name, 0, d1, 1024, 2300, 0, 150);
    CHECK_INT(h.sends, 2);               /* no tick in between */
    CHECK_STR(h.a2, "1");
    block_reply(&fs, name, 1, d2, 1024, 2300, 0, 250);
    CHECK_INT(h.sends, 3);
    block_reply(&fs, name, 2, "tail", 252, 2300, 0, 350);
    CHECK_INT(h.sends, 3);
    CHECK(h.done_state == HR_FILES_DONE);
    /* with WAIT the inline send waits for resume() */
    setup(&fs, &h);
    fs.send_inline = true;
    h.sink_verdict = HR_FILES_SINK_WAIT;
    CHECK(hr_files_start_read(&fs, name, 3000, 100));
    hr_files_tick(&fs, 101, true);
    block_reply(&fs, name, 0, d1, 1024, 3000, 0, 150);
    CHECK_INT(h.sends, 1);
    hr_files_resume(&fs, 200);
    CHECK_INT(h.sends, 2);
    CHECK_STR(h.a2, "1");
    /* listing too */
    setup(&fs, &h);
    fs.send_inline = true;
    CHECK(hr_files_start_list(&fs, ".csv", 100));
    hr_files_tick(&fs, 101, true);
    list_reply(&fs, "a.csv", 0, 10, 150);
    CHECK_INT(h.sends, 2);
    CHECK_STR(h.a2, "1");

    TEST_CASE("read: sink WAIT pauses requests until resume");
    setup(&fs, &h);
    h.sink_verdict = HR_FILES_SINK_WAIT;
    CHECK(hr_files_start_read(&fs, name, 1500, 100));
    hr_files_tick(&fs, 101, true);
    block_reply(&fs, name, 0, d1, 1024, 1500, 0, 150);
    hr_files_tick(&fs, 200, true);
    hr_files_tick(&fs, 300, true);
    CHECK_INT(h.sends, 1);               /* paused */
    hr_files_resume(&fs, 350);
    hr_files_tick(&fs, 400, true);
    CHECK_INT(h.sends, 2);
    CHECK_STR(h.a2, "1");
    block_reply(&fs, name, 1, d2, 476, 1500, 0, 450);
    CHECK(hr_files_busy(&fs));           /* last block, not stored yet */
    CHECK_INT(h.done_calls, 0);
    hr_files_resume(&fs, 500);
    CHECK_INT(h.done_calls, 1);
    CHECK(h.done_state == HR_FILES_DONE);

    TEST_CASE("read: sink that never resumes fails the transfer");
    setup(&fs, &h);
    h.sink_verdict = HR_FILES_SINK_WAIT;
    CHECK(hr_files_start_read(&fs, name, 3000, 100));
    hr_files_tick(&fs, 101, true);
    block_reply(&fs, name, 0, d1, 1024, 3000, 0, 150);
    hr_files_tick(&fs, 150 + HR_FILES_TIMEOUT_MS * 4 + 1, true);
    CHECK(h.done_state == HR_FILES_ERROR);
    CHECK(h.done_err == HR_FILES_ERR_SINK);

    TEST_CASE("read: checksum failure re-asks the same block, then gives up");
    setup(&fs, &h);
    CHECK(hr_files_start_read(&fs, name, 3000, 100));
    hr_files_tick(&fs, 101, true);
    block_reply(&fs, name, 0, d1, 1024, 3000, 1, 150);
    CHECK_INT(h.sink_calls, 0);
    hr_files_tick(&fs, 200, true);
    CHECK_INT(h.sends, 2);
    CHECK_STR(h.a2, "0");
    block_reply(&fs, name, 0, d1, 1024, 3000, 0, 250);   /* good now */
    CHECK_INT(h.sink_calls, 1);
    CHECK_INT(fs.blocks_bad, 1);
    hr_files_tick(&fs, 300, true);
    CHECK_STR(h.a2, "1");
    for (int i = 0; i < HR_FILES_RETRIES + 1; i++) {
        block_reply(&fs, name, 1, d2, 1024, 3000, 1, 400 + i * 100);
        hr_files_tick(&fs, 450 + i * 100, true);
    }
    CHECK(h.done_state == HR_FILES_ERROR);
    CHECK(h.done_err == HR_FILES_ERR_CHECKSUM);

    TEST_CASE("read: a block for the wrong file is ignored, ours re-asked");
    setup(&fs, &h);
    CHECK(hr_files_start_read(&fs, name, 3000, 100));
    hr_files_tick(&fs, 101, true);
    block_reply(&fs, "other.csv", 0, d1, 1024, 3000, 0, 150);
    CHECK_INT(h.sink_calls, 0);
    hr_files_tick(&fs, 200, true);
    CHECK_INT(h.sends, 2);
    CHECK_STR(h.a2, "0");

    TEST_CASE("read: the dryer's empty reply means not found");
    setup(&fs, &h);
    CHECK(hr_files_start_read(&fs, "missing.csv", -1, 100));
    hr_files_tick(&fs, 101, true);
    hr_files_on_block(&fs, "FDFILEBLOCK,,0,0,0,00", 21, 150);
    CHECK(h.done_state == HR_FILES_ERROR);
    CHECK(h.done_err == HR_FILES_ERR_NOTFOUND);

    TEST_CASE("read: too big, up front and from the first block");
    setup(&fs, &h);
    CHECK(!hr_files_start_read(&fs, name, HR_FILES_MAX_SIZE + 1, 100));
    CHECK(fs.err == HR_FILES_ERR_TOO_BIG);
    setup(&fs, &h);
    CHECK(hr_files_start_read(&fs, name, -1, 100));
    hr_files_tick(&fs, 101, true);
    block_reply(&fs, name, 0, d1, 1024, HR_FILES_MAX_SIZE + 5000, 0, 150);
    CHECK(h.done_err == HR_FILES_ERR_TOO_BIG);
    CHECK_INT(h.sink_calls, 0);

    TEST_CASE("read: name with a space or comma refused");
    setup(&fs, &h);
    CHECK(!hr_files_start_read(&fs, "my file.csv", 10, 100));
    CHECK(!hr_files_start_read(&fs, "a,b.csv", 10, 100));
    CHECK(!hr_files_start_read(&fs, "", 10, 100));
    CHECK(fs.state == HR_FILES_IDLE);

    TEST_CASE("read: busy refuses a second transfer");
    setup(&fs, &h);
    CHECK(hr_files_start_read(&fs, name, 3000, 100));
    CHECK(!hr_files_start_read(&fs, name, 3000, 100));
    CHECK(fs.err == HR_FILES_ERR_BUSY);
    CHECK(!hr_files_start_list(&fs, ".csv", 100));
}

static void test_sm_timeouts_and_link(void)
{
    hr_files_t fs;
    harness_t h;
    const char *name = "00042.26-09-15_10.30.csv";

    TEST_CASE("timeout: retries then ERR_TIMEOUT");
    setup(&fs, &h);
    CHECK(hr_files_start_read(&fs, name, 3000, 1000));
    hr_files_tick(&fs, 1001, true);
    CHECK_INT(h.sends, 1);
    unsigned long t = 1001;
    for (int i = 0; i < HR_FILES_RETRIES; i++) {
        t += HR_FILES_TIMEOUT_MS;
        hr_files_tick(&fs, t, true);        /* expires */
        t += 1;
        hr_files_tick(&fs, t, true);        /* re-sends */
        CHECK_INT(h.sends, 2 + i);
        CHECK_STR(h.a2, "0");
    }
    t += HR_FILES_TIMEOUT_MS;
    hr_files_tick(&fs, t, true);
    CHECK(h.done_state == HR_FILES_ERROR);
    CHECK(h.done_err == HR_FILES_ERR_TIMEOUT);
    CHECK_INT(fs.timeouts, HR_FILES_RETRIES + 1);
    CHECK(!hr_files_busy(&fs));

    TEST_CASE("timeout: a late reply after the abort is ignored");
    char d1[1024];
    memset(d1, 'Z', sizeof(d1));
    block_reply(&fs, name, 0, d1, 1024, 3000, 0, t + 5);
    CHECK_INT(h.sink_calls, 0);
    CHECK(fs.state == HR_FILES_ERROR);

    TEST_CASE("timeout: a tick clocked before the send is not a timeout");
    setup(&fs, &h);
    fs.send_inline = true;
    CHECK(hr_files_start_read(&fs, name, 3000, 5000));
    hr_files_tick(&fs, 5001, true);
    block_reply(&fs, name, 0, d1, 1024, 3000, 0, 5100);   /* inline send at 5100 */
    CHECK_INT(h.sends, 2);
    hr_files_tick(&fs, 4000, true);                        /* stale clock */
    CHECK_INT(h.sends, 2);
    CHECK_INT(fs.timeouts, 0);
    CHECK(fs.awaiting);

    TEST_CASE("link down aborts at once and sends nothing more");
    setup(&fs, &h);
    CHECK(hr_files_start_list(&fs, ".csv", 100));
    hr_files_tick(&fs, 101, true);
    hr_files_tick(&fs, 200, false);
    CHECK(h.done_state == HR_FILES_ERROR);
    CHECK(h.done_err == HR_FILES_ERR_LINK);
    hr_files_tick(&fs, 300, true);
    CHECK_INT(h.sends, 1);

    TEST_CASE("cancel");
    setup(&fs, &h);
    CHECK(hr_files_start_read(&fs, name, 3000, 100));
    hr_files_tick(&fs, 101, true);
    hr_files_cancel(&fs, HR_FILES_ERR_NONE, 150);
    CHECK(h.done_err == HR_FILES_ERR_CANCEL);
    CHECK(!hr_files_busy(&fs));
    hr_files_cancel(&fs, HR_FILES_ERR_NONE, 160);   /* idempotent */
    CHECK_INT(h.done_calls, 1);

    TEST_CASE("transport refusing the request");
    setup(&fs, &h);
    h.refuse = true;
    CHECK(hr_files_start_read(&fs, name, 3000, 100));
    hr_files_tick(&fs, 101, true);
    CHECK(h.done_err == HR_FILES_ERR_SEND);

    TEST_CASE("state strings");
    CHECK_STR(hr_files_state_str(HR_FILES_READING), "reading");
    CHECK_STR(hr_files_err_str(HR_FILES_ERR_LINK), "link down");
    CHECK_STR(hr_files_err_str(HR_FILES_ERR_NONE), "");
}

/* ------------------------------------------------------------------ */
/* Pipelining, kick, pacing, timing                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    harness_t h;
    long asked[64];
    int nasked;
} pipe_harness_t;

static bool p_send(const char *verb, const char *a1, const char *a2, void *u)
{
    pipe_harness_t *p = (pipe_harness_t *)u;
    bool ok = h_send(verb, a1, a2, &p->h);
    if (strcmp(verb, "FILEREAD") == 0 && p->nasked < 64) {
        p->asked[p->nasked++] = atol(a2);
    }
    return ok;
}

static void psetup(hr_files_t *fs, pipe_harness_t *p)
{
    memset(p, 0, sizeof(*p));
    hr_files_init(fs, p_send, p);
    hr_files_set_sink(fs, h_sink, &p->h);
    hr_files_set_done_cb(fs, h_done, &p->h);
    fs->send_inline = true;
}

static void test_sm_pipeline(void)
{
    hr_files_t fs;
    pipe_harness_t p;
    char d[1024];
    memset(d, 'P', sizeof(d));
    const char *name = "42838.2026-09-05_07.51.csv";

    TEST_CASE("kick: the first request goes out from the caller, not the tick");
    psetup(&fs, &p);
    CHECK(hr_files_start_read(&fs, name, 5934, 1000));
    CHECK_INT(p.h.sends, 0);
    hr_files_kick(&fs, 1120);
    CHECK_INT(p.h.sends, 1);
    CHECK_INT(p.asked[0], 0);
    CHECK_INT(fs.t_first_ms, 120);
    hr_files_tick(&fs, 1200, true);        /* nothing more at depth 1 */
    CHECK_INT(p.h.sends, 1);

    TEST_CASE("depth 2: two FILEREADs in flight, size known from the list");
    psetup(&fs, &p);
    hr_files_set_pacing(&fs, 2, 0);
    CHECK_INT(fs.depth, 2);
    CHECK(hr_files_start_read(&fs, name, 5934, 100));  /* 6 blocks */
    hr_files_kick(&fs, 100);
    CHECK_INT(p.h.sends, 2);
    CHECK_INT(p.asked[0], 0);
    CHECK_INT(p.asked[1], 1);
    CHECK_INT(fs.inflight, 2);
    CHECK(fs.awaiting);
    block_reply(&fs, name, 0, d, 1024, 5934, 0, 270);
    CHECK_INT(p.h.sends, 3);               /* block 2 asked as 0 arrived */
    CHECK_INT(p.asked[2], 2);
    CHECK_INT(fs.inflight, 2);
    CHECK_INT(fs.block, 1);
    CHECK_INT(fs.next_req, 3);
    block_reply(&fs, name, 1, d, 1024, 5934, 0, 440);
    block_reply(&fs, name, 2, d, 1024, 5934, 0, 610);
    block_reply(&fs, name, 3, d, 1024, 5934, 0, 780);
    CHECK_INT(p.h.sends, 6);               /* 0..5 asked, nothing past the end */
    CHECK_INT(p.asked[5], 5);
    block_reply(&fs, name, 4, d, 1024, 5934, 0, 950);
    CHECK_INT(p.h.sends, 6);
    CHECK_INT(fs.inflight, 1);
    block_reply(&fs, name, 5, d, 814, 5934, 0, 1120);
    CHECK_INT(p.h.sends, 6);
    CHECK_INT(p.h.done_calls, 1);
    CHECK(p.h.done_state == HR_FILES_DONE);
    CHECK_INT(p.h.sink_bytes, 5934);
    CHECK_INT(fs.blocks_ok, 6);
    CHECK_INT(fs.blocks_bad, 0);
    CHECK_INT(fs.t_dryer_ms, 1020);        /* 170 x 6 */
    CHECK_INT(fs.t_paused_ms, 0);

    TEST_CASE("depth 2: size unknown -> one request until the first block");
    psetup(&fs, &p);
    hr_files_set_pacing(&fs, 2, 0);
    CHECK(hr_files_start_read(&fs, "HRTempFC.txt", -1, 100));
    hr_files_kick(&fs, 100);
    CHECK_INT(p.h.sends, 1);
    hr_files_tick(&fs, 200, true);
    CHECK_INT(p.h.sends, 1);
    block_reply(&fs, "HRTempFC.txt", 0, "0,Celsius, ", 11, 11, 0, 300);
    CHECK(p.h.done_state == HR_FILES_DONE);
    CHECK_INT(p.h.sends, 1);
    /* a 3000-byte file: first block reveals 3 blocks; 1 and 2 go out together */
    psetup(&fs, &p);
    hr_files_set_pacing(&fs, 3, 0);
    CHECK(hr_files_start_read(&fs, name, -1, 100));
    hr_files_kick(&fs, 100);
    CHECK_INT(p.h.sends, 1);
    block_reply(&fs, name, 0, d, 1024, 3000, 0, 270);
    CHECK_INT(p.h.sends, 3);
    CHECK_INT(p.asked[1], 1);
    CHECK_INT(p.asked[2], 2);
    CHECK_INT(fs.inflight, 2);

    TEST_CASE("depth 2: the dryer skips a request -> window rewound, re-asked");
    psetup(&fs, &p);
    hr_files_set_pacing(&fs, 2, 0);
    CHECK(hr_files_start_read(&fs, name, 5934, 100));
    hr_files_kick(&fs, 100);                            /* asks 0, 1 */
    block_reply(&fs, name, 0, d, 1024, 5934, 0, 270);   /* asks 2 */
    CHECK_INT(p.h.sends, 3);
    block_reply(&fs, name, 2, d, 1024, 5934, 0, 440);   /* 1 was lost */
    CHECK_INT(fs.blocks_bad, 1);
    CHECK_INT(fs.block, 1);
    CHECK_INT(p.h.sends, 5);                            /* re-asked 1 and 2 */
    CHECK_INT(p.asked[3], 1);
    CHECK_INT(p.asked[4], 2);
    CHECK_INT(fs.inflight, 2);
    block_reply(&fs, name, 1, d, 1024, 5934, 0, 610);
    block_reply(&fs, name, 2, d, 1024, 5934, 0, 780);
    CHECK_INT(fs.block, 3);
    CHECK_INT(p.h.sink_calls, 3);
    /* a duplicate of an accepted block also rewinds, and is never stored */
    block_reply(&fs, name, 1, d, 1024, 5934, 0, 800);
    CHECK_INT(p.h.sink_calls, 3);
    CHECK_INT(fs.blocks_bad, 2);
    CHECK_INT(fs.block, 3);
    CHECK_INT(fs.next_req, 5);                          /* 3 and 4 re-asked */

    TEST_CASE("depth 2: mismatches for ever end in ERR_MISMATCH");
    psetup(&fs, &p);
    hr_files_set_pacing(&fs, 2, 0);
    CHECK(hr_files_start_read(&fs, name, 5934, 100));
    hr_files_kick(&fs, 100);
    for (int i = 0; i < HR_FILES_RETRIES * 3 + 1; i++) {
        block_reply(&fs, "other.csv", 0, d, 1024, 5934, 0, 200 + i * 10);
    }
    CHECK(p.h.done_state == HR_FILES_ERROR);
    CHECK(p.h.done_err == HR_FILES_ERR_MISMATCH);

    TEST_CASE("depth 2: WAIT stops new requests, in-flight replies still land");
    psetup(&fs, &p);
    hr_files_set_pacing(&fs, 2, 0);
    p.h.sink_verdict = HR_FILES_SINK_WAIT;
    CHECK(hr_files_start_read(&fs, name, 5934, 100));
    hr_files_kick(&fs, 100);                            /* 0, 1 out */
    block_reply(&fs, name, 0, d, 1024, 5934, 0, 270);   /* WAIT: no request */
    CHECK_INT(p.h.sends, 2);
    CHECK(fs.paused);
    CHECK_INT(fs.inflight, 1);
    block_reply(&fs, name, 1, d, 1024, 5934, 0, 440);   /* still accepted */
    CHECK_INT(p.h.sink_calls, 2);
    CHECK_INT(fs.block, 2);
    CHECK_INT(fs.inflight, 0);
    CHECK_INT(p.h.sends, 2);
    hr_files_tick(&fs, 500, true);
    CHECK_INT(p.h.sends, 2);
    p.h.sink_verdict = HR_FILES_SINK_OK;
    hr_files_resume(&fs, 700);
    CHECK_INT(p.h.sends, 4);                            /* 2, 3 */
    CHECK_INT(p.asked[2], 2);
    CHECK_INT(p.asked[3], 3);
    CHECK_INT(fs.t_paused_ms, 430);                     /* 270 -> 700 */

    TEST_CASE("depth 2: timeout rewinds the whole window");
    psetup(&fs, &p);
    hr_files_set_pacing(&fs, 2, 0);
    CHECK(hr_files_start_read(&fs, name, 5934, 100));
    hr_files_kick(&fs, 100);
    hr_files_tick(&fs, 100 + HR_FILES_TIMEOUT_MS, true);
    CHECK_INT(fs.timeouts, 1);
    CHECK_INT(p.h.sends, 4);                            /* 0, 1 again */
    CHECK_INT(p.asked[2], 0);
    CHECK_INT(p.asked[3], 1);

    TEST_CASE("gap: requests spaced by gap_ms, the held one goes from tick");
    psetup(&fs, &p);
    hr_files_set_pacing(&fs, 1, 500);
    CHECK(hr_files_start_read(&fs, name, 5934, 100));
    hr_files_kick(&fs, 100);
    CHECK_INT(p.h.sends, 1);
    block_reply(&fs, name, 0, d, 1024, 5934, 0, 270);   /* too soon: held */
    CHECK_INT(p.h.sends, 1);
    CHECK(fs.pending);
    hr_files_tick(&fs, 400, true);
    CHECK_INT(p.h.sends, 1);
    hr_files_tick(&fs, 650, true);
    CHECK_INT(p.h.sends, 2);
    CHECK_INT(p.asked[1], 1);

    TEST_CASE("pacing clamps");
    hr_files_set_pacing(&fs, 0, 0);
    CHECK_INT(fs.depth, 1);
    hr_files_set_pacing(&fs, 99, 0);
    CHECK_INT(fs.depth, HR_FILES_DEPTH_MAX);

    TEST_CASE("depth 1 behaves exactly as before: one request per reply");
    psetup(&fs, &p);
    CHECK(hr_files_start_read(&fs, name, 5934, 100));
    hr_files_kick(&fs, 100);
    for (int i = 0; i < 5; i++) {
        CHECK_INT(p.h.sends, i + 1);
        CHECK_INT(fs.inflight, 1);
        block_reply(&fs, name, i, d, 1024, 5934, 0, 270 + i * 170);
    }
    block_reply(&fs, name, 5, d, 814, 5934, 0, 270 + 5 * 170);
    CHECK_INT(p.h.sends, 6);
    CHECK(p.h.done_state == HR_FILES_DONE);
}

/* ------------------------------------------------------------------ */
/* Batch CSV                                                           */
/* ------------------------------------------------------------------ */
static void test_csv(void)
{
    hr_csv_cols_t c;
    hr_csv_row_t r;
    char line[256];

    TEST_CASE("csv header, image variant 1 (Top,Mid,Bot,Room)");
    CHECK(hr_csv_header_parse(
        "PSTF0000000001,TStamp,mTorr,HtrReq,HtrOn,Top-J20,Mid-J17,Bot-J19,Room-J18,Process\r\n",
        &c));
    CHECK_INT(c.ncols, 10);
    CHECK_INT(c.tstamp, 1);
    CHECK_INT(c.mtorr, 2);
    CHECK_INT(c.htrreq, 3);
    CHECK_INT(c.htron, 4);
    CHECK_INT(c.top, 5);
    CHECK_INT(c.mid, 6);
    CHECK_INT(c.bot, 7);
    CHECK_INT(c.room, 8);
    CHECK_INT(c.process, 9);

    TEST_CASE("csv header, image variant 2 (Mid,Bot,J20,Room)");
    CHECK(hr_csv_header_parse(
        "X,TStamp,mTorr,HtrReq,HtrOn,Mid-J17,Bot-J19,J20,Room-J18,Process", &c));
    CHECK_INT(c.mid, 5);
    CHECK_INT(c.bot, 6);
    CHECK_INT(c.top, 7);
    CHECK_INT(c.room, 8);

    TEST_CASE("csv header, image variant 3 (Trays,J19,J20,Room)");
    CHECK(hr_csv_header_parse(
        "X,TStamp,mTorr,HtrReq,HtrOn,Trays-J17,J19,J20,Room-J18,Process", &c));
    CHECK_INT(c.mid, 5);
    CHECK_INT(c.bot, 6);
    CHECK_INT(c.top, 7);
    CHECK_INT(c.room, 8);

    TEST_CASE("csv header, verbose build has extra columns");
    CHECK(hr_csv_header_parse(
        "X,TStamp,mTorr,HtrReq,HtrOn,Top-J20,Mid-J17,Bot-J19,Room-J18,Process,"
        "Heater Relay,HeatCyc,VacCyc,mT~Hr,mT~Mid,LowF,Auto", &c));
    CHECK_INT(c.ncols, 17);
    CHECK_INT(c.room, 8);

    TEST_CASE("csv header without TStamp is not a header");
    CHECK(!hr_csv_header_parse("0,9/15/2026 10:30,500000,0,0,68,69,68,72,Loading,", &c));

    TEST_CASE("csv row");
    CHECK(hr_csv_header_parse(
        "PSTF0000000001,TStamp,mTorr,HtrReq,HtrOn,Top-J20,Mid-J17,Bot-J19,Room-J18,Process",
        &c));
    strcpy(line, "3,9/15/2026 13:02,452,120,1,-8,-10,-6,74,Drying,2/\r\n");
    CHECK(hr_csv_row_parse(line, &c, &r));
    CHECK_INT(r.counter, 3);
    CHECK(r.have_time);
    CHECK_INT(r.month, 9);
    CHECK_INT(r.day, 15);
    CHECK_INT(r.year, 2026);
    CHECK_INT(r.hour, 13);
    CHECK_INT(r.minute, 2);
    CHECK_INT(r.mtorr, 452);
    CHECK_INT(r.htrreq, 120);
    CHECK_INT(r.htron, 1);
    CHECK_INT(r.top_f, -8);
    CHECK_INT(r.mid_f, -10);
    CHECK_INT(r.bot_f, -6);
    CHECK_INT(r.room_f, 74);
    CHECK_STR(r.process, "Drying");

    TEST_CASE("csv row with a missing thermocouple (--) and BEL line end");
    strcpy(line, "4,9/15/2026 13:17,435,120,1,--,2,5,74,Drying,\a\n");
    CHECK(hr_csv_row_parse(line, &c, &r));
    CHECK_INT(r.top_f, HR_CSV_NO_TEMP);
    CHECK_INT(r.mid_f, 2);
    CHECK_INT(r.room_f, 74);

    TEST_CASE("csv row, pressure cap and two-digit year");
    strcpy(line, "0,9/15/26 10:30,500000,0,0,68,69,68,72,Loading,");
    CHECK(hr_csv_row_parse(line, &c, &r));
    CHECK_INT(r.mtorr, 500000);
    CHECK_INT(r.year, 2026);

    TEST_CASE("csv row, blank and short lines");
    strcpy(line, "\r\n");
    CHECK(!hr_csv_row_parse(line, &c, &r));
    strcpy(line, "7");
    CHECK(!hr_csv_row_parse(line, &c, &r));

    TEST_CASE("csv minutes: monotonic across a day and a month boundary");
    hr_csv_row_t a = {.have_time = true, .year = 2026, .month = 9, .day = 15,
                      .hour = 23, .minute = 59};
    hr_csv_row_t b = {.have_time = true, .year = 2026, .month = 9, .day = 16,
                      .hour = 0, .minute = 1};
    hr_csv_row_t d = {.have_time = true, .year = 2026, .month = 10, .day = 1,
                      .hour = 0, .minute = 0};
    CHECK_INT(hr_csv_row_minutes(&b) - hr_csv_row_minutes(&a), 2);
    /* 16 Sep 00:01 -> 1 Oct 00:00: 14 days 23 h 59 min */
    CHECK_INT(hr_csv_row_minutes(&d) - hr_csv_row_minutes(&b), 15 * 1440 - 1);
    hr_csv_row_t leap = {.have_time = true, .year = 2028, .month = 3, .day = 1,
                         .hour = 0, .minute = 0};
    hr_csv_row_t feb = {.have_time = true, .year = 2028, .month = 2, .day = 28,
                        .hour = 0, .minute = 0};
    CHECK_INT(hr_csv_row_minutes(&leap) - hr_csv_row_minutes(&feb), 2 * 1440);
    hr_csv_row_t none = {.have_time = false};
    CHECK_INT(hr_csv_row_minutes(&none), -1);

    TEST_CASE("csv: the synthetic file end to end");
    {
        char text[sizeof(CSV_TEXT)];
        memcpy(text, CSV_TEXT, sizeof(CSV_TEXT));
        char *ln = text;
        int rows = 0;
        bool have_hdr = false;
        hr_csv_cols_t cc;
        while (ln != NULL && *ln != '\0') {
            char *nl = strchr(ln, '\n');
            char *next = NULL;
            if (nl != NULL) {
                *nl = '\0';
                next = nl + 1;
            }
            if (*ln == '\0') {
                ln = next;
                continue;
            }
            char *cur = ln;
            ln = next;
            if (!have_hdr) {
                have_hdr = hr_csv_header_parse(cur, &cc);
                continue;
            }
            hr_csv_row_t rr;
            if (hr_csv_row_parse(cur, &cc, &rr)) {
                rows++;
                CHECK(rr.room_f >= 72 && rr.room_f <= 75);
            }
        }
        CHECK(have_hdr);
        CHECK_INT(rows, 6);
    }
}


/* ------------------------------------------------------------------ */
/* REAL fixtures                                                       */
/* ------------------------------------------------------------------ */
/* Block 0 (1024 data bytes, CR -> BEL, sum F8) and block 2 (the 675-byte
 * tail, sum 31) of 42838.2026-01-13_04.37.csv, 2723 bytes. */
static const char REAL_BLOCK0[] =
    "FDFILEBLOCK,42838.2026-01-13_04.37.csv,1024,0,2723,,TStamp,mTorr,HtrReq,HtrOn,Top-J20,Mid-"
    "J17,Bot-J19,Room-J18,Process,Master,TTT,HPP,mT~Hr,mT~Mid,LowF,v6.5.0.644170,SN=42838,HL/D,"
    "mT~50,HL/D/6,TmTyp=B,500,600,150,-System Name-,Oil-Free Pump,EndPump:Off,HLG4SA325B7A04906"
    ",\a\n"
    "11,1/13/2026 4:37,29203,0,0,68,67,67,68,Test-f-h-V,150,2500i 150o 500s,0:0:0,0,0,0,0,0, , "
    ", , , ,500,600,150,,Pump On,\a\n"
    "11,1/13/2026 4:38,7319,0,0,67,67,67,69,Test-f-h-V,150,2500i 150o 500s,0:0:0,0,0,0,0,0,\a\n"
    "11,1/13/2026 4:39,9106,0,0,67,66,67,69,Test-f-h-V,150,2500i 150o 500s,0:0:0,0,11,6,0,0,\a\n"
    "11,1/13/2026 4:40,7712,0,0,67,66,67,69,Test-f-h-V,150,2500i 150o 500s,0:0:0,0,11,6,0,0,\a\n"
    "11,1/13/2026 4:41,2973,0,0,67,66,67,70,Test-f-h-V,150,2500i 150o 500s,0:0:0,0,11,6,0,0,\a\n"
    "11,1/13/2026 4:42,1617,0,0,67,66,66,70,Test-f-h-V,150,1617i 150o 500s,0:0:0,0,11,6,0,0,\a\n"
    "11,1/13/2026 4:43,1214,0,0,67,66,66,70,Test-f-h-V,150,1214i 150o 500s,0:0:0,0,11,6,0,0,\a\n"
    "11,1/13/2026 4:44,1064,0,0,67,66,66,70,Test-f-h-V,150,1064i 150o 500s,0:0:0,0,22465,22460,"
    "0,0,\a\n"
    "11,1/13/2026 4:45,1006,0,0,67,66,66,70,Test-f-h-V,150F8";
static const char REAL_BLOCK2[] =
    "FDFILEBLOCK,42838.2026-01-13_04.37.csv,675,2,2723,t-F-h-V,150,519i 150o 500s,0:0:0,0,27371"
    ",27366,0,0,\a\n"
    "11,1/13/2026 4:57,501,0,0,66,66,66,79,Test-F-h-V,150,501i 150o 500s,0:0:0,0,27371,27366,0,"
    "0,\a\n"
    "11,1/13/2026 4:58,519,40,40,71,70,70,80,Test-F-H-V,150,519i 150o 500s,80:80:80,0,27371,273"
    "66,0,0,\a\n"
    "11,1/13/2026 4:59,560,60,60,84,83,81,80,Test-F-H-V,150,560i 150o 500s,80:80:80,0,27672,276"
    "67,0,0,\a\n"
    "11,1/13/2026 5:00,581,60,60,95,95,92,81,Test-F-H-V,150,581i 150o 500s,80:80:80,0,27672,276"
    "67,0,0,\a\n"
    "11,1/13/2026 5:01,1908,24,24,103,103,98,81,Test-f-h-v,150,1908i 150o 500s,0:0:0,0,27672,27"
    "667,0,0, , , , , ,500,600,150,,Pump Off,\a\n"
    "11,1/13/2026 5:02,26185,0,0,102,101,96,80,Test-f-h-v,150,2500i 150o 500s,0:0:0,0,27672,276"
    "67,0,0,\a\n"
    "31";

/* Header and rows of 42838.2026-09-05_08.55.csv (276,530 bytes, 2,986 rows
 * a minute apart). The first header column is the batch name; the verbose
 * columns after Process are this firmware's (v6.5.0.644170). */
static const char REAL_HDR[] =
    "8DNEPNQAM,TStamp,mTorr,HtrReq,HtrOn,Top-J20,Mid-J17,Bot-J19,Room-J18,Process,"
    "Master,TTT,HPP,mT~Hr,mT~Mid,LowF,v6.5.0.644170,SN=42838,HL/D,mT~50,HL/D/6,"
    "TmTyp=B,500,600,120,-System Name-,Oil-Free Pump,EndPump:Off,HLG4SA325B7A04906,";
static const char REAL_ROW_START[] =
    "11,9/5/2026 8:55,60245,0,0,39,43,22,63,Startup,120,2500i 120o 500s,0:0:0,0,0,0,0,0,"
    "10/,10/, , , ,500,600,120,Auto,Pump Off,";
static const char REAL_ROW_DRY[] =
    "11,9/6/2026 9:54,493,60,60,95,96,96,71,Drying-3Z,96,493i 96o 500s,20:17:30,0,2,1,95,96,";
static const char REAL_ROW_END[] =
    "11,9/7/2026 10:40,50758,0,0,-36,-38,-41,51,PreDefrost,150,2500i 150o 500s,0:0:0,0,306,14,-39,-39,";

static void test_real_fixtures(void)
{
    hr_frame_t f;
    hr_fdlist_entry_t e;
    hr_fdblock_t b;

    TEST_CASE("real FDFILELIST replies (capture 02-fdfiles)");
    CHECK(hr_frame_parse("FDFILELIST,42838.2026-01-13_04.37.csv,0,2723", &f));
    CHECK(hr_fdlist_parse(&f, &e));
    CHECK_STR(e.name, "42838.2026-01-13_04.37.csv");
    CHECK_INT(e.index, 0);
    CHECK_INT(e.size, 2723);
    CHECK(!e.end);
    CHECK(hr_frame_parse("FDFILELIST,42838.2026-09-05_08.55.csv,4,276530", &f));
    CHECK(hr_fdlist_parse(&f, &e));
    CHECK_INT(e.size, 276530);

    TEST_CASE("real FDFILEBLOCK 0: header order, BEL, checksum F8");
    size_t l0 = sizeof(REAL_BLOCK0) - 1;
    CHECK_INT(l0, 51 + 1024 + 2);
    CHECK(hr_fdblock_parse(REAL_BLOCK0, l0, &b));
    CHECK_STR(b.name, "42838.2026-01-13_04.37.csv");
    CHECK_INT(b.nbytes, 1024);
    CHECK_INT(b.block, 0);
    CHECK_INT(b.size, 2723);
    CHECK_INT(b.sum, 0xF8);
    CHECK(b.sum_ok);
    /* the file's first line starts with an EMPTY batch name */
    CHECK(memcmp(b.data, ",TStamp,mTorr,", 14) == 0);
    /* line ends arrive as BEL LF and go back to CR LF */
    CHECK(memchr(b.data, '\r', 1024) == NULL);
    char copy[1024];
    memcpy(copy, b.data, 1024);
    hr_fdblock_unbel(copy, 1024);
    CHECK(memchr(copy, '\a', 1024) == NULL);
    CHECK(strstr(REAL_BLOCK0, "150\a\n") == NULL); /* the wire had 150F8 */

    TEST_CASE("real FDFILEBLOCK 2: short last block, checksum 31");
    size_t l2 = sizeof(REAL_BLOCK2) - 1;
    CHECK(hr_fdblock_parse(REAL_BLOCK2, l2, &b));
    CHECK_INT(b.nbytes, 675);
    CHECK_INT(b.block, 2);
    CHECK_INT(b.size, 2723);
    CHECK_INT(b.sum, 0x31);
    CHECK(b.sum_ok);
    CHECK(b.nbytes < HR_FDBLOCK_SIZE);   /* what ends a transfer */

    TEST_CASE("real block through the stream, byte by byte");
    {
        static hr_session_t s;
        static char big[HR_FILES_BIGBUF];
        sink_t k;
        memset(&k, 0, sizeof(k));
        hr_session_init(&s, tx_drop, NULL);
        hr_session_set_observer(&s, on_obs, &k);
        hr_session_set_block_sink(&s, big, sizeof(big), on_block, &k);
        feed_bytewise(&s, REAL_BLOCK0, l0, 100);
        hr_session_rx(&s, "\r", 1, 101);
        /* a real encoded STAT from the same machine follows the block */
        const char *stat = ")S$;K43S_G/%4`K>ISDA0&9Bb):b[T``P8.K^G\\?,8[WNL:07D_U8QJEVN/I@ICbRVF/I1\\K1aU6=121$P)8H.Y!";
        hr_session_rx(&s, stat, strlen(stat), 102);
        CHECK_INT(k.blocks, 1);
        CHECK_INT(k.last_len, l0);
        CHECK_INT(k.frames, 1);
        CHECK_STR(k.verbs[0], "STAT");
        CHECK_INT(s.stream.frames_bad, 0);
        CHECK_INT(s.stream.noise_bytes, 0);
        CHECK_INT(s.unknown_verbs, 0);
    }

    TEST_CASE("real block through the state machine");
    {
        hr_files_t fs;
        harness_t h;
        setup(&fs, &h);
        CHECK(hr_files_start_read(&fs, "42838.2026-01-13_04.37.csv", 2723, 100));
        hr_files_tick(&fs, 101, true);
        hr_files_on_block(&fs, REAL_BLOCK0, l0, 180);
        CHECK_INT(h.sink_calls, 1);
        CHECK_INT(fs.received, 1024);
        CHECK_INT(fs.block, 1);
        fs.block = 2; /* skip the middle block this fixture set lacks */
        hr_files_tick(&fs, 200, true);
        hr_files_on_block(&fs, REAL_BLOCK2, l2, 280);
        CHECK_INT(h.sink_calls, 2);
        CHECK(h.done_state == HR_FILES_DONE);
    }

    TEST_CASE("real CSV header: batch name first, Room-J18 at 8, 30 columns");
    hr_csv_cols_t c;
    CHECK(hr_csv_header_parse(REAL_HDR, &c));
    CHECK_INT(c.ncols, 24);            /* capped at HR_CSV_COLS_MAX */
    CHECK_INT(c.tstamp, 1);
    CHECK_INT(c.mtorr, 2);
    CHECK_INT(c.htrreq, 3);
    CHECK_INT(c.htron, 4);
    CHECK_INT(c.top, 5);
    CHECK_INT(c.mid, 6);
    CHECK_INT(c.bot, 7);
    CHECK_INT(c.room, 8);
    CHECK_INT(c.process, 9);

    TEST_CASE("real CSV rows: start, drying, end");
    char line[256];
    hr_csv_row_t r;
    strcpy(line, REAL_ROW_START);
    CHECK(hr_csv_row_parse(line, &c, &r));
    CHECK(r.have_time);
    CHECK_INT(r.month, 9); CHECK_INT(r.day, 5); CHECK_INT(r.year, 2026);
    CHECK_INT(r.hour, 8); CHECK_INT(r.minute, 55);
    CHECK_INT(r.mtorr, 60245);          /* atmosphere, raw */
    CHECK_INT(r.top_f, 39); CHECK_INT(r.mid_f, 43); CHECK_INT(r.bot_f, 22);
    CHECK_INT(r.room_f, 63);
    CHECK_STR(r.process, "Startup");
    long t0 = hr_csv_row_minutes(&r);
    strcpy(line, REAL_ROW_DRY);
    CHECK(hr_csv_row_parse(line, &c, &r));
    CHECK_INT(r.mtorr, 493);
    CHECK_INT(r.htrreq, 60); CHECK_INT(r.htron, 60);
    CHECK_INT(r.mid_f, 96); CHECK_INT(r.room_f, 71);
    CHECK_STR(r.process, "Drying-3Z");
    CHECK_INT(hr_csv_row_minutes(&r) - t0, 24 * 60 + 59);
    strcpy(line, REAL_ROW_END);
    CHECK(hr_csv_row_parse(line, &c, &r));
    CHECK_INT(r.mid_f, -38); CHECK_INT(r.room_f, 51);
    CHECK_STR(r.process, "PreDefrost");
    CHECK_INT(hr_csv_row_minutes(&r) - t0, 2 * 24 * 60 + 105);

    TEST_CASE("HRTempFC.txt: a data-flash record reads like any file");
    /* FILEREAD HRTempFC.txt 0 -> FDFILEBLOCK,HRTempFC.txt,11,0,11,0,Celsius, XX
     * (the panel's unit: "<0|1>,<Celsius|Fahrenheit>, "; 0,Celsius live) */
    {
        char f[128];
        size_t fl = make_block(f, sizeof(f), "HRTempFC.txt", 0, "0,Celsius, ",
                               11, 11, 0);
        hr_fdblock_t b;
        CHECK(hr_fdblock_parse(f, fl - 1, &b)); /* without the CR */
        CHECK(b.sum_ok);
        CHECK_INT(b.nbytes, 11);
        CHECK_INT(b.size, 11);
        CHECK(memcmp(b.data, "0,Celsius, ", 11) == 0);
    }
}

int main(void)
{
    test_fdlist_parse();
    test_fdblock_header_len();
    test_fdblock_parse();
    test_stream_plain_block();
    test_stream_encoded_block();
    test_sm_list();
    test_sm_read();
    test_sm_timeouts_and_link();
    test_sm_pipeline();
    test_csv();
    test_real_fixtures();
    return TEST_REPORT();
}
