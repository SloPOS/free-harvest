#include "hr_control.h"

#include <stdio.h>
#include <string.h>

/*
 * THE ACTION TABLE
 *
 * Only screen 1 (Ready) is populated, because only screen 1 has been MEASURED.
 * The four entries below come from a capture of the genuine app driving a
 * simulated dryer on 2026-08-21:
 *
 *     CLICK 1 10 54779 175300   Start
 *     CLICK 1  9 54780 175300   Custom
 *     CLICK 1  8 54781 175300   Candy
 *     CLICK 1  3 54782 175300   Config
 *
 * Every other screen's buttons are UNKNOWN and this table stays empty for them
 * on purpose. Guessing a button number is guessing which control gets pressed
 * on a running machine, and the whole point of this layer is to not do that.
 * Add rows only from captures.
 */
static const hr_action_t k_actions[] = {
    /* name          label            screen button severity */
    { "config",      "Settings",           1,  3, HR_SEV_BENIGN     },
    { "start_candy", "Start (Candy)",      1,  8, HR_SEV_CONFIRM    },
    { "start_custom","Start (Custom)",     1,  9, HR_SEV_CONFIRM    },
    { "start_auto",  "Start (Auto)",       1, 10, HR_SEV_CONFIRM    },
};

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
