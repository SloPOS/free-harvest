/*
 * hr_trend tests.
 *
 * The oscillation cases use the real pattern observed in an idle capture:
 * 63,62,63,62,63 while the machine was stationary. That is quantization, not
 * drift, and suppressing it is the whole point of the adaptive deadband.
 */
#include "hr_trend.h"
#include "test_util.h"



#define B HR_TREND_BUCKET_MS

/* Feed one reading per bucket, advancing the clock a full bucket each time. */
static void feed_per_bucket(hr_trend_t *tr, const int *temps, size_t n,
                            unsigned long *t)
{
    for (size_t i = 0; i < n; i++) {
        hr_trend_add(tr, *t, temps[i], 0, false);
        *t += B;
    }
    hr_trend_tick(tr, *t);
}

static void test_deadband_widens_as_it_gets_colder(void)
{
    /* The core claim: colder => wider band, because the true slope shrinks. */
    CHECK(hr_trend_deadband_cf(70) == 50);
    CHECK(hr_trend_deadband_cf(32) == 50);
    CHECK(hr_trend_deadband_cf(0) > hr_trend_deadband_cf(32));
    CHECK(hr_trend_deadband_cf(-20) > hr_trend_deadband_cf(0));
    /* Capped so it can never swallow a real trend. */
    CHECK(hr_trend_deadband_cf(-60) == 200);
    CHECK(hr_trend_deadband_cf(-20) == 200);

    /* Hysteresis grows too, and is likewise bounded. */
    CHECK(hr_trend_hysteresis(70) == 2);
    CHECK(hr_trend_hysteresis(-20) > hr_trend_hysteresis(70));
    CHECK(hr_trend_hysteresis(-100) == 6);
}

static void test_idle_plus_minus_one_is_suppressed(void)
{
    /* Exactly the captured pattern. The smoothed line must not move at all. */
    hr_trend_t tr;
    hr_trend_init(&tr);
    unsigned long t = 0;
    const int temps[] = {63, 62, 63, 62, 63, 62, 63, 62};
    feed_per_bucket(&tr, temps, 8, &t);

    CHECK(hr_trend_count(&tr) == 8);
    hr_trend_point_t p0, pN;
    CHECK(hr_trend_get(&tr, 0, &p0));
    CHECK(hr_trend_get(&tr, 7, &pN));
    /* Locked to the first reading throughout. */
    CHECK(p0.temp_smooth_cf == 6300);
    CHECK(pN.temp_smooth_cf == 6300);
    /* Raw is preserved unmodified so the UI can still show the real noise. */
    CHECK(pN.temp_raw_f == 62);
}

static void test_cold_oscillation_also_suppressed(void)
{
    /*
     * At -15F the band is wider, so a +/-1 flap is even more thoroughly
     * ignored. This is the regime reported as worst in practice.
     */
    hr_trend_t tr;
    hr_trend_init(&tr);
    unsigned long t = 0;
    const int temps[] = {-15, -14, -15, -14, -15, -14, -15, -14, -15, -14};
    feed_per_bucket(&tr, temps, 10, &t);

    hr_trend_point_t p;
    CHECK(hr_trend_get(&tr, 9, &p));
    CHECK(p.temp_smooth_cf == -1500);
}

static void test_real_trend_still_tracked(void)
{
    /*
     * The deadband must not blind us to genuine cooling. A steady monotonic
     * descent has to move the smoothed level.
     */
    hr_trend_t tr;
    hr_trend_init(&tr);
    unsigned long t = 0;
    const int temps[] = {60, 55, 50, 45, 40, 35, 30, 25, 20, 15, 10, 5, 0};
    feed_per_bucket(&tr, temps, 13, &t);

    hr_trend_point_t first, last;
    CHECK(hr_trend_get(&tr, 0, &first));
    CHECK(hr_trend_get(&tr, 12, &last));
    CHECK(first.temp_smooth_cf == 6000);
    /* Must have followed the descent, not stuck near the start. */
    CHECK(last.temp_smooth_cf < 1500);
}

static void test_sustained_small_change_eventually_accepted(void)
{
    /*
     * A 1F step that PERSISTS is real and must be adopted once hysteresis is
     * satisfied - otherwise slow late-stage cooling would be invisible.
     */
    hr_trend_t tr;
    hr_trend_init(&tr);
    unsigned long t = 0;
    const int temps[] = {40, 39, 39, 39, 39, 39, 39, 39};
    feed_per_bucket(&tr, temps, 8, &t);

    hr_trend_point_t p;
    CHECK(hr_trend_get(&tr, 7, &p));
    CHECK(p.temp_smooth_cf == 3900);
}

static void test_median_rejects_a_single_spike(void)
{
    /* Three samples in one bucket, one absurd: the median must discard it. */
    hr_trend_t tr;
    hr_trend_init(&tr);
    hr_trend_add(&tr, 0, 40, 0, false);
    hr_trend_add(&tr, 5000, 999, 0, false);   /* spike */
    hr_trend_add(&tr, 10000, 41, 0, false);
    hr_trend_tick(&tr, B);

    hr_trend_point_t p;
    CHECK(hr_trend_count(&tr) == 1);
    CHECK(hr_trend_get(&tr, 0, &p));
    CHECK(p.temp_raw_f == 41); /* not 999, not the mean */
}

static void test_gaps_become_empty_buckets(void)
{
    /*
     * A 76s gap was observed on real hardware. The series must stay on a true
     * 30s grid, so the gap has to materialise as empty buckets rather than
     * silently compressing time.
     */
    hr_trend_t tr;
    hr_trend_init(&tr);
    hr_trend_add(&tr, 0, 40, 0, false);
    hr_trend_add(&tr, 76000, 38, 0, false); /* 76s later */
    hr_trend_tick(&tr, 76000 + B);

    /* 0-30s, 30-60s, 60-90s => 3 buckets. */
    CHECK(hr_trend_count(&tr) == 3);
    hr_trend_point_t p0, p1;
    CHECK(hr_trend_get(&tr, 0, &p0));
    CHECK(hr_trend_get(&tr, 1, &p1));
    CHECK(p0.temp_raw_f == 40);
    CHECK(p1.temp_raw_f == HR_TREND_NO_TEMP);   /* gap marked, not invented */
    CHECK(p1.temp_smooth_cf == 4000);           /* line stays continuous */
}

static void test_invalid_pressure_is_not_recorded(void)
{
    /*
     * 10000 exactly means "pump off" and >10000 is the uncalibrated
     * at-atmosphere reading. Neither is a vacuum, so neither may enter the
     * series as one.
     */
    hr_trend_t tr;
    hr_trend_init(&tr);
    hr_trend_add(&tr, 0, 40, 10000, false);
    hr_trend_tick(&tr, B);
    hr_trend_add(&tr, B, 40, 441, true);
    hr_trend_tick(&tr, 2 * B);

    hr_trend_point_t p0, p1;
    CHECK(hr_trend_get(&tr, 0, &p0));
    CHECK(hr_trend_get(&tr, 1, &p1));
    CHECK(p0.pressure_raw == 0);
    CHECK(p1.pressure_raw == 441);
}

static void test_reset_clears_between_batches(void)
{
    /* One run's curve must never be fitted across another's. */
    hr_trend_t tr;
    hr_trend_init(&tr);
    unsigned long t = 0;
    const int temps[] = {60, 55, 50};
    feed_per_bucket(&tr, temps, 3, &t);
    CHECK(hr_trend_count(&tr) == 3);

    hr_trend_reset(&tr);
    CHECK(hr_trend_count(&tr) == 0);

    /* Smoothing state gone too: first reading of the new batch anchors it. */
    hr_trend_add(&tr, 0, 12, 0, false);
    hr_trend_tick(&tr, B);
    hr_trend_point_t p;
    CHECK(hr_trend_get(&tr, 0, &p));
    CHECK(p.temp_smooth_cf == 1200);
}

static void test_restore_rebuilds_the_series(void)
{
    /* A persisted run comes back verbatim - restore must not re-smooth or
       re-bucket, or replaying it would change the shape of history. */
    hr_trend_t tr;
    hr_trend_init(&tr);
    for (int i = 0; i < 5; i++) {
        hr_trend_point_t p;
        p.temp_raw_f = (int16_t)(20 - i);
        p.temp_smooth_cf = (int16_t)((20 - i) * 100);
        p.pressure_raw = (uint32_t)(500 + i);
        CHECK(hr_trend_restore_point(&tr, &p));
    }
    CHECK(hr_trend_count(&tr) == 5);
    hr_trend_point_t got;
    CHECK(hr_trend_get(&tr, 0, &got));
    CHECK(got.temp_raw_f == 20 && got.temp_smooth_cf == 2000);
    CHECK(hr_trend_get(&tr, 4, &got));
    CHECK(got.temp_raw_f == 16 && got.pressure_raw == 504);
}

static void test_outage_becomes_a_gap_not_a_line(void)
{
    /* The outage must be visible. Interpolating across it would assert data we
       never had. */
    hr_trend_t tr;
    hr_trend_init(&tr);
    hr_trend_point_t p = {.temp_raw_f = 10, .temp_smooth_cf = 1000,
                          .pressure_raw = 450};
    CHECK(hr_trend_restore_point(&tr, &p));
    hr_trend_restore_gap(&tr, 4);          /* 4 buckets = 2 minutes down */
    CHECK(hr_trend_count(&tr) == 5);

    hr_trend_point_t g;
    for (size_t i = 1; i < 5; i++) {
        CHECK(hr_trend_get(&tr, i, &g));
        CHECK(g.temp_raw_f == HR_TREND_NO_TEMP);
        CHECK(g.pressure_raw == 0);
    }
}

static void test_resume_continues_the_smoothed_line(void)
{
    /*
     * After a restore the smoothed level must carry over. Otherwise the first
     * reading after reboot re-anchors the line and the graph shows a step that
     * the machine never made.
     */
    hr_trend_t tr;
    hr_trend_init(&tr);
    hr_trend_point_t p = {.temp_raw_f = -12, .temp_smooth_cf = -1200,
                          .pressure_raw = 0};
    CHECK(hr_trend_restore_point(&tr, &p));
    hr_trend_restore_gap(&tr, 2);
    hr_trend_resume(&tr, 100000);

    /* One reading 1F away is inside the cold deadband, so the level holds. */
    hr_trend_add(&tr, 100000, -13, 0, false);
    hr_trend_tick(&tr, 100000 + B);
    hr_trend_point_t got;
    CHECK(hr_trend_get(&tr, hr_trend_count(&tr) - 1, &got));
    CHECK(got.temp_smooth_cf == -1200);
}

static void test_restore_stops_at_capacity(void)
{
    hr_trend_t tr;
    hr_trend_init(&tr);
    hr_trend_point_t p = {.temp_raw_f = 1, .temp_smooth_cf = 100,
                          .pressure_raw = 0};
    for (size_t i = 0; i < HR_TREND_CAPACITY; i++) {
        CHECK(hr_trend_restore_point(&tr, &p) == true);
    }
    CHECK(hr_trend_restore_point(&tr, &p) == false);
    hr_trend_restore_gap(&tr, 10);   /* must not overrun */
    CHECK(hr_trend_count(&tr) == HR_TREND_CAPACITY);
}

int main(void)
{
    test_deadband_widens_as_it_gets_colder();
    test_idle_plus_minus_one_is_suppressed();
    test_cold_oscillation_also_suppressed();
    test_real_trend_still_tracked();
    test_sustained_small_change_eventually_accepted();
    test_median_rejects_a_single_spike();
    test_gaps_become_empty_buckets();
    test_invalid_pressure_is_not_recorded();
    test_reset_clears_between_batches();
    test_restore_rebuilds_the_series();
    test_outage_becomes_a_gap_not_a_line();
    test_resume_continues_the_smoothed_line();
    test_restore_stops_at_capacity();
    return TEST_REPORT();
}
