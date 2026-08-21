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

    CHECK_STR(hr_build_finish(&b, NULL), "STAT 1 abc 42\r");
}

static void test_builds_trailing_empty_field(void)
{
    /* Empty trailing fields still occupy their slot, as spaces. */
    TEST_CASE("builds trailing empty field");
    hr_builder_t b;
    hr_build_begin(&b, "BATSUM");
    hr_build_int(&b, 3);
    hr_build_str(&b, "");
    hr_build_str(&b, "");

    CHECK_STR(hr_build_finish(&b, NULL), "BATSUM 3  \r");
}

static void test_outbound_framing_is_not_inbound_framing(void)
{
    /*
     * The protocol is ASYMMETRIC and this test exists to pin that down.
     *
     * A genuine HarvestRight adapter sends space-separated frames
     * ("STATE 1 0", "UNIQUE lH"), while the dryer sends comma-separated
     * ones ("STAT,1,0,..."). So a frame we BUILD deliberately does not
     * parse with hr_frame_parse(), which handles the inbound direction.
     *
     * An earlier version of this test asserted a round trip, which looked
     * reasonable and quietly encoded the wrong assumption: that both
     * directions share a framing. They do not.
     */
    TEST_CASE("outbound framing differs from inbound");
    hr_builder_t b;
    hr_build_begin(&b, "GOTIT");
    hr_build_str(&b, "SN12345");
    hr_build_int(&b, 7);

    const char *wire = hr_build_finish(&b, NULL);
    CHECK_STR(wire, "GOTIT SN12345 7\r");

    /* Parsed as INBOUND it is one comma-free token - verb only. */
    hr_frame_t f;
    CHECK(hr_frame_parse(wire, &f));
    CHECK_STR(f.verb, "GOTIT SN12345 7");
    CHECK_INT(f.nfields, 0);
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
    test_outbound_framing_is_not_inbound_framing();
    test_overflow_is_reported_not_truncated();
    return TEST_REPORT();
}
