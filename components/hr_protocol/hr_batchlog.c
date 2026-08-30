#include "hr_batchlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Screens. Same numbering the control table uses. */
#define SCR_IDLE      1
#define SCR_STARTING  2
#define SCR_FREEZING  4
#define SCR_DRYING    5
#define SCR_FINAL_DRY 6
#define SCR_COMPLETE  7
#define SCR_PREPARING 17

/* Vacuum considered "pulled down". Inferred from captures, not measured. */
#define PULLDOWN_UM   500

#define CSV_VERSION   2

static bool phase_is_running(int p)
{
    return p == SCR_PREPARING || p == SCR_STARTING || p == SCR_FREEZING ||
           p == SCR_DRYING || p == SCR_FINAL_DRY;
}

bool hr_phase_is_running(int phase)
{
    return phase_is_running(phase);
}

/*
 * Preparing and Starting run on a separate countdown that reaches its own end
 * and then restarts at zero when the batch clock proper begins. Observed on a
 * real run: 0..892 preparing, 900..1834 starting, then 1 at Freezing.
 */
static bool phase_is_prerun(int p)
{
    return p == SCR_PREPARING || p == SCR_STARTING;
}

/*
 * Checksum over the line up to (not including) the final field.
 *
 * A plain additive sum folded to 16 bits. Not cryptographic and not meant to
 * be: the thing being caught is a write torn by a power cut, where the tail of
 * a line is missing or half-written, and any checksum at all catches that. What
 * matters more is that a failing line is SKIPPED rather than parsed, which is
 * the decode contract.
 */
static uint16_t csum(const char *s, size_t n)
{
    uint32_t a = 0x1505;
    for (size_t i = 0; i < n; i++) {
        a = ((a << 5) + a) + (unsigned char)s[i];
    }
    return (uint16_t)(a & 0xFFFF);
}

static void sanitise(char *dst, size_t cap, const char *src)
{
    size_t j = 0;
    if (cap == 0) {
        return;
    }
    for (size_t i = 0; src != NULL && src[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        /* A comma or a newline in a name would shift or split the record, so
         * they become spaces rather than corrupting every field after them. */
        if (c == ',' || c == '\n' || c == '\r' || c < 0x20 || c > 0x7e) {
            c = ' ';
        }
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

size_t hr_batch_encode(const hr_batch_t *b, char *out, size_t cap)
{
    if (b == NULL || out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';

    char name[HR_BATCH_NAME_MAX];
    sanitise(name, sizeof(name), b->name);

    char params[128];
    size_t at = 0;
    params[0] = '\0';
    for (uint8_t i = 0; i < b->nparams && i < HR_BATCH_PARAMS; i++) {
        int w = snprintf(params + at, sizeof(params) - at, "%s%ld",
                         i ? " " : "", (long)b->params[i]);
        if (w < 0 || (size_t)w >= sizeof(params) - at) {
            return 0;
        }
        at += (size_t)w;
    }

    char body[HR_BATCH_LINE_MAX];
    int n = snprintf(body, sizeof(body),
                     "%d,%lu,%lu,%s,%u,%s,%ld,%d,%d,%ld,%lu,%u,%lu,%lu,%lu",
                     CSV_VERSION,
                     (unsigned long)b->start_epoch,
                     (unsigned long)b->duration_s,
                     name,
                     (unsigned)b->family,
                     params,
                     (long)b->extra_dry_s,
                     (int)b->min_temp_f,
                     (int)b->max_temp_f,
                     (long)b->best_vacuum_um,
                     (unsigned long)b->pulldown_s,
                     (unsigned)b->outcome,
                     (unsigned long)b->freeze_s,
                     (unsigned long)b->dry_s,
                     (unsigned long)b->final_s);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return 0;
    }

    int total = snprintf(out, cap, "%s,%u", body, csum(body, (size_t)n));
    if (total < 0 || (size_t)total >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)total;
}

bool hr_batch_decode(const char *line, hr_batch_t *out)
{
    if (line == NULL || out == NULL) {
        return false;
    }
    size_t len = strlen(line);
    if (len == 0 || len >= HR_BATCH_LINE_MAX) {
        return false;
    }

    /* The checksum is everything after the LAST comma. */
    const char *last = strrchr(line, ',');
    if (last == NULL || last == line) {
        return false;
    }
    size_t body_len = (size_t)(last - line);
    char *endp = NULL;
    unsigned long want = strtoul(last + 1, &endp, 10);
    if (endp == last + 1 || (endp != NULL && *endp != '\0')) {
        return false;
    }
    if (csum(line, body_len) != (uint16_t)want) {
        return false;
    }

    char body[HR_BATCH_LINE_MAX];
    memcpy(body, line, body_len);
    body[body_len] = '\0';

    /* Split on commas. Field 5 (params) is space-separated internally, which
     * is why it can be split this way without a quoting scheme. */
    /*
     * Version 1 had 12 fields; version 2 appends the three phase durations.
     * Both are accepted, so a logbook written before the upgrade still reads.
     */
    char *f[15];
    int nf = 0;
    char *p = body;
    f[nf++] = p;
    while (*p && nf < 15) {
        if (*p == ',') {
            *p = '\0';
            f[nf++] = p + 1;
        }
        p++;
    }
    int ver = atoi(f[0]);
    if ((ver != 1 && ver != 2) || (ver == 1 && nf != 12) ||
        (ver == 2 && nf != 15)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->start_epoch = (uint32_t)strtoul(f[1], NULL, 10);
    out->duration_s  = (uint32_t)strtoul(f[2], NULL, 10);
    sanitise(out->name, sizeof(out->name), f[3]);
    out->family      = (uint8_t)atoi(f[4]);

    out->nparams = 0;
    for (char *q = f[5]; q != NULL && *q && out->nparams < HR_BATCH_PARAMS; ) {
        out->params[out->nparams++] = (int32_t)strtol(q, &q, 10);
        while (*q == ' ') {
            q++;
        }
    }

    out->extra_dry_s    = (int32_t)strtol(f[6], NULL, 10);
    out->min_temp_f     = (int16_t)atoi(f[7]);
    out->max_temp_f     = (int16_t)atoi(f[8]);
    out->best_vacuum_um = (int32_t)strtol(f[9], NULL, 10);
    out->pulldown_s     = (uint32_t)strtoul(f[10], NULL, 10);
    out->outcome        = (uint8_t)atoi(f[11]);
    if (ver >= 2) {
        out->freeze_s = (uint32_t)strtoul(f[12], NULL, 10);
        out->dry_s    = (uint32_t)strtoul(f[13], NULL, 10);
        out->final_s  = (uint32_t)strtoul(f[14], NULL, 10);
    }
    return true;
}

/* ---- tracker ------------------------------------------------------------ */

void hr_batch_tracker_reset(hr_batch_tracker_t *t)
{
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof(*t));
    t->last_phase = -1;
    t->dry_start_elapsed = -1;
}

static void begin(hr_batch_tracker_t *t, uint32_t now_epoch, int32_t temp_f,
                  int32_t elapsed_s)
{
    memset(&t->cur, 0, sizeof(t->cur));
    t->cur.start_epoch = now_epoch;
    t->cur.outcome = HR_OUTCOME_RUNNING;
    t->cur.min_temp_f = (int16_t)temp_f;
    t->cur.max_temp_f = (int16_t)temp_f;
    t->cur.best_vacuum_um = 0;      /* 0 == none seen; any reading beats it */
    t->active = true;
    t->start_elapsed = elapsed_s;
    t->phase_start_elapsed = elapsed_s;
    t->dry_start_elapsed = -1;
    t->pulldown_done = false;
    t->dry_extensions = 0;
}

static void finish(hr_batch_tracker_t *t, hr_outcome_t why, hr_batch_t *out)
{
    t->cur.outcome = (uint8_t)why;
    if (out != NULL) {
        *out = t->cur;
    }
    t->active = false;
}

hr_batch_event_t hr_batch_observe(hr_batch_tracker_t *t, int phase,
                                  int32_t elapsed_s, int32_t temp_f,
                                  int32_t vacuum_um, const char *mode,
                                  uint32_t now_epoch, hr_batch_t *out)
{
    if (t == NULL) {
        return HR_BATCH_NOTHING;
    }

    hr_batch_event_t ev = HR_BATCH_NOTHING;
    const bool running = phase_is_running(phase);

    /*
     * Elapsed going backwards means the dryer started a new run - the same
     * rule main.c uses to decide a stored trend is stale. If a batch is open
     * when that happens, the old one never got a proper ending.
     */
    if (t->active && t->have_last && elapsed_s < t->last_elapsed) {
        if (phase_is_prerun(t->last_phase) && running) {
            /*
             * Not a new run: the dryer counted the preparation phases on their
             * own clock and has just restarted the counter for the batch
             * itself. Rebase onto the new origin and keep the record open.
             *
             * Treating this as a new run is what stopped a real batch from
             * ever reaching the logbook - it closed a 1834-second
             * "interrupted" record at the moment freezing began, and the run
             * that followed was a second batch whose ending nothing watched.
             */
            t->start_elapsed = elapsed_s;
            t->phase_start_elapsed = elapsed_s;
        } else {
            finish(t, HR_OUTCOME_INTERRUPTED, out);
            ev = HR_BATCH_FINISHED;
        }
    }

    if (!t->active && running && ev == HR_BATCH_NOTHING) {
        begin(t, now_epoch, temp_f, elapsed_s);
        ev = HR_BATCH_STARTED;
    }

    if (t->active) {
        /* A delta from where this run was first seen, not the counter's
         * absolute value - which is the previous run's total while idle. */
        int32_t ran = elapsed_s - t->start_elapsed;
        t->cur.duration_s = (uint32_t)(ran > 0 ? ran : 0);

        /* Time in each phase, for estimating the next run. */
        if (phase != t->last_phase && phase_is_running(phase)) {
            t->phase_start_elapsed = elapsed_s;
        }
        if (phase_is_running(phase)) {
            int32_t in_phase = elapsed_s - t->phase_start_elapsed;
            if (in_phase > 0) {
                if (phase == SCR_FREEZING) {
                    t->cur.freeze_s = (uint32_t)in_phase;
                } else if (phase == SCR_DRYING) {
                    t->cur.dry_s = (uint32_t)in_phase;
                } else if (phase == SCR_FINAL_DRY) {
                    t->cur.final_s = (uint32_t)in_phase;
                }
            }
        }
        /* Name the run from the dryer's own mode label. Refreshed on every
         * sample because the field is not reliably populated on the first
         * frame, and an unnamed record answers none of the questions the
         * logbook exists for. */
        if (mode != NULL && mode[0] != '\0' && t->cur.name[0] == '\0') {
            sanitise(t->cur.name, sizeof(t->cur.name), mode);
        }
        if (temp_f < t->cur.min_temp_f) {
            t->cur.min_temp_f = (int16_t)temp_f;
        }
        if (temp_f > t->cur.max_temp_f) {
            t->cur.max_temp_f = (int16_t)temp_f;
        }
        /* Deepest vacuum is the LOWEST non-zero reading, not the last. */
        if (vacuum_um > 0 &&
            (t->cur.best_vacuum_um == 0 || vacuum_um < t->cur.best_vacuum_um)) {
            t->cur.best_vacuum_um = vacuum_um;
        }

        if (phase == SCR_DRYING && t->dry_start_elapsed < 0) {
            t->dry_start_elapsed = elapsed_s;
        }
        if (!t->pulldown_done && t->dry_start_elapsed >= 0 &&
            vacuum_um > 0 && vacuum_um <= PULLDOWN_UM) {
            int32_t d = elapsed_s - t->dry_start_elapsed;
            t->cur.pulldown_s = (uint32_t)(d > 0 ? d : 0);
            t->pulldown_done = true;
        }
    }

    /*
     * Endings. Complete is the good one. Falling back to idle from a running
     * phase without passing through Complete means somebody ended it early.
     */
    if (t->active && ev == HR_BATCH_NOTHING) {
        if (phase == SCR_COMPLETE) {
            finish(t, HR_OUTCOME_COMPLETE, out);
            ev = HR_BATCH_FINISHED;
        } else if (phase == SCR_IDLE && phase_is_running(t->last_phase)) {
            finish(t, HR_OUTCOME_ENDED_EARLY, out);
            ev = HR_BATCH_FINISHED;
        }
    }

    t->last_phase = phase;
    t->last_elapsed = elapsed_s;
    t->have_last = true;
    return ev;
}

bool hr_batch_abandon(hr_batch_tracker_t *t, hr_batch_t *out)
{
    if (t == NULL || !t->active) {
        return false;
    }
    finish(t, HR_OUTCOME_INTERRUPTED, out);
    return true;
}

void hr_batch_set_extra_dry(hr_batch_tracker_t *t, int32_t seconds)
{
    if (t != NULL) {
        t->cur.extra_dry_s = seconds;
    }
}

/* Median of a small array, in place. n > 0. */
static uint32_t median_u32(uint32_t *v, size_t n)
{
    for (size_t i = 1; i < n; i++) {          /* insertion sort; n is tiny */
        uint32_t k = v[i];
        size_t j = i;
        while (j > 0 && v[j - 1] > k) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = k;
    }
    return (n & 1u) ? v[n / 2] : (uint32_t)(((uint64_t)v[n / 2 - 1] +
                                             v[n / 2]) / 2u);
}

#define EST_MAX 16      /* runs considered; more history is not more relevant */

bool hr_batch_estimate(const hr_batch_t *recent, size_t n,
                       hr_batch_estimate_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    uint32_t fr[EST_MAX], dr[EST_MAX], fi[EST_MAX];
    size_t k = 0;

    for (size_t i = 0; i < n && k < EST_MAX; i++) {
        const hr_batch_t *b = &recent[i];
        /*
         * Only completed runs, and only ones that actually recorded phases -
         * a version-1 record has none, and counting its zeros would drag every
         * estimate toward zero without ever looking wrong.
         */
        if (b->outcome != HR_OUTCOME_COMPLETE) {
            continue;
        }
        if (b->freeze_s == 0 || b->dry_s == 0) {
            continue;
        }
        fr[k] = b->freeze_s;
        dr[k] = b->dry_s;
        fi[k] = b->final_s;
        k++;
    }

    if (k == 0) {
        out->freeze_s = HR_SEED_FREEZE_S;
        out->dry_s    = HR_SEED_DRY_S;
        out->final_s  = HR_SEED_FINAL_S;
        out->samples  = 0;
    } else {
        out->freeze_s = median_u32(fr, k);
        out->dry_s    = median_u32(dr, k);
        out->final_s  = median_u32(fi, k);
        out->samples  = (uint8_t)k;
    }
    out->total_s = out->freeze_s + out->dry_s + out->final_s;
    return true;
}

const char *hr_outcome_str(uint8_t outcome)
{
    switch (outcome) {
    case HR_OUTCOME_RUNNING:     return "running";
    case HR_OUTCOME_COMPLETE:    return "complete";
    case HR_OUTCOME_ENDED_EARLY: return "ended early";
    case HR_OUTCOME_INTERRUPTED: return "interrupted";
    default:                     return "unknown";
    }
}
