#include "hr_history.h"
#include "test_util.h"

static void test_escape_passes_plain_text(void)
{
    TEST_CASE("escape passes plain text");
    char out[64];
    CHECK_INT(hr_json_escape("STAT,1,DRY", out, sizeof(out)), 10);
    CHECK_STR(out, "STAT,1,DRY");
}

static void test_escape_quotes_and_backslashes(void)
{
    TEST_CASE("escape quotes and backslashes");
    char out[64];
    hr_json_escape("a\"b\\c", out, sizeof(out));
    CHECK_STR(out, "a\\\"b\\\\c");
}

static void test_escape_drops_control_chars(void)
{
    TEST_CASE("escape drops control chars");
    char out[64];
    hr_json_escape("a\r\n\tb", out, sizeof(out));
    CHECK_STR(out, "ab");
}

static void test_escape_respects_capacity(void)
{
    TEST_CASE("escape respects capacity");
    char out[4];
    /* room for 3 chars + NUL; must not overflow */
    hr_json_escape("ABCDEFG", out, sizeof(out));
    CHECK(strlen(out) <= 3);
}

static void test_entry_json_serialises_fields(void)
{
    TEST_CASE("entry json serialises fields");
    hr_history_t h;
    hr_history_init(&h);
    hr_frame_t f;
    hr_frame_parse("STAT,1,DRY,20\r", &f);
    hr_history_add(&h, &f, 1234);

    hr_hist_entry_t out[1];
    hr_history_since(&h, 0, out, 1);

    char json[256];
    size_t n = hr_hist_entry_json(&out[0], json, sizeof(json));
    CHECK(n > 0);
    CHECK_STR(json,
              "{\"seq\":1,\"t\":1234,\"verb\":\"STAT\",\"body\":\"STAT,1,DRY,20\",\"n\":3}");
}

int main(void)
{
    test_escape_passes_plain_text();
    test_escape_quotes_and_backslashes();
    test_escape_drops_control_chars();
    test_escape_respects_capacity();
    test_entry_json_serialises_fields();
    return TEST_REPORT();
}
