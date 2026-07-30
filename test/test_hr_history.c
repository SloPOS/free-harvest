#include "hr_history.h"
#include "test_util.h"

static uint32_t add_line(hr_history_t *h, const char *line, uint32_t t)
{
    hr_frame_t f;
    if (!hr_frame_parse(line, &f)) {
        return 0;
    }
    return hr_history_add(h, &f, t);
}

static void test_add_assigns_increasing_seq(void)
{
    TEST_CASE("add assigns increasing seq");
    hr_history_t h;
    hr_history_init(&h);

    CHECK_INT(add_line(&h, "STAT,1,a\r", 100), 1);
    CHECK_INT(add_line(&h, "NTFY,2,b\r", 200), 2);
    CHECK_INT(hr_history_latest_seq(&h), 2);
    CHECK_INT(h.total, 2);
}

static void test_since_returns_only_newer_entries(void)
{
    TEST_CASE("since returns only newer entries");
    hr_history_t h;
    hr_history_init(&h);
    add_line(&h, "STAT,1\r", 100);
    add_line(&h, "STAT,2\r", 200);
    add_line(&h, "STAT,3\r", 300);

    hr_hist_entry_t out[HR_HIST_CAP];
    int n = hr_history_since(&h, 1, out, HR_HIST_CAP);
    CHECK_INT(n, 2);
    CHECK_INT(out[0].seq, 2);
    CHECK_STR(out[0].body, "STAT,2");
    CHECK_INT(out[1].seq, 3);
    CHECK_STR(out[1].body, "STAT,3");
}

static void test_since_zero_returns_all_available(void)
{
    TEST_CASE("since zero returns all available");
    hr_history_t h;
    hr_history_init(&h);
    add_line(&h, "A,1\r", 1);
    add_line(&h, "B,2\r", 2);

    hr_hist_entry_t out[HR_HIST_CAP];
    int n = hr_history_since(&h, 0, out, HR_HIST_CAP);
    CHECK_INT(n, 2);
    CHECK_STR(out[0].verb, "A");
    CHECK_STR(out[1].verb, "B");
}

static void test_ring_overwrites_oldest(void)
{
    TEST_CASE("ring overwrites oldest");
    hr_history_t h;
    hr_history_init(&h);
    /* push one more than capacity */
    for (uint32_t i = 0; i < HR_HIST_CAP + 5; i++) {
        char line[32];
        snprintf(line, sizeof(line), "STAT,%u\r", i);
        add_line(&h, line, i);
    }
    CHECK_INT(hr_history_latest_seq(&h), HR_HIST_CAP + 5);

    /* asking since 0 can only return the last HR_HIST_CAP frames */
    hr_hist_entry_t out[HR_HIST_CAP];
    int n = hr_history_since(&h, 0, out, HR_HIST_CAP);
    CHECK_INT(n, HR_HIST_CAP);
    /* oldest surviving seq = total - cap + 1 */
    CHECK_INT(out[0].seq, (HR_HIST_CAP + 5) - HR_HIST_CAP + 1);
}

static void test_since_respects_max(void)
{
    TEST_CASE("since respects max");
    hr_history_t h;
    hr_history_init(&h);
    for (int i = 0; i < 10; i++) {
        char line[32];
        snprintf(line, sizeof(line), "X,%d\r", i);
        add_line(&h, line, (uint32_t)i);
    }
    hr_hist_entry_t out[3];
    int n = hr_history_since(&h, 0, out, 3);
    CHECK_INT(n, 3);
    /* returns the OLDEST available first, capped at max */
    CHECK_STR(out[0].verb, "X");
}

static void test_verb_table_counts_and_latest(void)
{
    TEST_CASE("verb table counts and latest");
    hr_history_t h;
    hr_history_init(&h);
    add_line(&h, "STAT,1,DRY\r", 1);
    add_line(&h, "STAT,2,DRY\r", 2);
    add_line(&h, "NTFY,9\r", 3);

    const hr_hist_verb_t *stat = hr_history_verb(&h, "STAT");
    CHECK(stat != NULL);
    CHECK_INT(stat->count, 2);
    CHECK_STR(stat->last_body, "STAT,2,DRY");

    const hr_hist_verb_t *ntfy = hr_history_verb(&h, "NTFY");
    CHECK(ntfy != NULL);
    CHECK_INT(ntfy->count, 1);

    CHECK(hr_history_verb(&h, "NOPE") == NULL);
}

static void test_verb_diff_mask_marks_changed_fields(void)
{
    TEST_CASE("verb diff mask marks changed fields");
    hr_history_t h;
    hr_history_init(&h);
    /* field 0 same, field 1 changes, field 2 same */
    add_line(&h, "STAT,1,10,X\r", 1);
    add_line(&h, "STAT,1,20,X\r", 2);

    const hr_hist_verb_t *stat = hr_history_verb(&h, "STAT");
    CHECK(stat != NULL);
    /* only field index 1 changed */
    CHECK_INT(stat->changed_mask, (1u << 1));
}

int main(void)
{
    test_add_assigns_increasing_seq();
    test_since_returns_only_newer_entries();
    test_since_zero_returns_all_available();
    test_ring_overwrites_oldest();
    test_since_respects_max();
    test_verb_table_counts_and_latest();
    test_verb_diff_mask_marks_changed_fields();
    return TEST_REPORT();
}
