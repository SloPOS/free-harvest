#include "hr_telemetry.h"

#include <stdio.h>
#include <string.h>

/* Shared-header field indices (0-based, AFTER the verb):
 *   [0] type   [4] temperature F   [5] pressure raw   [6] batch elapsed s
 * Mode/version/prep positions depend on the type (see below). */
#define F_TYPE 0
#define F_TEMP 4
#define F_PRESSURE 5
#define F_ELAPSED 6

static void copy_field(char *dst, size_t cap, const hr_frame_t *f, int idx)
{
    const char *s = hr_frame_field(f, idx);
    if (s == NULL) {
        dst[0] = '\0';
        return;
    }
    /* skip pure-whitespace placeholder fields like " " */
    while (*s == ' ') {
        s++;
    }
    snprintf(dst, cap, "%s", s);
}

bool hr_telemetry_from_stat(const hr_frame_t *f, hr_telemetry_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (f == NULL || out == NULL) {
        return false;
    }
    if (strcmp(f->verb, "STAT") != 0) {
        return false;
    }
    /* Need at least through the shared header (type..elapsed). */
    if (f->nfields <= F_ELAPSED) {
        return false;
    }

    out->type = (int)hr_frame_field_int(f, F_TYPE, -1);
    if (out->type < 0) {
        return false;
    }
    out->temperature_f = hr_frame_field_int(f, F_TEMP, 0);
    out->pressure_raw = hr_frame_field_int(f, F_PRESSURE, 0);
    out->batch_elapsed_s = hr_frame_field_int(f, F_ELAPSED, 0);
    /* [8] counts seconds since the CURRENT phase started (resets each phase). */
    out->phase_elapsed_s = hr_frame_field_int(f, 7, 0);

    /*
     * Pressure: 10000 is a "pump off / no reading" placeholder. Any other
     * value is a real vacuum measurement in microns (mTorr) - confirmed by a
     * capture showing it fall 1209 -> 440 during a pulldown.
     */
    if (out->pressure_raw == HR_PRESSURE_PLACEHOLDER ||
        out->pressure_raw > HR_PRESSURE_PLACEHOLDER) {
        /*
         * 10000 exactly = "pump off" placeholder. Anything ABOVE it is the
         * uncalibrated idle sensor reading (~120k-155k at atmosphere), not a
         * vacuum measurement - a real pulldown reads well under 10000.
         */
        out->pressure_valid = false;
        out->pressure_microns = 0;
    } else {
        out->pressure_valid = true;
        out->pressure_microns = out->pressure_raw;
    }

    if (out->type == 17) {
        /* Prep countdown frame: mode at [9], seconds-remaining at [16]. */
        out->prep_active = true;
        out->prep_remaining_s = hr_frame_field_int(f, 16, 0);
        copy_field(out->mode, sizeof(out->mode), f, 9);
    } else if (out->type == 4 || out->type == 5 ||
               out->type == 6 || out->type == 7) {
        /*
         * In-cycle frames (freezing = 4, drying = 5). Same field layout as the
         * prep frame: mode at [9], phase-progress percent at [11].
         *
         * [11] is progress WITHIN the phase, not specifically temperature:
         * during freezing it tracked cooling (83->85 as temp fell 5F->3F);
         * during the vacuum pull it tracked pressure (94->99 as the vacuum
         * went 1209->441 microns). It resets when the phase changes.
         */
        out->phase_pct = hr_frame_field_int(f, 11, 0);
        if (out->type == 4) {
            out->freeze_active = true;
            out->freeze_pct = out->phase_pct; /* compat alias */
        }
        copy_field(out->mode, sizeof(out->mode), f, 9);
    } else if (out->type == 1) {
        /* Idle frame: mode at [11], version at [12]. */
        copy_field(out->mode, sizeof(out->mode), f, 11);
        copy_field(out->version, sizeof(out->version), f, 12);
    }
    /*
     * Any OTHER type: layout unconfirmed, so decode nothing beyond the
     * shared header.
     *
     * This used to fall through to the type-1 layout, which produced
     * nonsense: a screen walk showed mode reported as "5" on type 2,
     * "-15" on type 31 and "5" on type 15, because field [11] means
     * something different on each of those screens. STAT is multiplexed on
     * the type discriminator and only types 1, 4-7 and 17 have been
     * confirmed against real captures.
     *
     * Leaving the field empty is honest; showing a number as if it were a
     * mode is not.
     */

    out->valid = true;
    return true;
}

void hr_phase_tracker_init(hr_phase_tracker_t *tr)
{
    if (tr != NULL) {
        memset(tr, 0, sizeof(*tr));
        tr->last_elapsed = -1;
        tr->freeze_first_pct = -1;
        tr->freeze_last_pct = -1;
    }
}

/*
 * Record freeze progress against the dryer's own batch clock. Using the
 * dryer's elapsed counter (not our uptime) keeps the rate correct across
 * adapter reboots and Wi-Fi outages.
 */
static void track_freeze(hr_phase_tracker_t *tr, const hr_telemetry_t *t)
{
    if (!t->freeze_active) {
        return;
    }
    long pct = t->freeze_pct;
    long el = t->batch_elapsed_s;

    if (tr->freeze_first_pct < 0 || pct < tr->freeze_last_pct) {
        /* first sighting, or progress went backwards (new freeze) - restart */
        tr->freeze_first_pct = pct;
        tr->freeze_first_elapsed = el;
        tr->freeze_last_pct = pct;
        tr->freeze_last_elapsed = el;
        return;
    }
    if (pct > tr->freeze_last_pct) {
        tr->freeze_last_pct = pct;
        tr->freeze_last_elapsed = el;

        /* Record the observation for the recent-window rate. */
        tr->freeze_win_pct[tr->freeze_win_head] = pct;
        tr->freeze_win_elapsed[tr->freeze_win_head] = el;
        tr->freeze_win_head = (tr->freeze_win_head + 1) % HR_FREEZE_WINDOW;
        if (tr->freeze_win_count < HR_FREEZE_WINDOW) {
            tr->freeze_win_count++;
        }
    }
}

long hr_freeze_eta_s(const hr_phase_tracker_t *tr, const hr_telemetry_t *t)
{
    if (tr == NULL || t == NULL || !t->valid || !t->freeze_active) {
        return -1;
    }
    if (t->freeze_pct >= 100) {
        return 0;
    }
    /* Need two distinct percent readings to have a rate at all. */
    if (tr->freeze_first_pct < 0 ||
        tr->freeze_last_pct <= tr->freeze_first_pct) {
        return -1;
    }
    /*
     * Prefer the recent window. With a full window we have the oldest and
     * newest of the last HR_FREEZE_WINDOW percent observations, which measures
     * what the machine is achieving NOW rather than what it averaged since the
     * run began. Falls back to the whole-run figure until the window fills, so
     * an ETA still appears early instead of showing nothing.
     */
    long dpct, dt;
    if (tr->freeze_win_count >= 2) {
        int newest = (tr->freeze_win_head - 1 + HR_FREEZE_WINDOW)
                     % HR_FREEZE_WINDOW;
        int oldest = (tr->freeze_win_head - tr->freeze_win_count
                      + HR_FREEZE_WINDOW * 2) % HR_FREEZE_WINDOW;
        dpct = tr->freeze_win_pct[newest] - tr->freeze_win_pct[oldest];
        dt = tr->freeze_win_elapsed[newest] - tr->freeze_win_elapsed[oldest];
    } else {
        dpct = tr->freeze_last_pct - tr->freeze_first_pct;
        dt = tr->freeze_last_elapsed - tr->freeze_first_elapsed;
    }
    if (dpct <= 0 || dt <= 0) {
        return -1;
    }
    long remaining = 100 - tr->freeze_last_pct;
    if (remaining <= 0) {
        return 0;
    }
    /* seconds-per-percent * percent remaining */
    return (dt * remaining) / dpct;
}

void hr_phase_tracker_update(hr_phase_tracker_t *tr, const hr_telemetry_t *t,
                             unsigned long now_ms)
{
    if (tr == NULL || t == NULL || !t->valid) {
        return;
    }
    tr->last_seen_ms = now_ms;

    /* Freeze-progress rate, for the ETA. Safe to call for any frame. */
    track_freeze(tr, t);

    /*
     * Prep frames carry their own countdown and are handled by type; they do
     * not tell us anything about the batch-elapsed counter, so leave the
     * running determination alone while preparing.
     */
    if (t->type == 17) {
        tr->have = true;
        return;
    }

    if (!tr->have) {
        /* First frame: adopt the value but do NOT claim a run - we have no
         * evidence yet that it is advancing (it may be last batch's value). */
        tr->have = true;
        tr->last_elapsed = t->batch_elapsed_s;
        tr->last_change_ms = now_ms;
        tr->running = false;
        return;
    }

    if (t->batch_elapsed_s > tr->last_elapsed) {
        tr->last_elapsed = t->batch_elapsed_s;
        tr->last_change_ms = now_ms;
        tr->running = true;
    } else {
        if (t->batch_elapsed_s < tr->last_elapsed) {
            /* counter reset (new batch armed) - treat as a fresh baseline */
            tr->last_elapsed = t->batch_elapsed_s;
            tr->last_change_ms = now_ms;
        }
        if (now_ms - tr->last_change_ms > HR_RUN_STALE_MS) {
            tr->running = false;
        }
    }
}

void hr_phase_tracker_tick(hr_phase_tracker_t *tr, unsigned long now_ms)
{
    if (tr == NULL || !tr->have) {
        return;
    }
    if (tr->running && (now_ms - tr->last_change_ms) > HR_RUN_STALE_MS) {
        tr->running = false;
    }
}

hr_phase_t hr_phase_of_tracked(const hr_telemetry_t *t,
                               const hr_phase_tracker_t *tr)
{
    if (t == NULL || !t->valid) {
        return HR_PHASE_UNKNOWN;
    }
    /* Non type-1 frames are classified purely by type. */
    if (t->type != 1) {
        return hr_phase_of(t);
    }
    if (tr == NULL) {
        return hr_phase_of(t); /* legacy heuristic */
    }
    return tr->running ? HR_PHASE_RUNNING : HR_PHASE_IDLE;
}

/* ---- vacuum unit conversions ------------------------------------------- */
/* 1 micron = 1 mTorr = 0.001 Torr. 1 Torr = 133.322 Pa = 0.00133322 bar. */
double hr_microns_to_torr(long microns)
{
    return (double)microns / HR_MICRONS_PER_TORR;
}

double hr_microns_to_bar(long microns)
{
    return hr_microns_to_torr(microns) * 0.00133322;
}

double hr_microns_to_atm(long microns)
{
    return hr_microns_to_torr(microns) / 760.0;
}

double hr_microns_to_pascal(long microns)
{
    return hr_microns_to_torr(microns) * 133.322;
}

const char *hr_vacuum_comparison(long microns, const char **detail)
{
    static const char *d;
    if (microns <= 0) {
        d = "No vacuum reading - the pump isn't running.";
        if (detail) *detail = d;
        return "no vacuum";
    }
    /*
     * Reference points (in microns):
     *   sea level      760,000      Everest summit  253,000
     *   Mars surface     ~4,500     freeze-dry target ~500
     * A working freeze dryer pulls BELOW the Martian surface pressure.
     */
    if (microns >= 500000) {
        d = "Close to normal room pressure - the chamber has barely started "
            "pumping down.";
        if (detail) *detail = d;
        return "about sea-level pressure";
    }
    if (microns >= 250000) {
        d = "Thinner than the air on most mountain tops.";
        if (detail) *detail = d;
        return "like a high mountain";
    }
    if (microns >= 50000) {
        d = "Thinner than the air at the summit of Everest, where climbers "
            "need bottled oxygen.";
        if (detail) *detail = d;
        return "thinner than Everest's summit";
    }
    if (microns >= 4500) {
        d = "Approaching the pressure at the surface of Mars - about 0.6% of "
            "Earth's atmosphere.";
        if (detail) *detail = d;
        return "nearing Martian pressure";
    }
    if (microns >= 1000) {
        d = "Below the surface pressure of Mars. Water can't stay liquid here - "
            "ice turns straight to vapour.";
        if (detail) *detail = d;
        return "below the surface of Mars";
    }
    if (microns >= 200) {
        d = "A proper freeze-drying vacuum - many times lower than the Martian "
            "surface. This is where sublimation runs well.";
        if (detail) *detail = d;
        return "deep freeze-drying vacuum";
    }
    d = "An exceptionally hard vacuum for a home machine - approaching the "
        "thin upper edge of Earth's atmosphere.";
    if (detail) *detail = d;
    return "near-space vacuum";
}

hr_phase_t hr_phase_of(const hr_telemetry_t *t)
{
    if (t == NULL || !t->valid) {
        return HR_PHASE_UNKNOWN;
    }
    switch (t->type) {
    case 17:
        return HR_PHASE_PREPARING;
    case 4:
        return HR_PHASE_FREEZING;
    case 5:
        return HR_PHASE_DRYING;
    case 6:
        return HR_PHASE_FINAL_DRY;
    case 7:
        return HR_PHASE_COMPLETE;
    case 2:
        return HR_PHASE_TRANSITION;
    case 15:
        return HR_PHASE_DIAGNOSTICS;
    case 31:
        return HR_PHASE_RECIPE;
    case 1:
        /* A batch is under way once the elapsed counter starts moving. Note
         * the dryer also sits in type 1 AFTER a batch finishes, so "running"
         * is decided by whether the counter is still advancing. */
        return (t->batch_elapsed_s > 0) ? HR_PHASE_RUNNING : HR_PHASE_IDLE;
    default:
        return HR_PHASE_UNKNOWN;
    }
}

const char *hr_phase_label(hr_phase_t p)
{
    switch (p) {
    case HR_PHASE_IDLE:        return "Ready";
    case HR_PHASE_PREPARING:   return "Preparing dryer";
    case HR_PHASE_TRANSITION:  return "Starting batch";
    case HR_PHASE_RUNNING:     return "Batch running";
    case HR_PHASE_DIAGNOSTICS: return "Diagnostics";
    case HR_PHASE_RECIPE:      return "Recipe settings";
    case HR_PHASE_FREEZING:    return "Freezing";
    case HR_PHASE_DRYING:      return "Drying";
    case HR_PHASE_FINAL_DRY:   return "Final dry";
    case HR_PHASE_COMPLETE:    return "Batch complete";
    default:                   return "Unknown";
    }
}

size_t hr_telemetry_to_json(const hr_telemetry_t *t, char *buf, size_t cap)
{
    if (t == NULL || buf == NULL) {
        return 0;
    }
    int n = snprintf(buf, cap,
                     "{\"type\":%d,\"temp_f\":%ld,\"pressure\":%ld,"
                     "\"elapsed_s\":%ld,\"mode\":\"%s\",\"prep_s\":%ld,"
                     "\"freeze_pct\":%ld,\"phase_pct\":%ld,"
                     "\"phase_s\":%ld,\"vacuum_um\":%ld,\"vacuum_ok\":%s}",
                     t->type, t->temperature_f, t->pressure_raw,
                     t->batch_elapsed_s, t->mode, t->prep_remaining_s,
                     t->freeze_pct, t->phase_pct, t->phase_elapsed_s,
                     t->pressure_microns, t->pressure_valid ? "true" : "false");
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    return (size_t)n;
}
