/*
 * hr_batchlog - one record per freeze-drying run, and the tracker that builds it.
 *
 * Three jobs, from the three reasons a logbook is wanted: what was made and
 * when, how the recipe performed so it can be tuned, and how the machine itself
 * is holding up over months.
 *
 * Records are CSV lines with a trailing checksum. Nothing here touches a
 * filesystem or a network - the device half lives in main/hr_batchstore.c - so
 * all of this is testable on a host.
 */
#ifndef HR_BATCHLOG_H
#define HR_BATCHLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HR_BATCH_NAME_MAX 24
#define HR_BATCH_PARAMS   10
#define HR_BATCH_LINE_MAX 224

typedef enum {
    HR_OUTCOME_RUNNING = 0,
    HR_OUTCOME_COMPLETE,      /* reached the Complete screen                */
    HR_OUTCOME_ENDED_EARLY,   /* returned to idle from a running phase      */
    HR_OUTCOME_INTERRUPTED,   /* power or link lost mid-run                 */
} hr_outcome_t;

typedef struct {
    /*
     * Wall-clock start, or 0 when the adapter had never been told the time.
     * Zero means "date unknown" and is rendered that way - it is not turned
     * into 1970, which reads as a real date and is not one.
     */
    uint32_t start_epoch;
    uint32_t duration_s;

    char     name[HR_BATCH_NAME_MAX];
    uint8_t  family;                    /* 4 candy, 5 custom, 0 auto/none   */
    int32_t  params[HR_BATCH_PARAMS];
    uint8_t  nparams;

    int32_t  extra_dry_s;               /* drying added during the run      */

    /* Machine health. Extremes, not last values. */
    int16_t  min_temp_f;
    int16_t  max_temp_f;
    int32_t  best_vacuum_um;            /* deepest (lowest) reached         */
    uint32_t pulldown_s;                /* time from drying start to 500um  */

    uint8_t  outcome;                   /* hr_outcome_t                     */
} hr_batch_t;

/* ---- record encoding ---------------------------------------------------- */

/*
 * Render as one CSV line, checksum included, WITHOUT a trailing newline.
 * Returns bytes written, or 0 if it would not fit - a truncated record must
 * never reach the file, because a short line that still parses is a wrong
 * batch rather than an obvious failure.
 */
size_t hr_batch_encode(const hr_batch_t *b, char *out, size_t cap);

/*
 * Parse one line. Returns false when the checksum disagrees, the field count
 * is wrong, or the line is malformed - all of which are expected after a torn
 * write and none of which should yield a plausible-looking record.
 */
bool hr_batch_decode(const char *line, hr_batch_t *out);

/* ---- the tracker -------------------------------------------------------- */

/*
 * Watches telemetry and decides where one run ends and the next begins.
 *
 * Two signals, because neither alone is sufficient:
 *
 *   - phase, because the dryer keeps the PREVIOUS batch's elapsed value while
 *     idle, so a non-zero elapsed does not mean a run is under way;
 *   - elapsed going backwards, which is the rule main.c already uses to decide
 *     that a stored trend belongs to a finished run. Reusing it matters: a
 *     second, disagreeing definition of "a batch" in one firmware is a
 *     reliable source of bugs.
 */
typedef struct {
    bool       active;
    hr_batch_t cur;

    int        last_phase;
    int32_t    last_elapsed;
    bool       have_last;

    /* Pull-down timing: when drying began, and whether 500um was reached. */
    int32_t    dry_start_elapsed;
    bool       pulldown_done;

    /* Set from the extra-dry tracker at commit time. */
    uint32_t   dry_extensions;
} hr_batch_tracker_t;

typedef enum {
    HR_BATCH_NOTHING = 0,
    HR_BATCH_STARTED,
    HR_BATCH_FINISHED,     /* `out` holds the completed record              */
} hr_batch_event_t;

void hr_batch_tracker_reset(hr_batch_tracker_t *t);

/*
 * Feed one telemetry sample.
 *
 * phase is the STAT screen type; elapsed_s, temp_f and vacuum_um come from the
 * same frame. vacuum_um may be <= 0 when the dryer is not reporting one.
 *
 * Returns HR_BATCH_FINISHED and fills `out` on the sample that ends a run.
 */
hr_batch_event_t hr_batch_observe(hr_batch_tracker_t *t, int phase,
                                  int32_t elapsed_s, int32_t temp_f,
                                  int32_t vacuum_um, uint32_t now_epoch,
                                  hr_batch_t *out);

/*
 * Close an open run without a telemetry sample - used on boot when a batch was
 * left open by a power cut. Returns false when nothing was open.
 */
bool hr_batch_abandon(hr_batch_tracker_t *t, hr_batch_t *out);

/* Seconds of drying added during this run, for the record and the UI. */
void hr_batch_set_extra_dry(hr_batch_tracker_t *t, int32_t seconds);

const char *hr_outcome_str(uint8_t outcome);

#ifdef __cplusplus
}
#endif

#endif /* HR_BATCHLOG_H */
