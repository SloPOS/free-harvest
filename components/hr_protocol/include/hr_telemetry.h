/*
 * hr_telemetry - turn a decoded STAT frame into named sensor values for the
 * MQTT / Home Assistant layer, and build HA MQTT-discovery + state payloads.
 *
 * STAT is multiplexed: field[0] (after the verb) is a TYPE discriminator that
 * changes the layout. Field meanings were recovered from live captures +
 * firmware (see decoded/PROTOCOL_NOTES.md):
 *   shared header: [1]=type [5]=temperature(F) [6]=pressure(raw) [7]=batch
 *   elapsed seconds. type 17 adds a 15-min prep countdown at field [16].
 *
 * No ESP-IDF deps - fully host-testable.
 */
#ifndef HR_TELEMETRY_H
#define HR_TELEMETRY_H

#include "hr_protocol.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pressure placeholder. While the vacuum pump is off (idle, prep, early
 * freeze) the dryer reports exactly this value instead of a reading. Once the
 * pump runs, field [5] carries a REAL vacuum measurement in microns (mTorr) -
 * observed falling 1209 -> 440 during a pulldown.
 */
#define HR_PRESSURE_PLACEHOLDER 10000

typedef struct {
    bool valid;          /* false if the frame was not a usable STAT */
    int type;            /* STAT type discriminator (1,2,4,5,15,17,31,...) */
    long temperature_f;  /* field 5, degrees F */
    long pressure_raw;   /* field 6, as reported */
    bool pressure_valid; /* false when the reading is the 10000 placeholder */
    long pressure_microns; /* vacuum in microns/mTorr; 0 when not valid */
    long batch_elapsed_s;/* field 7, seconds since batch start (0 = idle) */
    long phase_elapsed_s;/* field 8, seconds since THIS phase began */
    bool prep_active;    /* true for type-17 prep countdown frames */
    long prep_remaining_s; /* seconds remaining in 15-min prep (type 17) */
    bool freeze_active;  /* true for type-4 freezing frames */
    /*
     * Progress percentage within the current phase (field [11]). During
     * freezing it tracks cooling; during the vacuum pull it tracks pressure.
     * Resets when the phase changes.
     */
    long phase_pct;
    long freeze_pct;     /* alias of phase_pct while freezing (compat) */
    char mode[16];       /* mode string when present (e.g. "Auto","QUALITY") */
    char version[24];    /* firmware version string when present */
    /*
     * Type 8, the pump purge the dryer runs on the way to a defrost (see
     * HR_PHASE_PUMP_PURGE). Field indices are the ones hr_frame_field()
     * counts, where [0] is the type.
     *
     * purge_active is true for every type-8 frame; purge_pump_on only once
     * the pump is actually venting (bit 2 of the flags in [9]).
     * purge_remaining_s is the dryer's own countdown in [10], and
     * defrost_offer_s the defrost duration the panel offers in [11].
     */
    bool purge_active;
    bool purge_pump_on;
    long purge_remaining_s;
    long defrost_offer_s;
} hr_telemetry_t;

/*
 * Parse a decoded STAT frame into telemetry. Returns false (and sets
 * out->valid=false) if `f` is not a STAT frame or is too short.
 */
bool hr_telemetry_from_stat(const hr_frame_t *f, hr_telemetry_t *out);

/*
 * Build a compact JSON object of the telemetry into `buf` for publishing to
 * the MQTT state topic. Returns bytes written (excluding NUL), 0 on overflow.
 * Example: {"type":1,"temp_f":69,"pressure":151882,"elapsed_s":0,
 *           "mode":"QUALITY","prep_s":0,...,"purge_s":0}
 * purge_s is the type-8 pump-purge countdown, 0 on every other screen.
 */
size_t hr_telemetry_to_json(const hr_telemetry_t *t, char *buf, size_t cap);

/* ------------------------------------------------------------------ */
/* Cycle phase                                                         */
/* ------------------------------------------------------------------ */
/*
 * Phase of the freeze-drying cycle, used to decide which screen and which
 * (panel) options to show - mirroring the owner's manual screens.
 *
 * CONFIDENCE: IDLE / PREPARING / TRANSITION / FREEZING / DRYING / FINAL_DRY /
 * COMPLETE / RUNNING, the DIAGNOSTICS and RECIPE views, and PUMP_PURGE are
 * confirmed against real captures. DEFROST and DEFROST_DONE are not: they
 * carry a label and nothing else until someone captures the frames.
 */
typedef enum {
    HR_PHASE_UNKNOWN = 0,
    HR_PHASE_IDLE,        /* type 1, no batch elapsed - "press START on dryer" */
    HR_PHASE_PREPARING,   /* type 17 - 15-minute pre-cool countdown */
    HR_PHASE_TRANSITION,  /* type 2 - seen between prep and run */
    HR_PHASE_RUNNING,     /* type 1 with elapsed>0 - batch under way */
    HR_PHASE_DIAGNOSTICS, /* type 15 - diagnostics/test screen */
    HR_PHASE_RECIPE,      /* type 31 - recipe/profile parameters */
    /*
     * type 4 - deep freeze after the user loads trays and presses CONTINUE.
     * The chamber cools toward the freeze target before vacuum is pulled
     * (pressure stays at the 10000 placeholder throughout). Field [11] is a
     * progress percentage that rises as the temperature falls.
     */
    HR_PHASE_FREEZING,
    /*
     * type 5 - drying. Vacuum has been pulled and holds (~435 microns
     * observed) while shelf heat is applied, so the temperature RISES
     * (-18F -> -13F observed). Announced by an NTFY,5 frame.
     */
    HR_PHASE_DRYING,
    /*
     * type 6 - final / secondary dry. The longest stage by far (12.9h in a
     * 25.6h capture): shelf temperature holds high (~124F) while the vacuum
     * is pulled to its DEEPEST point (~204 microns).
     */
    HR_PHASE_FINAL_DRY,
    /*
     * type 7 - venting / batch complete. Unmistakable: the vacuum is released
     * and pressure jumps from a few hundred microns back to atmosphere
     * (206 -> 148,066 observed) as the drain valve opens. This is the
     * end-of-cycle marker.
     */
    HR_PHASE_COMPLETE,
    /*
     * Everything below is APPENDED so the numbers above never move: the web
     * UI and MQTT consumers compare against them.
     */
    /*
     * type 8 - the screen between "batch complete" and a defrost, reached by
     * choosing DEFROST when the batch ends. On a machine with an oil-free
     * pump the dryer vents the pump for five minutes while the owner sets the
     * defrost time: [9] carries flags whose bit 2 is "venting now", [10]
     * counts 300 seconds down to zero, and [11] is the defrost time offered.
     *
     * Captured 2026-09-17 on a 6.0.644170 machine (22 frames): flags 3 -> 7
     * as the pump started, the countdown running out, then the Ready frame.
     * The batch-elapsed counter keeps ADVANCING on this screen although the
     * batch is over, so this is deliberately not a running phase.
     *
     * This screen uses the short STAT header - no mode block - which is why
     * its own fields start right after the status code, as on types 1 and 7.
     */
    HR_PHASE_PUMP_PURGE,
    /*
     * type 9 - the defrost itself; type 10 - defrost finished. Reported from
     * a read of the dryer's own screen table (vskiwi, PR #10), NOT from a
     * capture: neither frame has been seen on the wire, so nothing beyond the
     * shared header is decoded and the label is all that is claimed.
     */
    HR_PHASE_DEFROST,
    HR_PHASE_DEFROST_DONE,
} hr_phase_t;

/* Short human label, e.g. "Preparing dryer". Never NULL. */
const char *hr_phase_label(hr_phase_t p);

/* ------------------------------------------------------------------ */
/* Phase tracking (stateful)                                           */
/* ------------------------------------------------------------------ */
/*
 * The dryer's batch-elapsed counter does NOT reset when a batch ends - it
 * keeps the last batch's value and simply STOPS ADVANCING. (Confirmed in
 * capture: elapsed sat at 193 for >3 minutes of wall time while idle.)
 * So "elapsed > 0" does not mean "running"; only a *rising* elapsed does.
 *
 * hr_phase_tracker_t watches successive frames and reports RUNNING only while
 * the counter actually advances, falling back to IDLE after it goes stale.
 */
#define HR_RUN_STALE_MS 45000UL /* elapsed unchanged this long => not running */

/*
 * How many recent percent observations feed the ETA rate. Each entry is one
 * new percent value, so this spans the last few percent of progress rather
 * than a fixed wall-clock window - which is the right axis, because the whole
 * point is that seconds-per-percent changes as the run proceeds.
 */
#define HR_FREEZE_WINDOW 6

typedef struct {
    long last_elapsed;      /* last seen batch_elapsed_s */
    unsigned long last_change_ms; /* when it last increased */
    unsigned long last_seen_ms;   /* when we last had a frame */
    bool have;              /* seen at least one frame */
    bool running;           /* current running determination */

    /*
     * Freeze-progress rate tracking, for an ETA to 100%. We remember the
     * batch-elapsed time at which each new percent was first observed, so the
     * rate is measured against the dryer's own clock (immune to adapter
     * reboots and Wi-Fi gaps).
     */
    long freeze_first_pct;       /* first percent value we saw */
    long freeze_first_elapsed;   /* batch elapsed when we saw it */
    long freeze_last_pct;        /* most recent percent */
    long freeze_last_elapsed;    /* batch elapsed at that percent */

    /*
     * Recent-window rate.
     *
     * The whole-run average above is systematically optimistic near the end.
     * Freezing is not linear in time: the chamber approaches its target
     * asymptotically, so the last few percent take far longer than the first
     * few. An average taken from the first sighting keeps reporting the early,
     * fast rate long after the machine has stopped achieving it.
     *
     * This ring keeps the last few (percent, elapsed) observations so the rate
     * can be measured over recent behaviour instead. It is deliberately NOT a
     * curve fit - that needs a captured freeze curve to choose a model against,
     * and none has been recorded yet. This is the model-free improvement that
     * can be made honestly in the meantime.
     */
    long freeze_win_pct[HR_FREEZE_WINDOW];
    long freeze_win_elapsed[HR_FREEZE_WINDOW];
    int  freeze_win_head;        /* next slot to write */
    int  freeze_win_count;       /* valid entries, saturating at the size */
} hr_phase_tracker_t;

void hr_phase_tracker_init(hr_phase_tracker_t *tr);

/*
 * Feed a decoded frame with the current time. Updates the running/idle
 * determination. Call for every valid STAT.
 */
void hr_phase_tracker_update(hr_phase_tracker_t *tr, const hr_telemetry_t *t,
                             unsigned long now_ms);

/* Advance time without a new frame (lets a run go stale). */
void hr_phase_tracker_tick(hr_phase_tracker_t *tr, unsigned long now_ms);

/*
 * Derive the cycle phase. `tr` may be NULL, in which case RUNNING vs IDLE
 * falls back to the (unreliable) elapsed>0 test.
 */
hr_phase_t hr_phase_of_tracked(const hr_telemetry_t *t,
                               const hr_phase_tracker_t *tr);

/* Backwards-compatible stateless form (elapsed>0 heuristic). */
hr_phase_t hr_phase_of(const hr_telemetry_t *t);

/* ------------------------------------------------------------------ */
/* Vacuum units + comparisons                                          */
/* ------------------------------------------------------------------ */
/*
 * The dryer reports vacuum in MICRONS (millitorr) - confirmed by the string
 * "HighmTorr" in its own firmware's vacuum alarm. 1 micron = 1 mTorr =
 * 0.001 Torr. Sea level is ~760,000 microns.
 */
#define HR_MICRONS_PER_TORR 1000.0
#define HR_SEA_LEVEL_MICRONS 760000.0

/* Convert microns to other units. */
double hr_microns_to_torr(long microns);
double hr_microns_to_bar(long microns);
double hr_microns_to_atm(long microns);
double hr_microns_to_pascal(long microns);

/*
 * A human comparison for the current vacuum level, e.g.
 * "lower than the surface of Mars". Returns a short phrase, never NULL.
 * `detail` (optional, may be NULL) receives a longer explanatory sentence.
 */
const char *hr_vacuum_comparison(long microns, const char **detail);

/*
 * Estimated seconds remaining until freeze progress reaches 100%, based on
 * the observed rate so far. Returns -1 when there isn't enough data yet (we
 * need at least two distinct percent values) or we aren't freezing.
 *
 * NOTE: this is a linear extrapolation of an approximately-linear-but-not-
 * really process; freezing usually slows near the target. Present it as an
 * estimate, not a countdown.
 */
long hr_freeze_eta_s(const hr_phase_tracker_t *tr, const hr_telemetry_t *t);

#ifdef __cplusplus
}
#endif

#endif /* HR_TELEMETRY_H */
