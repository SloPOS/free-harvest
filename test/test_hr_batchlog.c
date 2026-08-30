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
    CHECK_INT(hr_batch_observe(&t, IDLE, 93600, 69, 0, "Auto", 100, &rec),
              HR_BATCH_NOTHING);
    CHECK(!t.active);

    CHECK_INT(hr_batch_observe(&t, PREP, 0, 68, 0, "Auto", 200, &rec),
              HR_BATCH_STARTED);
    CHECK(t.active);

    hr_batch_observe(&t, START,  600, 40, 0, "Auto", 300, &rec);
    hr_batch_observe(&t, FREEZE, 3600, -20, 0, "Auto", 400, &rec);
    hr_batch_observe(&t, FREEZE, 7200, -34, 0, "Auto", 500, &rec);   /* coldest */
    hr_batch_observe(&t, DRY,   10800, -30, 1400, "Auto", 600, &rec);
    hr_batch_observe(&t, DRY,   12640, -10, 480, "Auto", 700, &rec); /* pulled down */
    hr_batch_observe(&t, DRY,   40000, 20, 288, "Auto", 800, &rec);  /* deepest */
    hr_batch_observe(&t, FINAL, 80000, 41, 400, "Auto", 900, &rec);

    hr_batch_set_extra_dry(&t, 7200);
    CHECK_INT(hr_batch_observe(&t, DONE, 93600, 69, 0, "Auto", 1000, &rec),
              HR_BATCH_FINISHED);

    CHECK_INT(rec.outcome, HR_OUTCOME_COMPLETE);
    CHECK_INT((int)rec.duration_s, 93600);
    CHECK_INT(rec.min_temp_f, -34);          /* extreme, not last */
    CHECK_INT(rec.max_temp_f, 69);
    CHECK_INT((int)rec.best_vacuum_um, 288); /* deepest, not last */
    CHECK_INT((int)rec.pulldown_s, 12640 - 10800);
    CHECK_INT((int)rec.extra_dry_s, 7200);
    /* The run must be NAMED. An unnamed record answers none of the questions
     * the logbook exists for, and the first device run produced exactly that
     * because nothing was feeding the mode through. */
    CHECK_STR(rec.name, "Auto");
    CHECK(!t.active);
}

static void test_mode_names_the_run(void)
{
    hr_batch_tracker_t t;
    hr_batch_t rec;

    /* A mode that only appears after the run starts must still name it - the
     * field is not reliably populated on the first frame. */
    hr_batch_tracker_reset(&t);
    hr_batch_observe(&t, PREP, 0, 68, 0, "", 100, &rec);
    hr_batch_observe(&t, FREEZE, 600, 10, 0, "CANDY", 200, &rec);
    hr_batch_observe(&t, DONE, 900, 60, 0, "CANDY", 300, &rec);
    CHECK_STR(rec.name, "CANDY");

    /* A missing mode throughout leaves it empty rather than inventing one. */
    hr_batch_tracker_reset(&t);
    hr_batch_observe(&t, PREP, 0, 68, 0, NULL, 100, &rec);
    hr_batch_observe(&t, DONE, 900, 60, 0, NULL, 200, &rec);
    CHECK_STR(rec.name, "");

    /* A mode carrying a comma must not shift the record's fields. */
    hr_batch_tracker_reset(&t);
    hr_batch_observe(&t, PREP, 0, 68, 0, "od,d", 100, &rec);
    hr_batch_observe(&t, DONE, 900, 60, 0, "od,d", 200, &rec);
    CHECK(strchr(rec.name, ',') == NULL);
}

static void test_ended_early(void)
{
    hr_batch_tracker_t t;
    hr_batch_t rec;
    hr_batch_tracker_reset(&t);

    hr_batch_observe(&t, PREP, 0, 68, 0, "Auto", 100, &rec);
    hr_batch_observe(&t, FREEZE, 3600, -10, 0, "Auto", 200, &rec);
    /* Straight back to idle without passing Complete: somebody pressed End. */
    CHECK_INT(hr_batch_observe(&t, IDLE, 3700, 20, 0, "Auto", 300, &rec),
              HR_BATCH_FINISHED);
    CHECK_INT(rec.outcome, HR_OUTCOME_ENDED_EARLY);
}

static void test_elapsed_going_backwards_interrupts(void)
{
    hr_batch_tracker_t t;
    hr_batch_t rec;
    hr_batch_tracker_reset(&t);

    hr_batch_observe(&t, PREP, 0, 68, 0, "Auto", 100, &rec);
    hr_batch_observe(&t, FREEZE, 7200, -20, 0, "Auto", 200, &rec);

    /* The dryer restarted a run while we were not looking. The open batch
     * never got an ending, and must not be silently folded into the new one. */
    CHECK_INT(hr_batch_observe(&t, FREEZE, 60, -5, 0, "Auto", 300, &rec),
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

    hr_batch_observe(&t, DRY, 5000, 10, 700, "Auto", 100, &rec);
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
    hr_batch_observe(&t, PREP, 0, 68, 0, "Auto", 0, &rec);
    hr_batch_observe(&t, DONE, 100, 69, 0, "Auto", 0, &rec);
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
    hr_batch_observe(&t, PREP, 0, 68, 0, "Auto", 100, &rec);
    hr_batch_observe(&t, DRY, 3600, 10, 0, "Auto", 200, &rec);
    hr_batch_observe(&t, DONE, 7200, 60, 0, "Auto", 300, &rec);
    CHECK_INT((int)rec.best_vacuum_um, 0);
    CHECK_INT((int)rec.pulldown_s, 0);
}

/*
 * The dryer runs Preparing and Starting on their own countdown, then restarts
 * the counter at zero when the batch clock proper begins. Taken from a real
 * run: 0..892 preparing, 900..1834 starting, then 1 at Freezing.
 *
 * That restart used to be read as "a new batch started", which closed a
 * 1834-second interrupted record right as freezing began and left the run that
 * followed as a second batch nobody was watching the end of. The logbook
 * finished a 28-hour run with no entry for it.
 */
static void test_prep_countdown_is_not_a_new_run(void)
{
    hr_batch_tracker_t t;
    hr_batch_t rec;
    hr_batch_tracker_reset(&t);

    CHECK_INT(hr_batch_observe(&t, PREP, 0, 74, 0, "Auto", 100, &rec),
              HR_BATCH_STARTED);
    CHECK_INT(hr_batch_observe(&t, PREP, 892, 63, 0, "Auto", 100, &rec),
              HR_BATCH_NOTHING);
    CHECK_INT(hr_batch_observe(&t, START, 900, 63, 0, "Auto", 100, &rec),
              HR_BATCH_NOTHING);
    CHECK_INT(hr_batch_observe(&t, START, 1834, 59, 0, "Auto", 100, &rec),
              HR_BATCH_NOTHING);

    /* The restart. Same run, so nothing is emitted and nothing is closed. */
    CHECK_INT(hr_batch_observe(&t, FREEZE, 1, 59, 0, "Auto", 100, &rec),
              HR_BATCH_NOTHING);
    CHECK(t.active);

    CHECK_INT(hr_batch_observe(&t, FREEZE, 31818, -16, 600, "Auto", 100, &rec),
              HR_BATCH_NOTHING);

    /* Duration counts from the rebased origin, not from the counter's value. */
    CHECK_INT((int)t.cur.duration_s, 31817);

    CHECK_INT(hr_batch_observe(&t, DONE, 31900, 60, 600, "Auto", 100, &rec),
              HR_BATCH_FINISHED);
    CHECK_INT(rec.outcome, HR_OUTCOME_COMPLETE);
}

/* Each phase is timed on the dryer's own counter, for estimating the next run. */
static void test_phase_durations_are_recorded(void)
{
    hr_batch_tracker_t t;
    hr_batch_t rec;
    hr_batch_tracker_reset(&t);

    hr_batch_observe(&t, FREEZE, 1, 59, 0, "Auto", 100, &rec);
    hr_batch_observe(&t, FREEZE, 31818, -16, 0, "Auto", 100, &rec);
    hr_batch_observe(&t, DRY, 31826, -16, 440, "Auto", 100, &rec);
    hr_batch_observe(&t, DRY, 64065, 110, 440, "Auto", 100, &rec);
    hr_batch_observe(&t, FINAL, 64069, 110, 499, "Auto", 100, &rec);
    hr_batch_observe(&t, FINAL, 102224, 119, 284, "Auto", 100, &rec);
    CHECK_INT(hr_batch_observe(&t, DONE, 102230, 119, 284, "Auto", 100, &rec),
              HR_BATCH_FINISHED);

    CHECK_INT((int)rec.freeze_s, 31817);
    CHECK_INT((int)rec.dry_s, 32239);
    CHECK_INT((int)rec.final_s, 38155);

    /* And the record survives a trip through the file format. */
    char line[HR_BATCH_LINE_MAX];
    hr_batch_t back;
    CHECK(hr_batch_encode(&rec, line, sizeof(line)) > 0);
    CHECK(hr_batch_decode(line, &back));
    CHECK_INT((int)back.freeze_s, 31817);
    CHECK_INT((int)back.dry_s, 32239);
    CHECK_INT((int)back.final_s, 38155);
}

static void test_estimate_seeds_then_learns(void)
{
    hr_batch_estimate_t e;

    /* Nothing recorded yet: the seed, and it says so. */
    CHECK(hr_batch_estimate(NULL, 0, &e));
    CHECK_INT(e.samples, 0);
    CHECK_INT((int)e.freeze_s, HR_SEED_FREEZE_S);
    CHECK_INT((int)e.total_s,
              HR_SEED_FREEZE_S + HR_SEED_DRY_S + HR_SEED_FINAL_S);

    /* Three completed runs: the median of each phase, not the mean, so the
     * long outlier does not drag the estimate up. */
    hr_batch_t h[3];
    memset(h, 0, sizeof(h));
    for (int i = 0; i < 3; i++) {
        h[i].outcome = HR_OUTCOME_COMPLETE;
    }
    h[0].freeze_s = 30000; h[0].dry_s = 30000; h[0].final_s = 30000;
    h[1].freeze_s = 32000; h[1].dry_s = 32000; h[1].final_s = 32000;
    h[2].freeze_s = 90000; h[2].dry_s = 90000; h[2].final_s = 90000;

    CHECK(hr_batch_estimate(h, 3, &e));
    CHECK_INT(e.samples, 3);
    CHECK_INT((int)e.freeze_s, 32000);
    CHECK_INT((int)e.dry_s, 32000);
    CHECK_INT((int)e.final_s, 32000);
    CHECK_INT((int)e.total_s, 96000);
}

/*
 * A run that was ended early or interrupted says nothing about how long a full
 * one takes, and a version-1 record carries no phase times at all. Counting
 * either would pull every estimate toward zero while still looking plausible.
 */
static void test_estimate_ignores_unfinished_runs(void)
{
    hr_batch_estimate_t e;
    hr_batch_t h[3];
    memset(h, 0, sizeof(h));

    h[0].outcome = HR_OUTCOME_ENDED_EARLY;
    h[0].freeze_s = 100; h[0].dry_s = 100; h[0].final_s = 100;

    h[1].outcome = HR_OUTCOME_COMPLETE;      /* v1 record: no phase times */
    h[1].freeze_s = 0; h[1].dry_s = 0; h[1].final_s = 0;

    h[2].outcome = HR_OUTCOME_COMPLETE;
    h[2].freeze_s = 31817; h[2].dry_s = 32239; h[2].final_s = 38155;

    CHECK(hr_batch_estimate(h, 3, &e));
    CHECK_INT(e.samples, 1);
    CHECK_INT((int)e.freeze_s, 31817);
    CHECK_INT((int)e.total_s, 31817 + 32239 + 38155);
}

int main(void)
{
    test_round_trip();
    test_torn_lines_are_rejected();
    test_name_cannot_break_the_record();
    test_short_buffer_writes_nothing();
    test_a_whole_run();
    test_mode_names_the_run();
    test_ended_early();
    test_elapsed_going_backwards_interrupts();
    test_abandon();
    test_unknown_date_stays_unknown();
    test_no_vacuum_reported();
    test_prep_countdown_is_not_a_new_run();
    test_phase_durations_are_recorded();
    test_estimate_seeds_then_learns();
    test_estimate_ignores_unfinished_runs();
    return TEST_REPORT();
}
