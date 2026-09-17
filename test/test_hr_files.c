/*
 * The dryer's file protocol: what we will say to the machine, what we make of
 * its answers, and the framing that carries a kilobyte of somebody's CSV
 * through a reassembler built for short printable lines.
 */
#include "hr_files.h"
#include "hr_protocol.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* What we are willing to say                                          */
/* ------------------------------------------------------------------ */
static void test_arguments_we_will_send(void)
{
    TEST_CASE("arguments we will send");
    /* The names the dryer actually hands out, and the usual patterns. */
    CHECK(hr_files_arg_ok("42838.2026-09-05_08.55.csv"));
    CHECK(hr_files_arg_ok(".csv"));
    CHECK(hr_files_arg_ok(".dat"));
    CHECK(hr_files_arg_ok("HRTempFC.txt"));
    CHECK(hr_files_arg_ok("HH.37935.dat"));
    /* Its match is case-sensitive, so lower case cannot collide. */
    CHECK(hr_files_arg_ok("model.csv"));
    CHECK(hr_files_arg_ok("deltas.csv"));
}

static void test_arguments_we_refuse(void)
{
    TEST_CASE("arguments we refuse");
    CHECK(!hr_files_arg_ok(NULL));
    CHECK(!hr_files_arg_ok(""));
    CHECK(!hr_files_arg_ok("0123456789012345678901234567890123456789012345678"));

    /*
     * A CARRIAGE RETURN is the dangerous one. Our frames end with it, so a
     * name carrying one would end the frame early and leave the rest of it
     * to be read as a command of the sender's choosing - past the verb
     * allow-list, past the control switch, past everything. This is the
     * reason the gate exists.
     */
    CHECK(!hr_files_arg_ok("\rREBOOT"));
    CHECK(!hr_files_arg_ok("a\rDEL x"));
    CHECK(!hr_files_arg_ok("a\nb"));
    CHECK(!hr_files_arg_ok("a\tb"));
    CHECK(!hr_files_arg_ok("a\x01" "b"));
    CHECK(!hr_files_arg_ok("a\x7f" "b"));
    CHECK(!hr_files_arg_ok("caf\xc3\xa9.csv"));   /* not printable ASCII */

    /* Anything that would split the argument or corrupt the reply. */
    CHECK(!hr_files_arg_ok("two words.csv"));
    CHECK(!hr_files_arg_ok("a\"b.csv"));
    CHECK(!hr_files_arg_ok("a,b.csv"));
    CHECK(!hr_files_arg_ok("sub/dir.csv"));
    CHECK(!hr_files_arg_ok("sub\\dir.csv"));
    CHECK(!hr_files_arg_ok("*.csv"));
    CHECK(!hr_files_arg_ok("a?.csv"));
}

static void test_a_verb_hiding_in_a_name(void)
{
    /*
     * The dryer picks a command by searching the whole line, in its own
     * order, and acts on the first verb it finds - so a capitalised verb
     * inside a file name wins over the FILEREAD we meant. MODEL.CSV contains
     * DEL, which the machine tests long before either verb of ours.
     */
    TEST_CASE("a verb hiding in a name");
    CHECK(!hr_files_arg_ok("MODEL.CSV"));
    CHECK_STR(hr_files_arg_verb("MODEL.CSV"), "DEL");
    CHECK(!hr_files_arg_ok("DELTA"));
    CHECK(!hr_files_arg_ok("ADDENDUM.csv"));
    CHECK_STR(hr_files_arg_verb("ADDENDUM.csv"), "ADD");
    CHECK(!hr_files_arg_ok("REBOOT"));
    CHECK(!hr_files_arg_ok("myDIRfile"));
    CHECK(!hr_files_arg_ok("COPYme.csv"));
    CHECK(!hr_files_arg_ok("bigDUMP.dat"));
    /* and nothing is flagged that is not there */
    CHECK(hr_files_arg_verb("42838.2026-09-05_08.55.csv") == NULL);
}

/* ------------------------------------------------------------------ */
/* Replies                                                             */
/* ------------------------------------------------------------------ */
static bool parse_entry(const char *line, hr_file_entry_t *e)
{
    hr_frame_t f;
    return hr_frame_parse(line, &f) && hr_file_entry_parse(&f, e);
}

static void test_listing_entries(void)
{
    TEST_CASE("listing entries");
    hr_file_entry_t e;
    CHECK(parse_entry("FDFILELIST,42838.2026-09-05_08.55.csv,0,276480\r", &e));
    CHECK_STR(e.name, "42838.2026-09-05_08.55.csv");
    CHECK_INT(e.index, 0);
    CHECK_INT(e.size, 276480);
    CHECK(!e.last);

    /* Past the last match the dryer names the entry NULL. */
    CHECK(parse_entry("FDFILELIST,NULL,7,0\r", &e));
    CHECK(e.last);
    CHECK_INT(e.index, 7);
    CHECK_STR(e.name, "");

    CHECK(!parse_entry("STAT,1,0,0,0,69,151882,0,0,38,0,1,Auto,v6.4,,\r", &e));
    CHECK(!parse_entry("FDFILELIST,onlyname\r", &e));
    CHECK(!parse_entry("FDFILELIST,x,notanumber,10\r", &e));
}

/* Build a block frame the way the dryer does: the file's CRs travel as BEL,
 * and the frame ends with the data bytes summed mod 256 in upper-case hex. */
static size_t make_block(char *out, size_t cap, const char *name, long block,
                         long size, const char *data, size_t n)
{
    int hn = snprintf(out, cap, "FDFILEBLOCK,%s,%ld,%ld,%ld,", name,
                      (long)n, block, size);
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++) {
        char c = data[i] == '\r' ? (char)0x07 : data[i];
        out[hn + i] = c;
        sum += (unsigned char)c;
    }
    snprintf(out + hn + n, cap - (size_t)hn - n, "%02X", sum & 0xffu);
    return (size_t)hn + n + 2;
}

static void test_measuring_a_block_as_it_arrives(void)
{
    /*
     * The stream asks as each byte lands, so the answer has to be "not yet"
     * until the header is whole and "never" as soon as it cannot be one.
     */
    TEST_CASE("measuring a block as it arrives");
    char frame[256];
    const size_t len = make_block(frame, sizeof(frame), "t.csv", 0, 5,
                                  "a,b\nc", 5);
    size_t total = 0;

    CHECK_INT(hr_file_block_measure("FDFILE", 6, &total), 0);
    CHECK_INT(hr_file_block_measure("STAT,1,", 7, &total), -1);
    CHECK_INT(hr_file_block_measure("FDFILELIST,x,0,1", 16, &total), -1);
    /* header present but the fields not yet complete */
    CHECK_INT(hr_file_block_measure(frame, 20, &total), 0);
    /* whole header: now it knows the length */
    CHECK_INT(hr_file_block_measure(frame, len, &total), 1);
    CHECK_INT((long)total, (long)len);

    /* A header claiming more than a block holds is not one of ours. */
    CHECK_INT(hr_file_block_measure("FDFILEBLOCK,t.csv,2000,0,9,", 27, &total),
              -1);
    /* Nor is one with a name too long to be a name. */
    CHECK_INT(hr_file_block_measure(
                  "FDFILEBLOCK,0123456789012345678901234567890123456789"
                  "01234567,10,0,10,", 69, &total), -1);
}

static void test_parsing_a_block(void)
{
    TEST_CASE("parsing a block");
    char frame[2048];
    const char body[] = "1,9/15/2026 13:02,452,120\r\n2,9/15/2026 13:03,435,120\r\n";
    const size_t n = sizeof(body) - 1;
    const size_t len = make_block(frame, sizeof(frame), "t.csv", 3, 4096,
                                  body, n);

    hr_file_block_t b;
    CHECK(hr_file_block_parse(frame, len, &b));
    CHECK_STR(b.name, "t.csv");
    CHECK_INT(b.block, 3);
    CHECK_INT(b.nbytes, (long)n);
    CHECK_INT(b.size, 4096);
    CHECK(b.sum_ok);
    CHECK(!b.missing);
    /* The data are still the dryer's: CRs arrive as BEL. */
    CHECK(memchr(b.data, '\r', n) == NULL);
    CHECK(memchr(b.data, 0x07, n) != NULL);

    /* Restored in place, it is the file's own bytes again. */
    char *data = frame + (b.data - frame);
    hr_file_restore_cr(data, (size_t)b.nbytes);
    CHECK(memcmp(data, body, n) == 0);

    /* A frame shorter or longer than its header promises is not parsed. */
    CHECK(!hr_file_block_parse(frame, len - 1, &b));
    CHECK(!hr_file_block_parse(frame, len + 1, &b));
}

static void test_a_corrupt_block_is_reported_not_hidden(void)
{
    /*
     * A checksum mismatch is not a parse failure: the caller needs the block
     * number to ask for it again, which is the only way to fix it.
     */
    TEST_CASE("a corrupt block is reported, not hidden");
    char frame[256];
    size_t len = make_block(frame, sizeof(frame), "t.csv", 1, 20, "hello", 5);
    frame[len - 1] = frame[len - 1] == '0' ? '1' : '0';   /* break the sum */

    hr_file_block_t b;
    CHECK(hr_file_block_parse(frame, len, &b));
    CHECK(!b.sum_ok);
    CHECK_INT(b.block, 1);

    /* Two hex digits that are not hex at all: that IS malformed. */
    frame[len - 1] = 'z';
    CHECK(!hr_file_block_parse(frame, len, &b));
}

static void test_the_dryer_saying_it_has_no_such_file(void)
{
    TEST_CASE("no such file");
    const char *miss = "FDFILEBLOCK,,0,0,0,00";
    hr_file_block_t b;
    CHECK(hr_file_block_parse(miss, strlen(miss), &b));
    CHECK(b.missing);
    CHECK_INT(b.nbytes, 0);
    CHECK(b.sum_ok);            /* no bytes sum to zero */
}

/* ------------------------------------------------------------------ */
/* The framing                                                         */
/* ------------------------------------------------------------------ */
/* An ordinary frame, for checking the stream is still in step. */
#define STAT_LINE "STAT,1,0,0,0,69,151882,0,0,38,0,1,Auto,v6.4,,\r"

typedef struct {
    int frames;
    char last_verb[HR_MAX_VERB];
    int longs;
    size_t long_len;
    bool long_enc;
    char long_copy[HR_FILE_LONGBUF];
    int rejects;
} seen_t;

static void on_frame_cb(const hr_frame_t *f, void *user)
{
    seen_t *s = (seen_t *)user;
    s->frames++;
    snprintf(s->last_verb, sizeof(s->last_verb), "%s", f->verb);
}

static void on_long_cb(char *frame, size_t len, bool encoded, void *user)
{
    seen_t *s = (seen_t *)user;
    s->longs++;
    s->long_len = len;
    s->long_enc = encoded;
    memcpy(s->long_copy, frame, len < sizeof(s->long_copy) ? len
                                                           : sizeof(s->long_copy));
}

static void on_reject_cb(const char *frame, size_t len, const char *why,
                         void *user)
{
    (void)frame;
    (void)len;
    (void)why;
    ((seen_t *)user)->rejects++;
}

static void test_a_block_survives_the_line_reassembler(void)
{
    /*
     * This is the bug the whole side buffer exists for. A block carries line
     * feeds, commas and a checksum, and runs past HR_MAX_FRAME; fed to the
     * line rules it came apart into rubbish frames and a stray two-character
     * tail. Whole, it arrives once, exactly as sent.
     */
    TEST_CASE("a block survives the line reassembler");
    char block[HR_FILE_LONGBUF];
    char data[HR_FILE_BLOCK];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (i % 40 == 39) ? '\n' : (char)('a' + (i % 26));
    }
    const size_t blen = make_block(block, sizeof(block), "t.csv", 0, 8192,
                                   data, sizeof(data));
    CHECK(blen > HR_MAX_FRAME);

    seen_t seen;
    memset(&seen, 0, sizeof(seen));
    char side[HR_FILE_LONGBUF];
    hr_stream_t st;
    hr_stream_init(&st);
    hr_stream_set_reject_cb(&st, on_reject_cb, &seen);
    hr_stream_set_long(&st, side, sizeof(side), hr_file_block_measure,
                       on_long_cb, &seen);

    /* In awkward chunks, because USB reads do not respect frames. */
    const char *p = block;
    size_t left = blen;
    while (left > 0) {
        const size_t take = left < 7 ? left : 7;
        hr_stream_feed(&st, p, take, on_frame_cb, &seen);
        p += take;
        left -= take;
    }
    hr_stream_feed(&st, "\r", 1, on_frame_cb, &seen);

    CHECK_INT(seen.longs, 1);
    CHECK_INT((long)seen.long_len, (long)blen);
    CHECK(!seen.long_enc);
    CHECK(memcmp(seen.long_copy, block, blen) == 0);
    CHECK_INT(seen.frames, 0);      /* and nothing bogus came out of it */
    CHECK_INT(seen.rejects, 0);
    CHECK_INT((int)st.long_frames, 1);

    /* An ordinary frame right behind it still parses. */
    hr_stream_feed(&st, STAT_LINE, strlen(STAT_LINE), on_frame_cb,
                   &seen);
    CHECK_INT(seen.frames, 1);
    CHECK_STR(seen.last_verb, "STAT");
}

static void test_a_block_nobody_can_hold_is_swallowed(void)
{
    /*
     * With no side buffer lent - the feature off, or not built - the bytes
     * must be counted and dropped rather than parsed. A kilobyte of file
     * data loose in the frame path is how "FILEREAD does not work" looked.
     */
    TEST_CASE("a block nobody can hold is swallowed");
    char block[HR_FILE_LONGBUF];
    char data[600];
    memset(data, 'x', sizeof(data));
    memcpy(data, "STAT,1,0,0,0,69,151882,0,0,38,0,1,Auto,v6.4,,\r", 45);
    const size_t blen = make_block(block, sizeof(block), "t.csv", 0, 600,
                                   data, sizeof(data));

    seen_t seen;
    memset(&seen, 0, sizeof(seen));
    hr_stream_t st;
    hr_stream_init(&st);
    hr_stream_set_reject_cb(&st, on_reject_cb, &seen);
    /* measure, but nothing to hold it in */
    hr_stream_set_long(&st, NULL, 0, hr_file_block_measure, NULL, &seen);

    hr_stream_feed(&st, block, blen, on_frame_cb, &seen);
    hr_stream_feed(&st, "\r", 1, on_frame_cb, &seen);

    CHECK_INT(seen.longs, 0);
    /* The STAT-looking bytes inside the file data did NOT become a frame. */
    CHECK_INT(seen.frames, 0);
    CHECK_INT((int)st.long_dropped, 1);

    /* The stream is still in step: a real frame after it parses. */
    hr_stream_feed(&st, STAT_LINE, strlen(STAT_LINE), on_frame_cb,
                   &seen);
    CHECK_INT(seen.frames, 1);
}

static void test_an_oversize_encoded_frame_is_collected(void)
{
    /*
     * On 6.0.644170 the same block arrives inside the ")S" envelope, longer
     * than the line buffer. It goes to the side buffer too, still encoded -
     * the session decodes it there.
     */
    TEST_CASE("an oversize encoded frame is collected");
    seen_t seen;
    memset(&seen, 0, sizeof(seen));
    char side[HR_FILE_LONGBUF];
    hr_stream_t st;
    hr_stream_init(&st);
    hr_stream_set_long(&st, side, sizeof(side), hr_file_block_measure,
                       on_long_cb, &seen);

    /* ")S" then two length characters: 0x23 is zero, so "0#" is 13*64 = 832. */
    char enc[900];
    memset(enc, '#', sizeof(enc));
    enc[0] = ')';
    enc[1] = 'S';
    enc[2] = '0';
    enc[3] = '#';
    const size_t total = (size_t)('0' - 0x23) * 64;
    hr_stream_feed(&st, enc, total, on_frame_cb, &seen);

    CHECK_INT(seen.longs, 1);
    CHECK(seen.long_enc);
    CHECK_INT((long)seen.long_len, (long)total);
    CHECK_INT(seen.frames, 0);
}

/* ------------------------------------------------------------------ */
/* The transfer                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    int sent;
    char last[128];
    bool refuse;        /* the consumer has no room */
    char data[8192];
    size_t len;
    int done;
    hr_files_state_t done_state;
    hr_files_err_t done_err;
    int entries;
    char last_entry[HR_FILE_NAME_MAX];
    bool last_was_end;
    bool fail_send;
} rig_t;

static bool rig_send(const char *verb, const char *arg, const char *index,
                     void *user)
{
    rig_t *r = (rig_t *)user;
    if (r->fail_send) {
        return false;
    }
    r->sent++;
    snprintf(r->last, sizeof(r->last), "%s %s %s", verb, arg, index);
    return true;
}

static void rig_entry(const hr_file_entry_t *e, void *user)
{
    rig_t *r = (rig_t *)user;
    r->entries++;
    r->last_was_end = e->last;
    snprintf(r->last_entry, sizeof(r->last_entry), "%s", e->name);
}

static bool rig_data(const char *data, size_t n, void *user)
{
    rig_t *r = (rig_t *)user;
    if (r->refuse) {
        return false;
    }
    if (r->len + n <= sizeof(r->data)) {
        memcpy(r->data + r->len, data, n);
        r->len += n;
    }
    return true;
}

static void rig_done(hr_files_state_t st, hr_files_err_t err, void *user)
{
    rig_t *r = (rig_t *)user;
    r->done++;
    r->done_state = st;
    r->done_err = err;
}

static void rig_init(rig_t *r, hr_files_t *fs)
{
    memset(r, 0, sizeof(*r));
    hr_files_init(fs, rig_send, r);
    hr_files_set_entry_cb(fs, rig_entry, r);
    hr_files_set_data_cb(fs, rig_data, r);
    hr_files_set_done_cb(fs, rig_done, r);
}

static void feed_frame(hr_files_t *fs, const char *line, unsigned long ms)
{
    hr_frame_t f;
    if (hr_frame_parse(line, &f)) {
        hr_files_on_frame(fs, &f, ms);
    }
}

static void test_a_listing_walks_to_the_end(void)
{
    TEST_CASE("a listing walks to the end");
    rig_t r;
    hr_files_t fs;
    rig_init(&r, &fs);

    CHECK(hr_files_list(&fs, ".csv", 1000));
    hr_files_tick(&fs, 1000, true);
    CHECK_INT(r.sent, 1);
    CHECK_STR(r.last, "FDFILES .csv 0");

    feed_frame(&fs, "FDFILELIST,a.csv,0,100\r", 1100);
    hr_files_tick(&fs, 1100, true);
    CHECK_INT(r.entries, 1);
    CHECK_STR(r.last_entry, "a.csv");
    CHECK_STR(r.last, "FDFILES .csv 1");

    feed_frame(&fs, "FDFILELIST,b.csv,1,200\r", 1200);
    hr_files_tick(&fs, 1200, true);
    CHECK_STR(r.last, "FDFILES .csv 2");

    feed_frame(&fs, "FDFILELIST,NULL,2,0\r", 1300);
    hr_files_tick(&fs, 1300, true);
    CHECK_INT(r.done, 1);
    CHECK_INT(r.done_state, HR_FILES_DONE);
    CHECK_INT((int)fs.entries, 2);
    CHECK(!hr_files_busy(&fs));
    CHECK_INT(r.sent, 3);         /* nothing asked after the end */
}

static void test_an_answer_for_another_index_is_ignored(void)
{
    /* A late answer to a request we already retried must not end the walk. */
    TEST_CASE("an answer for another index is ignored");
    rig_t r;
    hr_files_t fs;
    rig_init(&r, &fs);
    CHECK(hr_files_list(&fs, ".csv", 1000));
    hr_files_tick(&fs, 1000, true);

    feed_frame(&fs, "FDFILELIST,old.csv,5,10\r", 1100);
    CHECK_INT(r.entries, 0);
    CHECK(hr_files_busy(&fs));
}

static void test_a_read_walks_the_blocks(void)
{
    TEST_CASE("a read walks the blocks");
    rig_t r;
    hr_files_t fs;
    rig_init(&r, &fs);

    char frame[HR_FILE_LONGBUF];
    char full[HR_FILE_BLOCK];
    memset(full, 'A', sizeof(full));

    CHECK(hr_files_read(&fs, "t.csv", 1124, 1000));
    hr_files_tick(&fs, 1000, true);
    CHECK_STR(r.last, "FILEREAD t.csv 0");

    size_t n = make_block(frame, sizeof(frame), "t.csv", 0, 1124, full,
                          sizeof(full));
    hr_files_on_block(&fs, frame, n, 1100);
    hr_files_tick(&fs, 1100, true);
    CHECK_INT((long)r.len, 1024);
    CHECK_STR(r.last, "FILEREAD t.csv 1");

    /* A short block is the end of the file. */
    n = make_block(frame, sizeof(frame), "t.csv", 1, 1124, "tail\r\n", 6);
    hr_files_on_block(&fs, frame, n, 1200);
    hr_files_tick(&fs, 1200, true);
    CHECK_INT((long)r.len, 1030);
    CHECK_INT(r.done, 1);
    CHECK_INT(r.done_state, HR_FILES_DONE);
    /* and the file's own line endings came back */
    CHECK(memcmp(r.data + 1024, "tail\r\n", 6) == 0);
    CHECK_INT((int)fs.blocks_ok, 2);
}

static void test_a_bad_checksum_asks_again(void)
{
    TEST_CASE("a bad checksum asks again");
    rig_t r;
    hr_files_t fs;
    rig_init(&r, &fs);
    char frame[512];

    CHECK(hr_files_read(&fs, "t.csv", 2048, 1000));
    hr_files_tick(&fs, 1000, true);

    size_t n = make_block(frame, sizeof(frame), "t.csv", 0, 2048, "hello", 5);
    frame[n - 1] = frame[n - 1] == '0' ? '1' : '0';
    hr_files_on_block(&fs, frame, n, 1100);
    hr_files_tick(&fs, 1100, true);
    CHECK_INT((long)r.len, 0);             /* nothing corrupt was passed on */
    CHECK_STR(r.last, "FILEREAD t.csv 0"); /* the same block, again */
    CHECK_INT((int)fs.blocks_bad, 1);
    CHECK_INT(r.sent, 2);

    /* Good the second time round. */
    n = make_block(frame, sizeof(frame), "t.csv", 0, 5, "hello", 5);
    hr_files_on_block(&fs, frame, n, 1200);
    hr_files_tick(&fs, 1200, true);
    CHECK_INT((long)r.len, 5);
    CHECK_INT(r.done_state, HR_FILES_DONE);
}

static void test_a_silent_dryer_times_out(void)
{
    TEST_CASE("a silent dryer times out");
    rig_t r;
    hr_files_t fs;
    rig_init(&r, &fs);
    CHECK(hr_files_read(&fs, "t.csv", 4096, 1000));
    hr_files_tick(&fs, 1000, true);
    CHECK_INT(r.sent, 1);

    unsigned long t = 1000;
    for (int i = 0; i < HR_FILES_RETRIES; i++) {
        t += HR_FILES_REPLY_MS;
        hr_files_tick(&fs, t, true);
    }
    CHECK_INT(r.sent, 1 + HR_FILES_RETRIES);
    CHECK_INT(r.done, 0);

    t += HR_FILES_REPLY_MS;
    hr_files_tick(&fs, t, true);
    CHECK_INT(r.done, 1);
    CHECK_INT(r.done_state, HR_FILES_FAILED);
    CHECK_INT(r.done_err, HR_FILES_ERR_TIMEOUT);
    CHECK_INT((int)fs.timeouts, HR_FILES_RETRIES + 1);
}

static void test_the_link_going_down_ends_it(void)
{
    TEST_CASE("the link going down ends it");
    rig_t r;
    hr_files_t fs;
    rig_init(&r, &fs);
    CHECK(hr_files_read(&fs, "t.csv", 4096, 1000));
    hr_files_tick(&fs, 1000, true);
    hr_files_tick(&fs, 1100, false);
    CHECK_INT(r.done, 1);
    CHECK_INT(r.done_err, HR_FILES_ERR_LINK);
    CHECK(!hr_files_busy(&fs));
}

static void test_a_full_consumer_pauses_the_transfer(void)
{
    /*
     * The browser is slower than the dryer. When the buffer between them
     * fills, the transfer stops asking rather than dropping bytes, and the
     * block that could not be taken is asked for again - one repeated
     * kilobyte being cheaper than a second buffer on a chip with no room.
     */
    TEST_CASE("a full consumer pauses the transfer");
    rig_t r;
    hr_files_t fs;
    rig_init(&r, &fs);
    char frame[HR_FILE_LONGBUF];
    char full[HR_FILE_BLOCK];
    memset(full, 'B', sizeof(full));

    CHECK(hr_files_read(&fs, "t.csv", 4096, 1000));
    hr_files_tick(&fs, 1000, true);

    r.refuse = true;
    size_t n = make_block(frame, sizeof(frame), "t.csv", 0, 4096, full,
                          sizeof(full));
    hr_files_on_block(&fs, frame, n, 1100);
    hr_files_tick(&fs, 1100, true);
    CHECK_INT((long)r.len, 0);
    CHECK_INT(r.sent, 1);           /* nothing new asked while full */
    CHECK(fs.paused);
    /* and it does not time out while the hold-up is ours */
    hr_files_tick(&fs, 1100 + HR_FILES_REPLY_MS * 5, true);
    CHECK_INT(r.done, 0);

    r.refuse = false;
    hr_files_resume(&fs, 2000);
    hr_files_tick(&fs, 2000, true);
    CHECK_INT(r.sent, 2);
    CHECK_STR(r.last, "FILEREAD t.csv 0");   /* the same block again */
    hr_files_on_block(&fs, frame, n, 2100);
    hr_files_tick(&fs, 2100, true);
    CHECK_INT((long)r.len, 1024);
}

static void test_no_such_file(void)
{
    TEST_CASE("no such file ends the read");
    rig_t r;
    hr_files_t fs;
    rig_init(&r, &fs);
    CHECK(hr_files_read(&fs, "nope.csv", 100, 1000));
    hr_files_tick(&fs, 1000, true);

    char miss[] = "FDFILEBLOCK,,0,0,0,00";
    hr_files_on_block(&fs, miss, strlen(miss), 1100);
    CHECK_INT(r.done, 1);
    CHECK_INT(r.done_err, HR_FILES_ERR_MISSING);
}

static void test_what_we_will_not_start(void)
{
    /*
     * The gate is on the way IN, so a request that would put something
     * dangerous on the wire never becomes a transfer at all - and nothing is
     * sent. This is the regression test for the frame-injection hole: a
     * pattern carrying a carriage return would have been two commands.
     */
    TEST_CASE("what we will not start");
    rig_t r;
    hr_files_t fs;
    rig_init(&r, &fs);

    CHECK(!hr_files_list(&fs, "\rREBOOT", 1000));
    CHECK(!hr_files_busy(&fs));
    CHECK_INT(r.sent, 0);
    hr_files_tick(&fs, 1100, true);
    CHECK_INT(r.sent, 0);

    CHECK(!hr_files_list(&fs, "DELTA", 1000));
    CHECK(!hr_files_read(&fs, "MODEL.CSV", 10, 1000));
    CHECK(!hr_files_read(&fs, "a b.csv", 10, 1000));
    CHECK(!hr_files_read(&fs, ".hidden.csv", 10, 1000));  /* not from a listing */
    CHECK_INT(r.sent, 0);

    /* Too big to fetch at all. */
    CHECK(!hr_files_read(&fs, "huge.csv", HR_FILE_SIZE_MAX + 1, 1000));
    CHECK_INT(fs.err, HR_FILES_ERR_TOO_BIG);
    CHECK_INT(r.sent, 0);

    /* One at a time. */
    CHECK(hr_files_read(&fs, "t.csv", 10, 1000));
    CHECK(!hr_files_read(&fs, "u.csv", 10, 1000));
    CHECK(!hr_files_list(&fs, ".csv", 1000));
}

static void test_a_link_that_cannot_send(void)
{
    TEST_CASE("a link that cannot send");
    rig_t r;
    hr_files_t fs;
    rig_init(&r, &fs);
    r.fail_send = true;
    CHECK(hr_files_read(&fs, "t.csv", 10, 1000));
    hr_files_tick(&fs, 1000, true);
    CHECK_INT(r.done, 1);
    CHECK_INT(r.done_err, HR_FILES_ERR_SEND);
}

static void test_cancel(void)
{
    TEST_CASE("cancel");
    rig_t r;
    hr_files_t fs;
    rig_init(&r, &fs);
    CHECK(hr_files_read(&fs, "t.csv", 4096, 1000));
    hr_files_tick(&fs, 1000, true);
    hr_files_cancel(&fs, 1100);
    CHECK_INT(r.done, 1);
    CHECK_INT(r.done_err, HR_FILES_ERR_CANCELLED);
    CHECK(!hr_files_busy(&fs));
    /* and a cancel with nothing running is harmless */
    hr_files_cancel(&fs, 1200);
    CHECK_INT(r.done, 1);
}

int main(void)
{
    test_arguments_we_will_send();
    test_arguments_we_refuse();
    test_a_verb_hiding_in_a_name();
    test_listing_entries();
    test_measuring_a_block_as_it_arrives();
    test_parsing_a_block();
    test_a_corrupt_block_is_reported_not_hidden();
    test_the_dryer_saying_it_has_no_such_file();
    test_a_block_survives_the_line_reassembler();
    test_a_block_nobody_can_hold_is_swallowed();
    test_an_oversize_encoded_frame_is_collected();
    test_a_listing_walks_to_the_end();
    test_an_answer_for_another_index_is_ignored();
    test_a_read_walks_the_blocks();
    test_a_bad_checksum_asks_again();
    test_a_silent_dryer_times_out();
    test_the_link_going_down_ends_it();
    test_a_full_consumer_pauses_the_transfer();
    test_no_such_file();
    test_what_we_will_not_start();
    test_a_link_that_cannot_send();
    test_cancel();
    return TEST_REPORT();
}
