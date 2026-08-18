/*
 * hr_trend - fixed-interval temperature/pressure history with adaptive
 * smoothing.
 *
 * Portable: no ESP-IDF dependencies, so it is unit-testable on a host PC.
 *
 * WHY THIS EXISTS
 *
 * Two problems make the raw STAT stream unusable as a graph source.
 *
 * 1. Frames arrive irregularly - about every 15s when idle, with gaps up to 76s
 *    observed mid-run - so they cannot feed a fixed-interval chart directly.
 *    Samples are therefore bucketed into fixed 30s slots.
 *
 * 2. The dryer reports WHOLE degrees Fahrenheit. Whenever the true rate of
 *    change falls below 1 F per bucket the reading has no choice but to
 *    alternate between adjacent integers. That gets worse as the chamber
 *    approaches its target, which is exactly where a stable reading matters
 *    most. So the smoothing deadband WIDENS as temperature falls.
 *
 * Each bucket keeps the MEDIAN of the samples in it, not the mean: a median
 * rejects a single outlying sample outright rather than averaging it in.
 *
 * Both the raw median and the smoothed value are retained so the UI can draw a
 * smooth line over a faint raw band - honest about the underlying noise, and
 * necessary for tuning the constants below.
 */
#ifndef HR_TREND_H
#define HR_TREND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bucket width. 30s per the design; nothing below assumes this exact value. */
#define HR_TREND_BUCKET_MS 30000UL

/*
 * Ring capacity in buckets. 3600 * 30s = 30 hours, comfortably longer than the
 * 22h full cycle we have measured, so a complete run is retained without
 * rolling off the start - which matters because the curve fit needs the early
 * part of the cooling curve.
 *
 * 3600 * 8 bytes = ~28.8 KB.
 */
#define HR_TREND_CAPACITY 3600

/* Samples held per bucket while it fills, for the median. */
#define HR_TREND_BUCKET_SAMPLES 8

/* Sentinel for "no temperature reading in this bucket". */
#define HR_TREND_NO_TEMP INT16_MIN

typedef struct {
    int16_t temp_raw_f;      /* bucket median, whole degrees F */
    int16_t temp_smooth_cf;  /* smoothed, HUNDREDTHS of a degree F */
    uint32_t pressure_raw;   /* bucket median, microns; 0 = no valid reading */
} hr_trend_point_t;

typedef struct {
    /* --- accumulating bucket --- */
    bool have_bucket;
    unsigned long bucket_start_ms;
    int16_t t_samples[HR_TREND_BUCKET_SAMPLES];
    uint8_t t_count;
    uint32_t p_samples[HR_TREND_BUCKET_SAMPLES];
    uint8_t p_count;

    /* --- smoothing state --- */
    bool have_level;
    int32_t level_cf;            /* current accepted level, hundredths F */
    int8_t cand_dir;             /* direction being confirmed: -1 or +1 */
    uint8_t cand_runs;           /* consecutive buckets in that direction */

    /* --- committed history --- */
    hr_trend_point_t pts[HR_TREND_CAPACITY];
    size_t count;
    bool overflowed;
} hr_trend_t;

void hr_trend_init(hr_trend_t *tr);

/*
 * Discard all history and smoothing state. Call when a new batch starts, so one
 * run's curve is never fitted across another's.
 */
void hr_trend_reset(hr_trend_t *tr);

/*
 * Feed one reading. `temp_f` is whole degrees F. `pressure` is microns and is
 * ignored when `pressure_valid` is false (pump off, or the uncalibrated
 * at-atmosphere reading). Buckets are closed automatically as time advances.
 */
void hr_trend_add(hr_trend_t *tr, unsigned long now_ms, int temp_f,
                  uint32_t pressure, bool pressure_valid);

/*
 * Close the open bucket if `now_ms` has moved past it. Lets history advance
 * when frames stop arriving, and must be called before reading the series so
 * the final partial bucket is committed.
 */
void hr_trend_tick(hr_trend_t *tr, unsigned long now_ms);

size_t hr_trend_count(const hr_trend_t *tr);

/* Copy point `i` (0 = oldest). Returns false if `i` is out of range. */
bool hr_trend_get(const hr_trend_t *tr, size_t i, hr_trend_point_t *out);

/*
 * The smoothing deadband at a given temperature, in hundredths of a degree F.
 * Exposed for testing: this is the core of the anti-oscillation behaviour.
 * 0.5 F above freezing, widening toward 2.0 F at -20 F and below.
 */
int hr_trend_deadband_cf(int temp_f);

/*
 * How many consecutive buckets must agree before the smoothed level moves.
 * Also grows as temperature falls, for the same reason as the deadband.
 */
int hr_trend_hysteresis(int temp_f);

/* ------------------------------------------------------------------ */
/* Restore after a power loss                                          */
/* ------------------------------------------------------------------ */
/*
 * The adapter is powered from the dryer, so a brownout or a reflash wipes this
 * RAM series. The dryer, however, keeps counting: its batch-elapsed value runs
 * whether or not we are alive. So the outage can be measured exactly rather
 * than guessed - persist the series with the elapsed value of its last point,
 * and on the next boot the difference IS the gap.
 */

/*
 * Append a previously persisted point verbatim, bypassing bucketing and
 * smoothing. Used only when restoring; `count` grows by one. Returns false if
 * the ring is full.
 */
bool hr_trend_restore_point(hr_trend_t *tr, const hr_trend_point_t *p);

/*
 * Append `n` empty buckets to represent time the adapter was not running.
 * Drawn as a break in the line - never interpolated, because we genuinely do
 * not know what happened during the outage.
 */
void hr_trend_restore_gap(hr_trend_t *tr, size_t n);

/*
 * Resume live bucketing after a restore. Anchors the next bucket to `now_ms`
 * and seeds the smoothing level from the last restored point, so the line
 * continues from where it left off instead of re-anchoring to a fresh value.
 */
void hr_trend_resume(hr_trend_t *tr, unsigned long now_ms);

#ifdef __cplusplus
}
#endif

#endif /* HR_TREND_H */
