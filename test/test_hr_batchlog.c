#include "hr_batchlog.h"
#include "test_util.h"

#include <string.h>

static hr_batch_t fixture(void)
{
    hr_batch_t b;
    memset(&b, 0, sizeof(b));
    b.start_epoch = 1755820800u;
    b.duration_s = 93600;
    strcpy(b.name, "Strawberries");
    b.family = 4;
    int32_t p[8] = {4, 70, 140, 150, 160, 300, 7200, 300};
    memcpy(b.params, p, sizeof(p));
    b.nparams = 8;
    b.extra_dry_s = 7200;
    b.min_temp_f = -34;
    b.max_temp_f = 71;
    b.best_vacuum_um = 288;
    b.pulldown_s = 1840;
    b.outcome = HR_OUTCOME_COMPLETE;
    return b;
}

static void test_round_trip(void)
{
    hr_batch_t in = fixture(), out;
    char line[HR_BATCH_LINE_MAX];
    CHECK(hr_batch_encode(&in, line, sizeof(line)) > 0);
    CHECK(hr_batch_decode(line, &out));

    CHECK_INT((int)out.start_epoch, (int)in.start_epoch);
    CHECK_INT((int)out.duration_s, (int)in.duration_s);
    CHECK_STR(out.name, in.name);
    CHECK_INT(out.family, in.family);
    CHECK_INT(out.nparams, in.nparams);
    for (int i = 0; i < in.nparams; i++) {
        CHECK_INT((int)out.params[i], (int)in.params[i]);
    }
    CHECK_INT((int)out.extra_dry_s, (int)in.extra_dry_s);
    CHECK_INT(out.min_temp_f, in.min_temp_f);
    CHECK_INT(out.max_temp_f, in.max_temp_f);
    CHECK_INT((int)out.best_vacuum_um, (int)in.best_vacuum_um);
    CHECK_INT((int)out.pulldown_s, (int)in.pulldown_s);
    CHECK_INT(out.outcome, in.outcome);
}

static void test_torn_lines_are_rejected(void)
{
    hr_batch_t in = fixture(), out;
    char line[HR_BATCH_LINE_MAX];
    size_t n = hr_batch_encode(&in, line, sizeof(line));
    CHECK(n > 0);

    /*
     * A power cut mid-append leaves a partial line. Every truncation of a
     * valid record must fail, because a short line that still parses is a
     * WRONG batch rather than an obvious failure - which is the entire reason
     * the checksum is there.
     */
    for (size_t cut = 1; cut < n; cut++) {
        char torn[HR_BATCH_LINE_MAX];
        memcpy(torn, line, cut);
        torn[cut] = '\0';
        CHECK(!hr_batch_decode(torn, &out));
    }

    /* A flipped byte in the body must fail too. */
    char bad[HR_BATCH_LINE_MAX];
    strcpy(bad, line);
    bad[5] = (bad[5] == '9') ? '8' : '9';
    CHECK(!hr_batch_decode(bad, &out));

    CHECK(!hr_batch_decode("", &out));
    CHECK(!hr_batch_decode("garbage", &out));
    CHECK(!hr_batch_decode(NULL, &out));
    CHECK(!hr_batch_decode(line, NULL));
}

static void test_name_cannot_break_the_record(void)
{
    /* A comma in a name would shift every field after it, and the record
     * would still parse - as a different batch. */
    hr_batch_t in = fixture(), out;
    strcpy(in.name, "Straw,berry\nfoo");
    char line[HR_BATCH_LINE_MAX];
    CHECK(hr_batch_encode(&in, line, sizeof(line)) > 0);
    CHECK(hr_batch_decode(line, &out));
    CHECK(strchr(out.name, ',') == NULL);
    CHECK(strchr(out.name, '\n') == NULL);
    CHECK_INT((int)out.duration_s, (int)in.duration_s);   /* not shifted */
}

static void test_short_buffer_writes_nothing(void)
{
    hr_batch_t in = fixture();
    char tiny[24];
    CHECK_INT((int)hr_batch_encode(&in, tiny, sizeof(tiny)), 0);
    CHECK_INT((int)tiny[0], 0);
    CHECK_INT((int)hr_batch_encode(&in, NULL, 100), 0);
    CHECK_INT((int)hr_batch_encode(NULL, tiny, sizeof(tiny)), 0);
}

/* Screens, for readability below. */
#define IDLE 1
#define START 2
#define FREEZE 4
#define DRY 5
#define FINAL 6
#define DONE 7
#define PREP 17

static void test_a_whole_run(void)
{
    hr_batch_tracker_t t;
    hr_batch_t rec;
    hr_batch_tracker_reset(&t);

    /* Idle, with the PREVIOUS batch's elapsed still showing. This is the trap
     * the UI comment warns about: elapsed > 0 does not mean running. */
    CHECK_INT(hr_batch_observe(&t, IDLE, 93600, 69, 0, 100, &rec),
              HR_BATCH_NOTHING);
    CHECK(!t.active);

    CHECK_INT(hr_batch_observe(&t, PREP, 0, 68, 0, 200, &rec),
              HR_BATCH_STARTED);
    CHECK(t.active);

    hr_batch_observe(&t, START,  600, 40, 0, 300, &rec);
    hr_batch_observe(&t, FREEZE, 3600, -20, 0, 400, &rec);
    hr_batch_observe(&t, FREEZE, 7200, -34, 0, 500, &rec);   /* coldest */
    hr_batch_observe(&t, DRY,   10800, -30, 1400, 600, &rec);
    hr_batch_observe(&t, DRY,   12640, -10, 480, 700, &rec); /* pulled down */
    hr_batch_observe(&t, DRY,   40000, 20, 288, 800, &rec);  /* deepest */
    hr_batch_observe(&t, FINAL, 80000, 41, 400, 900, &rec);

    hr_batch_set_extra_dry(&t, 7200);
    CHECK_INT(hr_batch_observe(&t, DONE, 93600, 69, 0, 1000, &rec),
              HR_BATCH_FINISHED);

    CHECK_INT(rec.outcome, HR_OUTCOME_COMPLETE);
    CHECK_INT((int)rec.duration_s, 93600);
    CHECK_INT(rec.min_temp_f, -34);          /* extreme, not last */
    CHECK_INT(rec.max_temp_f, 69);
    CHECK_INT((int)rec.best_vacuum_um, 288); /* deepest, not last */
    CHECK_INT((int)rec.pulldown_s, 12640 - 10800);
    CHECK_INT((int)rec.extra_dry_s, 7200);
    CHECK(!t.active);
}

static void test_ended_early(void)
{
    hr_batch_tracker_t t;
    hr_batch_t rec;
    hr_batch_tracker_reset(&t);

    hr_batch_observe(&t, PREP, 0, 68, 0, 100, &rec);
    hr_batch_observe(&t, FREEZE, 3600, -10, 0, 200, &rec);
    /* Straight back to idle without passing Complete: somebody pressed End. */
    CHECK_INT(hr_batch_observe(&t, IDLE, 3700, 20, 0, 300, &rec),
              HR_BATCH_FINISHED);
    CHECK_INT(rec.outcome, HR_OUTCOME_ENDED_EARLY);
}

static void test_elapsed_going_backwards_interrupts(void)
{
    hr_batch_tracker_t t;
    hr_batch_t rec;
    hr_batch_tracker_reset(&t);

    hr_batch_observe(&t, PREP, 0, 68, 0, 100, &rec);
    hr_batch_observe(&t, FREEZE, 7200, -20, 0, 200, &rec);

    /* The dryer restarted a run while we were not looking. The open batch
     * never got an ending, and must not be silently folded into the new one. */
    CHECK_INT(hr_batch_observe(&t, FREEZE, 60, -5, 0, 300, &rec),
              HR_BATCH_FINISHED);
    CHECK_INT(rec.outcome, HR_OUTCOME_INTERRUPTED);
    CHECK_INT((int)rec.duration_s, 7200);
}

static void test_abandon(void)
{
    hr_batch_tracker_t t;
    hr_batch_t rec;
    hr_batch_tracker_reset(&t);

    CHECK(!hr_batch_abandon(&t, &rec));      /* nothing open */

    hr_batch_observe(&t, DRY, 5000, 10, 700, 100, &rec);
    CHECK(t.active);
    CHECK(hr_batch_abandon(&t, &rec));
    CHECK_INT(rec.outcome, HR_OUTCOME_INTERRUPTED);
    CHECK(!t.active);
    CHECK(!hr_batch_abandon(&t, &rec));      /* only once */
}

static void test_unknown_date_stays_unknown(void)
{
    /* Before the browser has ever set the clock the epoch is 0. It must stay
     * 0 through a round trip rather than becoming a real-looking 1970 date. */
    hr_batch_tracker_t t;
    hr_batch_t rec, back;
    hr_batch_tracker_reset(&t);
    hr_batch_observe(&t, PREP, 0, 68, 0, 0, &rec);
    hr_batch_observe(&t, DONE, 100, 69, 0, 0, &rec);
    CHECK_INT((int)rec.start_epoch, 0);

    char line[HR_BATCH_LINE_MAX];
    CHECK(hr_batch_encode(&rec, line, sizeof(line)) > 0);
    CHECK(hr_batch_decode(line, &back));
    CHECK_INT((int)back.start_epoch, 0);
}

static void test_no_vacuum_reported(void)
{
    /* A run where the dryer never reports a vacuum must not record 0 as if it
     * were a spectacularly deep one. */
    hr_batch_tracker_t t;
    hr_batch_t rec;
    hr_batch_tracker_reset(&t);
    hr_batch_observe(&t, PREP, 0, 68, 0, 100, &rec);
    hr_batch_observe(&t, DRY, 3600, 10, 0, 200, &rec);
    hr_batch_observe(&t, DONE, 7200, 60, 0, 300, &rec);
    CHECK_INT((int)rec.best_vacuum_um, 0);
    CHECK_INT((int)rec.pulldown_s, 0);
}

int main(void)
{
    test_round_trip();
    test_torn_lines_are_rejected();
    test_name_cannot_break_the_record();
    test_short_buffer_writes_nothing();
    test_a_whole_run();
    test_ended_early();
    test_elapsed_going_backwards_interrupts();
    test_abandon();
    test_unknown_date_stays_unknown();
    test_no_vacuum_reported();
    return TEST_REPORT();
}
