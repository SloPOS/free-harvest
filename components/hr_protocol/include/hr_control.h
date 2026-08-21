/*
 * hr_control - named, screen-validated control actions.
 *
 * The dryer's control verb is CLICK, captured from the genuine app:
 *
 *     CLICK <screen> <button> <counter> <session>
 *     CLICK 1 10 54779 175300      = Start, on screen 1 (Ready)
 *
 * WHY THIS LAYER EXISTS
 *
 * Button numbers are SCREEN-RELATIVE. Button 10 is Start on Ready; on another
 * screen the same number is some other control. That makes a stale view the
 * real hazard: a phone still showing Ready while the machine has moved on would
 * send button 10 against whatever screen is now live.
 *
 * A UI cannot solve this, because a UI is always behind. So the caller sends
 * the screen it BELIEVED it was looking at, and this layer refuses if that no
 * longer matches live telemetry - optimistic concurrency, like an HTTP ETag.
 * Raw CLICK is never exposed; only named actions bound to a required screen.
 */
#ifndef HR_CONTROL_H
#define HR_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* Reversible, or merely navigational: opening settings, going back. */
    HR_SEV_BENIGN = 0,
    /* Changes what the machine is doing, but along the intended path. */
    HR_SEV_CONFIRM,
    /* Ends a cycle early, or advances past a step the recipe wanted. Losing a
     * 24-hour run to a mis-tap is the failure mode this class exists for. */
    HR_SEV_DESTRUCTIVE,
} hr_severity_t;

typedef struct {
    const char   *name;    /* stable id used on the wire: "start_auto"      */
    const char   *label;   /* human text for the button: "Start (Auto)"     */
    int           screen;  /* STAT type this action is only valid on        */
    int           button;  /* button number within that screen              */
    hr_severity_t sev;
} hr_action_t;

typedef enum {
    HR_CTRL_OK = 0,
    HR_CTRL_UNKNOWN_ACTION,  /* no such action name                          */
    HR_CTRL_STALE_VIEW,      /* caller's screen != live screen               */
    HR_CTRL_WRONG_SCREEN,    /* action is not offered on the live screen     */
    HR_CTRL_NEEDS_CONFIRM,   /* severity requires an explicit confirmation   */
    HR_CTRL_NO_TELEMETRY,    /* live screen unknown - never guess            */
} hr_ctrl_result_t;

/* Look up an action by name. NULL when unknown. */
const hr_action_t *hr_control_lookup(const char *name);

/* Fill `out` with the actions valid on `screen`. Returns how many were written,
 * capped at `cap`. Used to render only the buttons the machine is offering. */
size_t hr_control_for_screen(int screen, const hr_action_t **out, size_t cap);

/* Total actions known, for enumeration and tests. */
size_t hr_control_count(void);
const hr_action_t *hr_control_at(size_t i);

/*
 * Decide whether an action may be sent right now.
 *
 * live_screen is the STAT type from current telemetry, or < 0 when there is no
 * telemetry at all. believed_screen is what the caller was looking at when the
 * user tapped. confirmed is the caller's explicit acknowledgement.
 */
hr_ctrl_result_t hr_control_check(const char *name, int believed_screen,
                                  int live_screen, bool confirmed,
                                  const hr_action_t **out_action);

/* Human-readable reason, for API responses and logs. */
const char *hr_ctrl_result_str(hr_ctrl_result_t r);

/*
 * Build the wire frame. Fields are space-delimited, matching the captured
 * format. Returns bytes written, 0 on failure. The frame terminator is added
 * by the session layer, not here.
 */
size_t hr_control_build_click(char *out, size_t cap, int screen, int button,
                              uint32_t counter, uint32_t session);

#ifdef __cplusplus
}
#endif

#endif /* HR_CONTROL_H */
