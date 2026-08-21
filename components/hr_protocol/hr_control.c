#include "hr_control.h"

#include <stdio.h>
#include <string.h>

/*
 * THE ACTION TABLE
 *
 * Every row below is MEASURED - captured from the genuine app driving each
 * screen on 2026-08-21. Nothing here is inferred, and rows must only ever be
 * added the same way. Guessing a button number is guessing which control gets
 * pressed on a running machine.
 *
 *     CLICK 1  10  Start Auto        CLICK 4  3  Skip to Drying
 *     CLICK 1   9  Start Custom      CLICK 4  4  End Batch
 *     CLICK 1   8  Start Candy       CLICK 5  1  End Batch
 *     CLICK 1   3  Settings          CLICK 6  1  End Batch
 *     CLICK 17  3  Skip to Freezing  CLICK 6  2  More Dry Time
 *     CLICK 17  4  End Batch         CLICK 6  3  Less Dry Time
 *
 * The trailing field is 175300 in every capture, across three separate
 * sessions on different days, so it is a fixed constant rather than a session
 * token. The counter before it is a GLOBAL command sequence shared with other
 * verbs - a captured CLICK ...49059 was followed by SPC ...49060.
 */
static const hr_action_t k_actions[] = {
    /* --- Screen 1: Ready ------------------------------------------------ */
    /*
     * "config" (button 3) IS DELIBERATELY ABSENT.
     *
     * It opens the machine's settings/diagnostics page, and that page stops
     * servicing USB - telemetry went silent for ~147 seconds in a capture and
     * only resumed when the panel was dismissed BY HAND. So it is the one
     * button that destroys the channel it was pressed over: no state arrives,
     * no further command can be sent, and nothing remote can undo it.
     *
     * There is no confirmation dialog that makes that safe, because the cost is
     * not "are you sure" - it is "someone must now walk to the machine". So it
     * is not offered rather than offered-with-a-warning.
     *
     * Candy and Custom do NOT start anything. Each opens a recipe
     * configuration screen (STAT type 43), which keeps talking over USB and can
     * be backed out of, but whose buttons we have not captured - so they lead
     * to a screen the UI cannot yet operate. Labelled for what they actually
     * do rather than what the app's own button text implies.
     */
    { "candy_setup",    "Candy Setup…",    1,  8, HR_SEV_CONFIRM     },
    { "custom_setup",   "Custom Setup…",   1,  9, HR_SEV_CONFIRM     },
    { "start_auto",     "Start Auto",          1, 10, HR_SEV_CONFIRM     },

    /* --- Screen 17: Preparing (pre-cool) -------------------------------- */
    { "prep_advance",   "Skip to Freezing",   17,  3, HR_SEV_DESTRUCTIVE },
    { "prep_end",       "End Batch",          17,  4, HR_SEV_DESTRUCTIVE },

    /* --- Screen 4: Freezing --------------------------------------------- */
    { "freeze_advance", "Skip to Drying",      4,  3, HR_SEV_DESTRUCTIVE },
    { "freeze_end",     "End Batch",           4,  4, HR_SEV_DESTRUCTIVE },

    /* --- Screen 2: Load trays / Continue ---------------------------------- */
    /*
     * This is the screen a remote start stalls on. CONTINUE means "the trays
     * are in and the valve is shut" - a claim about the physical world that
     * nobody standing at a phone can actually make. Pressing it from away
     * commits the machine to a run on whatever is, or is not, inside.
     */
    { "load_continue",  "Trays Loaded – Continue", 2, 1, HR_SEV_CONFIRM },
    { "load_end",       "End Batch",           2,  2, HR_SEV_DESTRUCTIVE },

    /* --- Screen 5: Drying ------------------------------------------------ */
    { "dry_end",        "End Batch",           5,  1, HR_SEV_DESTRUCTIVE },

    /* --- Screen 6: Final dry --------------------------------------------- */
    { "final_end",      "End Batch",           6,  1, HR_SEV_DESTRUCTIVE },
    { "final_more",     "More Dry Time",       6,  2, HR_SEV_BENIGN      },
    { "final_less",     "Less Dry Time",       6,  3, HR_SEV_CONFIRM     },

    /* --- Screen 7: Batch complete ---------------------------------------- */
    /*
     * The batch is already finished here, so nothing on this screen destroys a
     * run in progress - the choices are about how to finish. More Dry Time is
     * the conservative one (it adds two hours and returns to drying), so it is
     * the only benign entry. The rest each commit the machine to heat or end
     * the cycle, and all three are easy to regret from a phone.
     */
    { "done_defrost",   "Defrost",             7,  1, HR_SEV_CONFIRM     },
    { "done_more_dry",  "More Dry Time (+2h)", 7,  2, HR_SEV_BENIGN      },
    { "done_no_defrost","Finish, No Defrost",  7,  3, HR_SEV_CONFIRM     },
    { "done_warm_trays","Warm Trays",          7,  5, HR_SEV_CONFIRM     },

    /* --- Screen 43: Recipe configuration ---------------------------------- */
    /*
     * Only Cancel is a CLICK. Every other control on this screen edits recipe
     * values and is sent as a SENDCANDY frame carrying the whole recipe, not as
     * a button press - see PROTOCOL_NOTES. So this screen cannot be operated by
     * this table alone, and backing out is all we can currently offer.
     */
    { "cfg_cancel",     "Cancel",             43, 18, HR_SEV_BENIGN      },

    /* --- Screen 31: Custom recipe configuration --------------------------- */
    /* Cancel is button 26 here against 18 on screen 43 - the same control,
     * different number, on two screens that do the same job. */
    { "custom_cancel",  "Cancel",             31, 26, HR_SEV_BENIGN      },
};

/*
 * SEVERITY, and why these particular calls.
 *
 * The two "advance" actions are DESTRUCTIVE even though they sound like
 * navigation. Skipping out of pre-cool or freezing moves the batch past a step
 * the recipe asked for, on a process where the cost of being wrong is a ruined
 * 24-hour run rather than an error message. They are irreversible from the app.
 *
 * "More Dry Time" is BENIGN: extending drying is the conservative direction and
 * costs only time. "Less Dry Time" is CONFIRM for the same reason inverted -
 * it shortens the run toward an under-dried result.
 *
 * Starting a cycle is CONFIRM rather than DESTRUCTIVE: it commits the machine
 * for a day but destroys nothing, and it is reversible with End Batch.
 *
 * Note button numbers are NOT consistent between screens - End Batch is 4 on
 * Preparing and Freezing but 1 on Drying and Final Dry. That is exactly why
 * this table is keyed by screen and why a stale view has to be refused.
 */

#define N_ACTIONS (sizeof(k_actions) / sizeof(k_actions[0]))

size_t hr_control_count(void) { return N_ACTIONS; }

const hr_action_t *hr_control_at(size_t i)
{
    return (i < N_ACTIONS) ? &k_actions[i] : NULL;
}

const hr_action_t *hr_control_lookup(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < N_ACTIONS; i++) {
        if (strcmp(k_actions[i].name, name) == 0) {
            return &k_actions[i];
        }
    }
    return NULL;
}

size_t hr_control_for_screen(int screen, const hr_action_t **out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < N_ACTIONS; i++) {
        if (k_actions[i].screen != screen) {
            continue;
        }
        if (out != NULL && n < cap) {
            out[n] = &k_actions[i];
        }
        n++;
    }
    return (out != NULL && n > cap) ? cap : n;
}

hr_ctrl_result_t hr_control_check(const char *name, int believed_screen,
                                  int live_screen, bool confirmed,
                                  const hr_action_t **out_action)
{
    if (out_action != NULL) {
        *out_action = NULL;
    }

    const hr_action_t *a = hr_control_lookup(name);
    if (a == NULL) {
        return HR_CTRL_UNKNOWN_ACTION;
    }

    /*
     * No telemetry means we do not know what is on screen, and a CLICK is
     * meaningless - worse, dangerous - without that. Refuse rather than assume
     * the machine is where it was last seen.
     */
    if (live_screen < 0) {
        return HR_CTRL_NO_TELEMETRY;
    }

    /*
     * The stale-view check. The caller tapped while looking at
     * believed_screen; if the machine has moved on since, the button number
     * they chose now addresses a different control. This is the check that
     * makes remote control safe to expose at all.
     */
    if (believed_screen != live_screen) {
        return HR_CTRL_STALE_VIEW;
    }

    /* And the action must belong to the screen actually in front of us. */
    if (a->screen != live_screen) {
        return HR_CTRL_WRONG_SCREEN;
    }

    if (a->sev != HR_SEV_BENIGN && !confirmed) {
        return HR_CTRL_NEEDS_CONFIRM;
    }

    if (out_action != NULL) {
        *out_action = a;
    }
    return HR_CTRL_OK;
}

const char *hr_ctrl_result_str(hr_ctrl_result_t r)
{
    switch (r) {
    case HR_CTRL_OK:             return "ok";
    case HR_CTRL_UNKNOWN_ACTION: return "unknown action";
    case HR_CTRL_STALE_VIEW:     return "screen changed since you looked";
    case HR_CTRL_WRONG_SCREEN:   return "not available on this screen";
    case HR_CTRL_NEEDS_CONFIRM:  return "confirmation required";
    case HR_CTRL_NO_TELEMETRY:   return "no telemetry from the dryer";
    default:                     return "unknown";
    }
}

size_t hr_control_build_click(char *out, size_t cap, int screen, int button,
                              uint32_t counter, uint32_t session)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    int n = snprintf(out, cap, "CLICK %d %d %lu %lu", screen, button,
                     (unsigned long)counter, (unsigned long)session);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}
