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
    CHECK_INT(hr_control_check("start_candy", 1, 1, false, &a), HR_CTRL_NEEDS_CONFIRM);
    CHECK_INT(hr_control_check("start_custom", 1, 1, false, &a), HR_CTRL_NEEDS_CONFIRM);

    /* Opening settings does not. */
    CHECK_INT(hr_control_check("config", 1, 1, false, &a), HR_CTRL_OK);
    CHECK(a != NULL);
    CHECK_INT(a->button, 3);
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
    size_t n = hr_control_for_screen(1, buf, 8);
    CHECK_INT((int)n, 4);

    /* Screens whose buttons have never been captured must offer NOTHING.
     * An empty toolbar is correct; a guessed one presses unknown controls on
     * a running machine. */
    CHECK_INT((int)hr_control_for_screen(4, buf, 8), 0);
    CHECK_INT((int)hr_control_for_screen(5, buf, 8), 0);
    CHECK_INT((int)hr_control_for_screen(17, buf, 8), 0);
    CHECK_INT((int)hr_control_for_screen(0, buf, 8), 0);
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
    test_every_action_is_self_consistent();
    return TEST_REPORT();
}
