#include "hr_control.h"
#include "test_util.h"

#include <string.h>

static void test_lookup(void)
{
    const hr_action_t *a = hr_control_lookup("start_auto");
    CHECK(a != NULL);
    CHECK_INT(a->screen, 1);
    CHECK_INT(a->button, 10);
    CHECK_INT(a->sev, HR_SEV_CONFIRM);

    CHECK(hr_control_lookup("nope") == NULL);
    CHECK(hr_control_lookup("") == NULL);
    CHECK(hr_control_lookup(NULL) == NULL);
}

static void test_frame_matches_capture(void)
{
    /* The exact bytes the genuine app sent for Start. If this ever stops
     * matching, the format drifted and every other test here is worthless. */
    char buf[64];
    size_t n = hr_control_build_click(buf, sizeof(buf), 1, 10, 54779, 175300);
    CHECK(n > 0);
    CHECK(strcmp(buf, "CLICK 1 10 54779 175300") == 0);

    /* Too small a buffer must fail closed, not truncate into a valid-looking
     * frame addressing some other button. */
    char tiny[8];
    CHECK_INT((int)hr_control_build_click(tiny, sizeof(tiny), 1, 10, 54779, 175300), 0);
    CHECK_INT((int)tiny[0], 0);
    CHECK_INT((int)hr_control_build_click(NULL, 10, 1, 10, 1, 1), 0);
}

static void test_stale_view_is_refused(void)
{
    const hr_action_t *a = NULL;

    /* Looking at Ready, machine is on Ready: fine. */
    CHECK_INT(hr_control_check("start_auto", 1, 1, true, &a), HR_CTRL_OK);
    CHECK(a != NULL);

    /*
     * Looking at Ready, but the machine has moved to Freezing. Button 10 on
     * screen 4 is some other control entirely. THIS is the case the layer
     * exists for.
     */
    CHECK_INT(hr_control_check("start_auto", 1, 4, true, &a), HR_CTRL_STALE_VIEW);
    CHECK(a == NULL);

    /* No telemetry: never assume the machine is where we last saw it. */
    CHECK_INT(hr_control_check("start_auto", 1, -1, true, &a), HR_CTRL_NO_TELEMETRY);
    CHECK(a == NULL);
}

static void test_confirmation_is_required(void)
{
    const hr_action_t *a = NULL;

    /* Starting a 24-hour cycle needs an explicit yes. */
    CHECK_INT(hr_control_check("start_auto", 1, 1, false, &a), HR_CTRL_NEEDS_CONFIRM);
    CHECK(a == NULL);
    CHECK_INT(hr_control_check("candy_setup", 1, 1, false, &a), HR_CTRL_NEEDS_CONFIRM);
    CHECK_INT(hr_control_check("custom_setup", 1, 1, false, &a), HR_CTRL_NEEDS_CONFIRM);

    /* Nothing on the Ready screen is benign any more: the one benign button
     * was Settings, and Settings kills USB comms until someone walks to the
     * machine. It is no longer offered at all. */
    CHECK(hr_control_lookup("config") == NULL);
    CHECK_INT(hr_control_check("config", 1, 1, true, &a), HR_CTRL_UNKNOWN_ACTION);
}

static void test_unknown_action(void)
{
    const hr_action_t *a = NULL;
    CHECK_INT(hr_control_check("rm_rf", 1, 1, true, &a), HR_CTRL_UNKNOWN_ACTION);
    CHECK(a == NULL);
}

static void test_screen_listing(void)
{
    const hr_action_t *buf[8];
    CHECK_INT((int)hr_control_for_screen(1,  buf, 8), 3);  /* Ready      */
    CHECK_INT((int)hr_control_for_screen(17, buf, 8), 2);  /* Preparing  */
    CHECK_INT((int)hr_control_for_screen(4,  buf, 8), 2);  /* Freezing   */
    CHECK_INT((int)hr_control_for_screen(5,  buf, 8), 1);  /* Drying     */
    CHECK_INT((int)hr_control_for_screen(6,  buf, 8), 3);  /* Final dry  */

    CHECK_INT((int)hr_control_for_screen(2,  buf, 8), 2);  /* Load trays */
    CHECK_INT((int)hr_control_for_screen(7,  buf, 8), 4);  /* Complete   */
    CHECK_INT((int)hr_control_for_screen(43, buf, 8), 1);  /* Candy cfg  */
    CHECK_INT((int)hr_control_for_screen(31, buf, 8), 1);  /* Custom cfg */

    /* Cancel is button 18 on one config screen and 26 on the other. Same
     * control, same job, different number - the table must not merge them. */
    CHECK_INT(hr_control_lookup("cfg_cancel")->button,    18);
    CHECK_INT(hr_control_lookup("custom_cancel")->button, 26);

    /* Screens whose buttons have never been captured must offer NOTHING.
     * An empty toolbar is correct; a guessed one presses unknown controls on
     * a running machine. Screen 2 is "Starting batch", which is exactly where
     * the machine stranded during the ADV probe - do not invent buttons for it. */
    CHECK_INT((int)hr_control_for_screen(15, buf, 8), 0);
    CHECK_INT((int)hr_control_for_screen(0,  buf, 8), 0);

    /* A cap smaller than the match count must not overrun. */
    const hr_action_t *two[2];
    CHECK_INT((int)hr_control_for_screen(1, two, 2), 2);
}

static void test_end_batch_button_differs_per_screen(void)
{
    /*
     * This is the whole reason the table is keyed by screen. "End Batch" is
     * button 4 on Preparing and Freezing, but button 1 on Drying and Final Dry.
     * Send a screen-17 End Batch against a live screen 5 and button 4 would
     * address something else entirely.
     */
    CHECK_INT(hr_control_lookup("prep_end")->button,   4);
    CHECK_INT(hr_control_lookup("freeze_end")->button, 4);
    CHECK_INT(hr_control_lookup("dry_end")->button,    1);
    CHECK_INT(hr_control_lookup("final_end")->button,  1);

    /* Every End Batch is destructive, on every screen. */
    CHECK_INT(hr_control_lookup("prep_end")->sev,   HR_SEV_DESTRUCTIVE);
    CHECK_INT(hr_control_lookup("freeze_end")->sev, HR_SEV_DESTRUCTIVE);
    CHECK_INT(hr_control_lookup("dry_end")->sev,    HR_SEV_DESTRUCTIVE);
    CHECK_INT(hr_control_lookup("final_end")->sev,  HR_SEV_DESTRUCTIVE);

    /* Skipping a step is destructive too - it advances the batch past a stage
     * the recipe asked for, and cannot be undone from the app. */
    CHECK_INT(hr_control_lookup("prep_advance")->sev,   HR_SEV_DESTRUCTIVE);
    CHECK_INT(hr_control_lookup("freeze_advance")->sev, HR_SEV_DESTRUCTIVE);

    /* The concrete stale-view case: user tapped End Batch on the Preparing
     * screen, machine has since reached Drying. Must refuse. */
    const hr_action_t *a = NULL;
    CHECK_INT(hr_control_check("prep_end", 17, 5, true, &a), HR_CTRL_STALE_VIEW);
    CHECK(a == NULL);
}

static void test_drying_direction_asymmetry(void)
{
    /* More dry time is the conservative direction and costs only time.
     * Less dry time moves toward an under-dried batch, so it asks first. */
    CHECK_INT(hr_control_lookup("final_more")->sev, HR_SEV_BENIGN);
    CHECK_INT(hr_control_lookup("final_less")->sev, HR_SEV_CONFIRM);

    const hr_action_t *a = NULL;
    CHECK_INT(hr_control_check("final_more", 6, 6, false, &a), HR_CTRL_OK);
    CHECK_INT(hr_control_check("final_less", 6, 6, false, &a), HR_CTRL_NEEDS_CONFIRM);
}

static void test_action_names_are_unique(void)
{
    for (size_t i = 0; i < hr_control_count(); i++) {
        for (size_t j = i + 1; j < hr_control_count(); j++) {
            CHECK(strcmp(hr_control_at(i)->name, hr_control_at(j)->name) != 0);
        }
    }
}

static void test_every_action_is_self_consistent(void)
{
    /* Guards the table itself: an action must be reachable on its own screen,
     * and must not be silently unreachable through the check path. */
    for (size_t i = 0; i < hr_control_count(); i++) {
        const hr_action_t *a = hr_control_at(i);
        CHECK(a != NULL);
        CHECK(a->name != NULL && a->name[0] != '\0');
        CHECK(a->label != NULL && a->label[0] != '\0');
        CHECK(a->screen > 0);
        CHECK(a->button > 0);
        CHECK(hr_control_lookup(a->name) == a);

        const hr_action_t *got = NULL;
        CHECK_INT(hr_control_check(a->name, a->screen, a->screen, true, &got),
                  HR_CTRL_OK);
        CHECK(got == a);
    }
}

int main(void)
{
    test_lookup();
    test_frame_matches_capture();
    test_stale_view_is_refused();
    test_confirmation_is_required();
    test_unknown_action();
    test_screen_listing();
    test_end_batch_button_differs_per_screen();
    test_drying_direction_asymmetry();
    test_action_names_are_unique();
    test_every_action_is_self_consistent();
    return TEST_REPORT();
}
