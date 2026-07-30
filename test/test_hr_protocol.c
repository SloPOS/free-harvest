#include "hr_protocol.h"
#include "test_util.h"

static void test_parses_verb_and_fields(void)
{
    TEST_CASE("parses verb and fields");
    hr_frame_t f;
    CHECK(hr_frame_parse("STAT,1,ABC,42\r", &f));
    CHECK_STR(f.verb, "STAT");
    CHECK_INT(f.nfields, 3);
    CHECK_STR(hr_frame_field(&f, 0), "1");
    CHECK_STR(hr_frame_field(&f, 1), "ABC");
    CHECK_STR(hr_frame_field(&f, 2), "42");
}

static void test_preserves_empty_and_trailing_fields(void)
{
    /* The dryer emits "BATSUM,%d,," - empty fields carry meaning. */
    TEST_CASE("preserves empty and trailing fields");
    hr_frame_t f;
    CHECK(hr_frame_parse("BATSUM,3,,\r", &f));
    CHECK_STR(f.verb, "BATSUM");
    CHECK_INT(f.nfields, 3);
    CHECK_STR(hr_frame_field(&f, 0), "3");
    CHECK_STR(hr_frame_field(&f, 1), "");
    CHECK_STR(hr_frame_field(&f, 2), "");
}

static void test_parses_verb_only_frame(void)
{
    TEST_CASE("parses verb-only frame");
    hr_frame_t f;
    CHECK(hr_frame_parse("REQINFO\r", &f));
    CHECK_STR(f.verb, "REQINFO");
    CHECK_INT(f.nfields, 0);
}

static void test_rejects_empty_line(void)
{
    TEST_CASE("rejects empty line");
    hr_frame_t f;
    CHECK(!hr_frame_parse("", &f));
    CHECK(!hr_frame_parse("\r", &f));
}

static void test_field_int_conversion(void)
{
    TEST_CASE("field int conversion");
    hr_frame_t f;
    CHECK(hr_frame_parse("NTFY,7,1234,hello,\r", &f));
    CHECK_INT(hr_frame_field_int(&f, 0, -1), 7);
    CHECK_INT(hr_frame_field_int(&f, 1, -1), 1234);
    /* non-numeric and out-of-range fall back to the default */
    CHECK_INT(hr_frame_field_int(&f, 2, -1), -1);
    CHECK_INT(hr_frame_field_int(&f, 99, -1), -1);
}

static void test_tostring_rebuilds_original_body(void)
{
    TEST_CASE("tostring rebuilds original body");
    hr_frame_t f;
    char out[HR_MAX_FRAME];

    CHECK(hr_frame_parse("STAT,1,DRY,20,-30,a,b,\r", &f));
    CHECK_INT(hr_frame_tostring(&f, out, sizeof(out)), 22);
    CHECK_STR(out, "STAT,1,DRY,20,-30,a,b,");
}

static void test_tostring_handles_verb_only_and_small_buffer(void)
{
    TEST_CASE("tostring handles verb-only and small buffer");
    hr_frame_t f;
    char out[8];

    CHECK(hr_frame_parse("BEEP\r", &f));
    CHECK_INT(hr_frame_tostring(&f, out, sizeof(out)), 4);
    CHECK_STR(out, "BEEP");

    CHECK(hr_frame_parse("STAT,111111,222222\r", &f));
    CHECK_INT(hr_frame_tostring(&f, out, sizeof(out)), 0);
}

int main(void)
{
    test_tostring_rebuilds_original_body();
    test_tostring_handles_verb_only_and_small_buffer();
    test_parses_verb_and_fields();
    test_preserves_empty_and_trailing_fields();
    test_parses_verb_only_frame();
    test_rejects_empty_line();
    test_field_int_conversion();
    return TEST_REPORT();
}
