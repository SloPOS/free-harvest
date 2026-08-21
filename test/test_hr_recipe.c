#include "hr_recipe.h"
#include "test_util.h"

#include <string.h>

/* The two payloads captured from the genuine app, field for field. */
static hr_recipe_t candy_fixture(void)
{
    hr_recipe_t r;
    memset(&r, 0, sizeof(r));
    r.used = true;
    r.family = HR_FAM_CANDY;
    strcpy(r.name, "CANDY");
    int32_t v[HR_CANDY_NUMERICS] = {4, 70, 140, 150, 160, 300, 7200, 300};
    memcpy(r.num, v, sizeof(v));
    r.nnum = HR_CANDY_NUMERICS;
    return r;
}

static hr_recipe_t custom_fixture(void)
{
    hr_recipe_t r;
    memset(&r, 0, sizeof(r));
    r.used = true;
    r.family = HR_FAM_CUSTOM;
    strcpy(r.name, "CUSTOM");
    int32_t v[HR_CUSTOM_NUMERICS] = {5, -10, 31500, 150, 7200, 500, 1, 100,
                                     54000, 0};
    memcpy(r.num, v, sizeof(v));
    r.nnum = HR_CUSTOM_NUMERICS;
    return r;
}

static void test_frames_match_the_captures(void)
{
    char buf[320];
    hr_recipe_t c = candy_fixture();

    /* Captured: SENDCANDY "4,70,140,150,160,300,7200,300,CANDY,0," 35890 */
    CHECK(hr_recipe_build(&c, false, 35890, buf, sizeof(buf)) > 0);
    CHECK_STR(buf, "SENDCANDY \"4,70,140,150,160,300,7200,300,CANDY,0,\" 35890");

    hr_recipe_t u = custom_fixture();
    /* Captured: SENDCUSTOM "5,-10,31500,150,7200,500,1,100,54000,0,CUSTOM,0," 17633 */
    CHECK(hr_recipe_build(&u, false, 17633, buf, sizeof(buf)) > 0);
    CHECK_STR(buf,
        "SENDCUSTOM \"5,-10,31500,150,7200,500,1,100,54000,0,CUSTOM,0,\" 17633");

    /* Start flag set. Captured for Custom's Start button at counter 17637. */
    hr_recipe_t s = custom_fixture();
    s.num[HR_CUSTOM_EXTRA_FREEZE_S] = 0;
    s.num[HR_CUSTOM_DRY_TEMP] = 120;
    CHECK(hr_recipe_build(&s, true, 17637, buf, sizeof(buf)) > 0);
    CHECK_STR(buf,
        "SENDCUSTOM \"5,-10,0,120,7200,500,1,100,54000,0,CUSTOM,1,\" 17637");
}

static void test_name_cannot_smuggle_a_verb(void)
{
    /*
     * The dryer matches verbs with strstr over the whole line and tests ADD,
     * DIR, DEL and friends BEFORE SENDCANDY. An innocuous recipe name in
     * capitals can therefore be routed to a different command entirely.
     */
    CHECK_INT(hr_recipe_check_name("ADDED SUGAR"), HR_RECIPE_NAME_HAS_VERB);
    CHECK_INT(hr_recipe_check_name("RED DIRT"),    HR_RECIPE_NAME_HAS_VERB);
    CHECK_INT(hr_recipe_check_name("DELICATE"),    HR_RECIPE_NAME_HAS_VERB);
    CHECK_INT(hr_recipe_check_name("XWING"),       HR_RECIPE_NAME_HAS_VERB);
    CHECK_INT(hr_recipe_check_name("BEEF COPY"),   HR_RECIPE_NAME_HAS_VERB);

    /* The dryer's compare is case-sensitive, so lowercase is genuinely safe
     * and must not be rejected - being over-strict here would block most
     * ordinary names. */
    CHECK_INT(hr_recipe_check_name("Added Sugar"), HR_RECIPE_OK);
    CHECK_INT(hr_recipe_check_name("Red dirt"),    HR_RECIPE_OK);
    CHECK_INT(hr_recipe_check_name("Strawberries"), HR_RECIPE_OK);
    CHECK_INT(hr_recipe_check_name("CANDY"),       HR_RECIPE_OK);
}

static void test_name_cannot_break_the_payload(void)
{
    /* A comma does not corrupt the name - it shifts every field after it, so
     * the dryer reads a different recipe and reports no error at all. */
    CHECK_INT(hr_recipe_check_name("Straw,berry"), HR_RECIPE_BAD_NAME);
    CHECK_INT(hr_recipe_check_name("say \"hi\""),  HR_RECIPE_BAD_NAME);
    CHECK_INT(hr_recipe_check_name(""),            HR_RECIPE_BAD_NAME);
    CHECK_INT(hr_recipe_check_name(NULL),          HR_RECIPE_BAD_NAME);
    CHECK_INT(hr_recipe_check_name("012345678901234567890123456789"),
              HR_RECIPE_BAD_NAME);

    /* And a rejected name must never reach the wire. */
    char buf[320];
    hr_recipe_t r = candy_fixture();
    strcpy(r.name, "ADDED");
    CHECK_INT((int)hr_recipe_build(&r, false, 1, buf, sizeof(buf)), 0);
    CHECK_INT((int)buf[0], 0);
}

static void test_validation(void)
{
    hr_recipe_t r = candy_fixture();
    CHECK_INT(hr_recipe_validate(&r), HR_RECIPE_OK);

    /* Family byte and family must agree, or the verb says one thing and the
     * payload another. */
    r = candy_fixture();
    r.num[0] = 5;
    CHECK_INT(hr_recipe_validate(&r), HR_RECIPE_BAD_FIELDS);

    /* Wrong field count for the family. */
    r = candy_fixture();
    r.nnum = HR_CUSTOM_NUMERICS;
    CHECK_INT(hr_recipe_validate(&r), HR_RECIPE_BAD_FIELDS);

    /* A minutes/seconds mix-up is the mistake most likely to happen, since
     * STAT reports minutes and this frame takes seconds. 7200 MINUTES is 120
     * hours and must not pass. */
    r = candy_fixture();
    r.num[HR_CANDY_DRY_S] = 7200 * 60;
    CHECK_INT(hr_recipe_validate(&r), HR_RECIPE_BAD_FIELDS);

    /* Nonsense temperature. */
    r = candy_fixture();
    r.num[HR_CANDY_DRY_TEMP] = 5000;
    CHECK_INT(hr_recipe_validate(&r), HR_RECIPE_BAD_FIELDS);

    /* Negative freeze temps are normal and must pass. */
    hr_recipe_t u = custom_fixture();
    u.num[HR_CUSTOM_FREEZE_TEMP] = -40;
    CHECK_INT(hr_recipe_validate(&u), HR_RECIPE_OK);

    r.family = (hr_family_t)99;
    CHECK_INT(hr_recipe_validate(&r), HR_RECIPE_BAD_FAMILY);
    CHECK_INT((int)hr_recipe_numerics((hr_family_t)99), 0);
}

static void test_short_buffer_yields_nothing(void)
{
    /* A truncated payload would still parse - it would just be a different,
     * plausible-looking recipe. Failing closed is the only safe option. */
    char tiny[20];
    hr_recipe_t r = candy_fixture();
    CHECK_INT((int)hr_recipe_build(&r, false, 1, tiny, sizeof(tiny)), 0);
    CHECK_INT((int)tiny[0], 0);
    CHECK_INT((int)hr_recipe_build(&r, false, 1, NULL, 100), 0);
}

static void test_extra_dry_is_learned_from_the_screen(void)
{
    hr_dry_tracker_t t;
    hr_dry_reset(&t);

    /* A normal run that never needed more drying. */
    int normal[] = {1, 17, 2, 4, 5, 6, 7, 1};
    for (size_t i = 0; i < sizeof(normal) / sizeof(normal[0]); i++) {
        hr_dry_observe(&t, normal[i]);
    }
    CHECK_INT((int)hr_dry_extra_s(&t), 0);

    /* Now one where drying was extended twice: complete -> drying, twice. */
    hr_dry_reset(&t);
    int extended[] = {5, 6, 7, 6, 7, 5, 7, 1};
    for (size_t i = 0; i < sizeof(extended) / sizeof(extended[0]); i++) {
        hr_dry_observe(&t, extended[i]);
    }
    CHECK_INT((int)hr_dry_extra_s(&t), 2 * 2 * 3600);

    /* Moving forward INTO complete must never count - only backwards out. */
    hr_dry_reset(&t);
    hr_dry_observe(&t, 5);
    hr_dry_observe(&t, 6);
    hr_dry_observe(&t, 7);
    CHECK_INT((int)hr_dry_extra_s(&t), 0);

    hr_dry_reset(&t);
    hr_dry_observe(&t, -1);          /* no telemetry must not corrupt state */
    CHECK_INT((int)hr_dry_extra_s(&t), 0);
    hr_dry_observe(NULL, 5);
    CHECK_INT((int)hr_dry_extra_s(NULL), 0);
}

static void test_suggestion(void)
{
    hr_recipe_t r = candy_fixture();      /* dry time 7200 = 2h */
    hr_dry_tracker_t t;
    hr_dry_reset(&t);

    /* Nothing added: suggest what it already has, not zero. */
    CHECK_INT((int)hr_recipe_suggested_dry_s(&r, &t), 7200);

    /* One extension: two hours more next time. */
    hr_dry_observe(&t, 7);
    hr_dry_observe(&t, 6);
    CHECK_INT((int)hr_recipe_suggested_dry_s(&r, &t), 7200 + 7200);

    /* Custom's drying is governed by fields we have not identified, so there
     * is nothing honest to suggest for it. */
    hr_recipe_t u = custom_fixture();
    CHECK_INT((int)hr_recipe_suggested_dry_s(&u, &t), 0);
    CHECK_INT((int)hr_recipe_suggested_dry_s(NULL, &t), 0);

    /* A suggestion must stay inside the range validate() will accept, or the
     * app would offer a value it then refuses to save. */
    hr_dry_tracker_t many;
    hr_dry_reset(&many);
    for (int i = 0; i < 40; i++) {
        hr_dry_observe(&many, 7);
        hr_dry_observe(&many, 6);
    }
    int32_t s = hr_recipe_suggested_dry_s(&r, &many);
    CHECK(s <= 24 * 3600);
    hr_recipe_t applied = candy_fixture();
    applied.num[HR_CANDY_DRY_S] = s;
    CHECK_INT(hr_recipe_validate(&applied), HR_RECIPE_OK);
}

int main(void)
{
    test_frames_match_the_captures();
    test_name_cannot_smuggle_a_verb();
    test_name_cannot_break_the_payload();
    test_validation();
    test_short_buffer_yields_nothing();
    test_extra_dry_is_learned_from_the_screen();
    test_suggestion();
    return TEST_REPORT();
}
