/*
 * hr_temp: the F -> C conversion the UI, /api/state and MQTT all share, and
 * the rule that picks which unit to present in.
 */
#include "hr_temp.h"
#include "test_util.h"

int main(void)
{
    TEST_CASE("F -> tenths of C, rounded half away from zero");
    CHECK_INT(hr_temp_f_to_c_x10(32), 0);
    CHECK_INT(hr_temp_f_to_c_x10(212), 1000);
    CHECK_INT(hr_temp_f_to_c_x10(-40), -400);
    CHECK_INT(hr_temp_f_to_c_x10(24), -44);   /* -4.44 -> -4.4 */
    CHECK_INT(hr_temp_f_to_c_x10(23), -50);   /* -5.0 */
    CHECK_INT(hr_temp_f_to_c_x10(69), 206);   /* 20.56 -> 20.6 */
    CHECK_INT(hr_temp_f_to_c_x10(124), 511);  /* 51.11 -> 51.1 */
    CHECK_INT(hr_temp_f_to_c_x10(-18), -278); /* -27.78 -> -27.8 */
    CHECK_INT(hr_temp_f_to_c_x10(31), -6);    /* -0.56 -> -0.6 */
    CHECK_INT(hr_temp_f_to_c_x10(33), 6);     /* 0.56 -> 0.6 */

    TEST_CASE("F -> whole C");
    CHECK_INT(hr_temp_f_to_c(32), 0);
    CHECK_INT(hr_temp_f_to_c(24), -4);    /* -4.44 */
    CHECK_INT(hr_temp_f_to_c(23), -5);
    CHECK_INT(hr_temp_f_to_c(-18), -28);  /* -27.78 */
    CHECK_INT(hr_temp_f_to_c(124), 51);
    CHECK_INT(hr_temp_f_to_c(69), 21);    /* 20.56 */
    CHECK_INT(hr_temp_f_to_c(41), 5);     /* exactly 5 */
    CHECK_INT(hr_temp_f_to_c(50), 10);

    TEST_CASE("double view");
    CHECK(hr_temp_from_f(24, HR_TEMP_F) == 24.0);
    CHECK(hr_temp_from_f(24, HR_TEMP_C) == -4.4);
    CHECK(hr_temp_from_f(212, HR_TEMP_C) == 100.0);

    TEST_CASE("JSON number formatting");
    char buf[16];
    CHECK_INT(hr_temp_fmt_num(24, HR_TEMP_F, buf, sizeof(buf)), 2);
    CHECK_STR(buf, "24");
    CHECK(hr_temp_fmt_num(-18, HR_TEMP_F, buf, sizeof(buf)) > 0);
    CHECK_STR(buf, "-18");
    CHECK(hr_temp_fmt_num(24, HR_TEMP_C, buf, sizeof(buf)) > 0);
    CHECK_STR(buf, "-4.4");
    CHECK(hr_temp_fmt_num(31, HR_TEMP_C, buf, sizeof(buf)) > 0);
    CHECK_STR(buf, "-0.6");   /* the minus survives a zero whole part */
    CHECK(hr_temp_fmt_num(32, HR_TEMP_C, buf, sizeof(buf)) > 0);
    CHECK_STR(buf, "0.0");
    CHECK(hr_temp_fmt_num(212, HR_TEMP_C, buf, sizeof(buf)) > 0);
    CHECK_STR(buf, "100.0");
    CHECK(hr_temp_fmt_num(124, HR_TEMP_C, buf, sizeof(buf)) > 0);
    CHECK_STR(buf, "51.1");
    /* Too small a buffer: nothing written, empty string. */
    char tiny[3];
    CHECK_INT(hr_temp_fmt_num(124, HR_TEMP_C, tiny, sizeof(tiny)), 0);
    CHECK_STR(tiny, "");
    CHECK_INT(hr_temp_fmt_num(1, HR_TEMP_F, NULL, 8), 0);

    TEST_CASE("resolve: explicit preference always wins");
    CHECK_INT(hr_temp_resolve(HR_TEMP_PREF_F, HR_TEMP_DRYER_UNKNOWN, HR_TEMP_C),
              HR_TEMP_F);
    CHECK_INT(hr_temp_resolve(HR_TEMP_PREF_C, HR_TEMP_DRYER_UNKNOWN, HR_TEMP_F),
              HR_TEMP_C);
    CHECK_INT(hr_temp_resolve(HR_TEMP_PREF_F, HR_TEMP_C, HR_TEMP_C), HR_TEMP_F);
    CHECK_INT(hr_temp_resolve(HR_TEMP_PREF_C, HR_TEMP_F, HR_TEMP_F), HR_TEMP_C);

    TEST_CASE("resolve: auto follows the dryer when it says, else fallback");
    CHECK_INT(hr_temp_resolve(HR_TEMP_PREF_AUTO, HR_TEMP_DRYER_UNKNOWN,
                              HR_TEMP_F), HR_TEMP_F);
    CHECK_INT(hr_temp_resolve(HR_TEMP_PREF_AUTO, HR_TEMP_DRYER_UNKNOWN,
                              HR_TEMP_C), HR_TEMP_C);
    CHECK_INT(hr_temp_resolve(HR_TEMP_PREF_AUTO, HR_TEMP_C, HR_TEMP_F),
              HR_TEMP_C);
    CHECK_INT(hr_temp_resolve(HR_TEMP_PREF_AUTO, HR_TEMP_F, HR_TEMP_C),
              HR_TEMP_F);
    /* Garbage from a future decoder is not a unit. */
    CHECK_INT(hr_temp_resolve(HR_TEMP_PREF_AUTO, 7, HR_TEMP_F), HR_TEMP_F);

    TEST_CASE("preference spelling: parse and print");
    hr_temp_pref_t p = HR_TEMP_PREF_AUTO;
    CHECK(hr_temp_pref_parse("c", &p));
    CHECK_INT(p, HR_TEMP_PREF_C);
    CHECK(hr_temp_pref_parse("F", &p));
    CHECK_INT(p, HR_TEMP_PREF_F);
    CHECK(hr_temp_pref_parse("Celsius", &p));
    CHECK_INT(p, HR_TEMP_PREF_C);
    CHECK(hr_temp_pref_parse("metric", &p));
    CHECK_INT(p, HR_TEMP_PREF_C);
    CHECK(hr_temp_pref_parse("imperial", &p));
    CHECK_INT(p, HR_TEMP_PREF_F);
    CHECK(hr_temp_pref_parse("AUTO", &p));
    CHECK_INT(p, HR_TEMP_PREF_AUTO);
    p = HR_TEMP_PREF_C;
    CHECK(!hr_temp_pref_parse("kelvin", &p));
    CHECK(!hr_temp_pref_parse("", &p));
    CHECK(!hr_temp_pref_parse("cc", &p));
    CHECK(!hr_temp_pref_parse(NULL, &p));
    CHECK_INT(p, HR_TEMP_PREF_C); /* untouched on failure */
    CHECK_STR(hr_temp_pref_str(HR_TEMP_PREF_F), "f");
    CHECK_STR(hr_temp_pref_str(HR_TEMP_PREF_C), "c");
    CHECK_STR(hr_temp_pref_str(HR_TEMP_PREF_AUTO), "auto");
    CHECK_STR(hr_temp_unit_letter(HR_TEMP_F), "F");
    CHECK_STR(hr_temp_unit_letter(HR_TEMP_C), "C");

    return TEST_REPORT();
}
