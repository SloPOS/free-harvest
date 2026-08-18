#include "hr_trend.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Adaptive thresholds                                                 */
/* ------------------------------------------------------------------ */
/*
 * Both thresholds widen as temperature falls, for one physical reason: the
 * dryer reports whole degrees F, and cooling is exponential, so the true rate
 * of change shrinks as the chamber approaches its target. Once that rate drops
 * below 1 F per bucket the reading MUST alternate between adjacent integers -
 * there is no third value available to it. A fixed deadband that is adequate at
 * room temperature therefore lets the display oscillate exactly where a steady
 * reading matters most.
 *
 * Constants are physically motivated but empirical; tune against the recorded
 * full-cycle dataset rather than by intuition.
 */

int hr_trend_deadband_cf(int temp_f)
{
    const int base = 50; /* 0.50 F above freezing */
    if (temp_f >= 32) {
        return base;
    }
    /* Grow linearly with depth below freezing, reaching 2.00 F at -20 F. */
    int below = 32 - temp_f;            /* >0 */
    int extra = (below * 150) / 52;     /* 52 F span: +32 -> -20 */
    int d = base + extra;
    return d > 200 ? 200 : d;
}

int hr_trend_hysteresis(int temp_f)
{
    if (temp_f >= 32) {
        return 2;
    }
    int below = 32 - temp_f;
    int n = 2 + below / 15;
    return n > 6 ? 6 : n;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void sort_i16(int16_t *a, size_t n)
{
    /* Insertion sort: n <= HR_TREND_BUCKET_SAMPLES (8). */
    for (size_t i = 1; i < n; i++) {
        int16_t v = a[i];
        size_t j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
}

static void sort_u32(uint32_t *a, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        uint32_t v = a[i];
        size_t j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
}

static int16_t median_i16(int16_t *a, size_t n)
{
    sort_i16(a, n);
    return a[n / 2];
}

static uint32_t median_u32(uint32_t *a, size_t n)
{
    sort_u32(a, n);
    return a[n / 2];
}

/* ------------------------------------------------------------------ */
/* Smoothing                                                           */
/* ------------------------------------------------------------------ */
/*
 * Deadband plus DIRECTIONAL run-length hysteresis.
 *
 * The level moves only once the raw median has sat beyond the deadband for
 * `hysteresis` consecutive buckets moving in the SAME DIRECTION.
 *
 * Direction, not value: an earlier version required consecutive identical
 * readings, which silently froze the level during real cooling, because a
 * descending series never repeats a value. Quantization flapping alternates
 * direction and so keeps resetting the run; a genuine trend holds its direction
 * and is adopted quickly.
 *
 * When the level does move it jumps TO the current reading rather than easing
 * toward it. Confirmation has already established the reading is real, and
 * easing would add lag exactly where fidelity matters.
 *
 * NOTE: this smoothed series is for DISPLAY. The curve fit in hr_estimate reads
 * the raw medians, because least-squares already averages the noise and does so
 * without introducing lag.
 */
static int16_t smooth_step(hr_trend_t *tr, int16_t raw_f)
{
    if (!tr->have_level) {
        tr->have_level = true;
        tr->level_cf = (int32_t)raw_f * 100;
        tr->cand_dir = 0;
        tr->cand_runs = 0;
        return (int16_t)tr->level_cf;
    }

    int dead = hr_trend_deadband_cf((int)raw_f);
    int need = hr_trend_hysteresis((int)raw_f);
    int32_t raw_cf = (int32_t)raw_f * 100;
    int32_t delta = raw_cf - tr->level_cf;
    int32_t mag = (delta < 0) ? -delta : delta;

    if (mag <= dead) {
        /* Inside the band: hold, and abandon any run in progress. */
        tr->cand_runs = 0;
        return (int16_t)tr->level_cf;
    }

    int8_t dir = (delta > 0) ? (int8_t)1 : (int8_t)-1;
    if (tr->cand_runs > 0 && tr->cand_dir == dir) {
        tr->cand_runs++;
    } else {
        tr->cand_dir = dir;
        tr->cand_runs = 1;
    }

    if ((int)tr->cand_runs >= need) {
        tr->level_cf = raw_cf;
        tr->cand_runs = 0;
    }
    return (int16_t)tr->level_cf;
}

/* ------------------------------------------------------------------ */
/* Bucket lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void commit_bucket(hr_trend_t *tr)
{
    if (!tr->have_bucket) {
        return;
    }

    hr_trend_point_t p;
    if (tr->t_count > 0) {
        p.temp_raw_f = median_i16(tr->t_samples, tr->t_count);
        p.temp_smooth_cf = smooth_step(tr, p.temp_raw_f);
    } else {
        /*
         * No frames landed in this bucket (a 76s gap spans two). Record the
         * gap rather than inventing a reading, but carry the smoothed level
         * forward so the line stays continuous.
         */
        p.temp_raw_f = HR_TREND_NO_TEMP;
        p.temp_smooth_cf = tr->have_level ? (int16_t)tr->level_cf
                                          : HR_TREND_NO_TEMP;
    }
    p.pressure_raw = (tr->p_count > 0) ? median_u32(tr->p_samples, tr->p_count)
                                       : 0;

    if (tr->count < HR_TREND_CAPACITY) {
        tr->pts[tr->count++] = p;
    } else {
        /*
         * 30 hours exceeded. Keep the OLDEST data and stop appending rather
         * than shifting: the start of the cooling curve is what the fit needs,
         * and a run this long is already past the phase we estimate.
         */
        tr->overflowed = true;
    }

    tr->t_count = 0;
    tr->p_count = 0;
    tr->have_bucket = false;
}

void hr_trend_init(hr_trend_t *tr)
{
    if (tr == NULL) {
        return;
    }
    memset(tr, 0, sizeof(*tr));
}

void hr_trend_reset(hr_trend_t *tr)
{
    hr_trend_init(tr);
}

void hr_trend_tick(hr_trend_t *tr, unsigned long now_ms)
{
    if (tr == NULL || !tr->have_bucket) {
        return;
    }
    /*
     * Close every bucket the clock has passed. Gaps produce empty buckets so
     * the series stays on a true 30s grid - the graph's x-axis and the curve
     * fit both depend on that.
     */
    while (tr->have_bucket &&
           now_ms - tr->bucket_start_ms >= HR_TREND_BUCKET_MS) {
        unsigned long next = tr->bucket_start_ms + HR_TREND_BUCKET_MS;
        commit_bucket(tr);
        if (now_ms - next >= HR_TREND_BUCKET_MS) {
            /* Still behind: open an empty bucket to represent the gap. */
            tr->have_bucket = true;
            tr->bucket_start_ms = next;
        }
    }
}

void hr_trend_add(hr_trend_t *tr, unsigned long now_ms, int temp_f,
                  uint32_t pressure, bool pressure_valid)
{
    if (tr == NULL) {
        return;
    }

    if (!tr->have_bucket) {
        tr->have_bucket = true;
        tr->bucket_start_ms = now_ms;
    } else {
        hr_trend_tick(tr, now_ms);
        if (!tr->have_bucket) {
            tr->have_bucket = true;
            tr->bucket_start_ms = now_ms;
        }
    }

    if (tr->t_count < HR_TREND_BUCKET_SAMPLES) {
        tr->t_samples[tr->t_count++] = (int16_t)temp_f;
    }
    if (pressure_valid && pressure > 0 &&
        tr->p_count < HR_TREND_BUCKET_SAMPLES) {
        tr->p_samples[tr->p_count++] = pressure;
    }
}

size_t hr_trend_count(const hr_trend_t *tr)
{
    return (tr == NULL) ? 0 : tr->count;
}

bool hr_trend_get(const hr_trend_t *tr, size_t i, hr_trend_point_t *out)
{
    if (tr == NULL || out == NULL || i >= tr->count) {
        return false;
    }
    *out = tr->pts[i];
    return true;
}
