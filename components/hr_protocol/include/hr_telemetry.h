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

typedef struct {
    bool valid;          /* false if the frame was not a usable STAT */
    int type;            /* STAT type discriminator (1,2,15,17,31,...) */
    long temperature_f;  /* field 5, degrees F */
    long pressure_raw;   /* field 6, raw sensor counts */
    long batch_elapsed_s;/* field 7, seconds since batch start (0 = idle) */
    bool prep_active;    /* true for type-17 prep countdown frames */
    long prep_remaining_s; /* seconds remaining in 15-min prep (type 17) */
    char mode[16];       /* mode string when present (e.g. "Auto","QUALITY") */
    char version[24];    /* firmware version string when present */
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
 *           "mode":"QUALITY","prep_s":0}
 */
size_t hr_telemetry_to_json(const hr_telemetry_t *t, char *buf, size_t cap);

/* ------------------------------------------------------------------ */
/* Cycle phase                                                         */
/* ------------------------------------------------------------------ */
/*
 * Phase of the freeze-drying cycle, used to decide which screen and which
 * (panel) options to show - mirroring the owner's manual screens.
 *
 * CONFIDENCE: HR_PHASE_IDLE / PREPARING / RUNNING and the DIAGNOSTICS and
 * RECIPE views are confirmed against real captures. The finer sub-phases the
 * manual describes (Freezing vs Drying vs Extra Dry vs Complete vs Defrost)
 * have NOT been observed on the wire yet - a full cycle capture is needed
 * before they can be distinguished, so they are not guessed at here.
 */
typedef enum {
    HR_PHASE_UNKNOWN = 0,
    HR_PHASE_IDLE,        /* type 1, no batch elapsed - "press START on dryer" */
    HR_PHASE_PREPARING,   /* type 17 - 15-minute pre-cool countdown */
    HR_PHASE_TRANSITION,  /* type 2 - seen between prep and run */
    HR_PHASE_RUNNING,     /* type 1 with elapsed>0 - batch under way */
    HR_PHASE_DIAGNOSTICS, /* type 15 - diagnostics/test screen */
    HR_PHASE_RECIPE,      /* type 31 - recipe/profile parameters */
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

typedef struct {
    long last_elapsed;      /* last seen batch_elapsed_s */
    unsigned long last_change_ms; /* when it last increased */
    unsigned long last_seen_ms;   /* when we last had a frame */
    bool have;              /* seen at least one frame */
    bool running;           /* current running determination */
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

#ifdef __cplusplus
}
#endif

#endif /* HR_TELEMETRY_H */
