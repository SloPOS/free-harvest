#include "hr_protocol.h"
#include "test_util.h"

static void test_builds_verb_only_frame(void)
{
    TEST_CASE("builds verb-only frame");
    hr_builder_t b;
    size_t len = 0;
    hr_build_begin(&b, "REQSTAT");
    const char *out = hr_build_finish(&b, &len);

    CHECK_STR(out, "REQSTAT\r");
    CHECK_INT(len, 8);
}

static void test_builds_frame_with_mixed_fields(void)
{
    TEST_CASE("builds frame with mixed fields");
    hr_builder_t b;
    hr_build_begin(&b, "STAT");
    hr_build_int(&b, 1);
    hr_build_str(&b, "abc");
    hr_build_int(&b, 42);

    CHECK_STR(hr_build_finish(&b, NULL), "STAT,1,abc,42\r");
}

static void test_builds_trailing_empty_field(void)
{
    /* Mirrors the dryer's own "BATSUM,%d,," - two empty trailing fields. */
    TEST_CASE("builds trailing empty field");
    hr_builder_t b;
    hr_build_begin(&b, "BATSUM");
    hr_build_int(&b, 3);
    hr_build_str(&b, "");
    hr_build_str(&b, "");

    CHECK_STR(hr_build_finish(&b, NULL), "BATSUM,3,,\r");
}

static void test_built_frame_round_trips_through_parser(void)
{
    TEST_CASE("built frame round-trips through parser");
    hr_builder_t b;
    hr_build_begin(&b, "GOTIT");
    hr_build_str(&b, "SN12345");
    hr_build_int(&b, 7);

    const char *wire = hr_build_finish(&b, NULL);
    hr_frame_t f;
    CHECK(hr_frame_parse(wire, &f));
    CHECK_STR(f.verb, "GOTIT");
    CHECK_INT(f.nfields, 2);
    CHECK_STR(hr_frame_field(&f, 0), "SN12345");
    CHECK_INT(hr_frame_field_int(&f, 1, -1), 7);
}

static void test_overflow_is_reported_not_truncated(void)
{
    TEST_CASE("overflow is reported not truncated");
    hr_builder_t b;
    char big[HR_MAX_FRAME];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    hr_build_begin(&b, "STAT");
    hr_build_str(&b, big);

    CHECK(!b.ok);
    CHECK(hr_build_finish(&b, NULL) == NULL);
}

int main(void)
{
    test_builds_verb_only_frame();
    test_builds_frame_with_mixed_fields();
    test_builds_trailing_empty_field();
    test_built_frame_round_trips_through_parser();
    test_overflow_is_reported_not_truncated();
    return TEST_REPORT();
}
