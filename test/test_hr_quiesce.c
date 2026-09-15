/*
 * The quiesce gate that guards the capture filesystem's teardown.
 *
 * The shutdown sequence relies on exactly these properties: a user inside the
 * gate keeps it from going idle, a late user is refused once it is closing,
 * and the gate only reports idle when both are true - closing and empty.
 */
#include "hr_quiesce.h"
#include "test_util.h"

static void test_open_gate(void)
{
    TEST_CASE("an open gate admits and counts users, and is never idle");
    hr_quiesce_t q = HR_QUIESCE_INIT;
    CHECK(!hr_quiesce_closing(&q));
    CHECK(!hr_quiesce_idle(&q));         /* open, not closing: not a go-ahead */
    CHECK_INT(hr_quiesce_users(&q), 0);

    CHECK(hr_quiesce_enter(&q));
    CHECK(hr_quiesce_enter(&q));         /* nesting is a count */
    CHECK_INT(hr_quiesce_users(&q), 2);
    hr_quiesce_leave(&q);
    CHECK_INT(hr_quiesce_users(&q), 1);
    hr_quiesce_leave(&q);
    CHECK_INT(hr_quiesce_users(&q), 0);
    CHECK(!hr_quiesce_idle(&q));
}

static void test_close_waits_for_users(void)
{
    TEST_CASE("closing refuses newcomers and goes idle only when empty");
    hr_quiesce_t q = HR_QUIESCE_INIT;
    CHECK(hr_quiesce_enter(&q));          /* a reader mid-download */

    hr_quiesce_close(&q);
    CHECK(hr_quiesce_closing(&q));
    CHECK(!hr_quiesce_idle(&q));          /* must wait for the reader */
    CHECK_INT(hr_quiesce_users(&q), 1);

    /* A late writer is turned away and leaves no trace in the count. */
    CHECK(!hr_quiesce_enter(&q));
    CHECK_INT(hr_quiesce_users(&q), 1);

    hr_quiesce_leave(&q);                 /* the reader finishes */
    CHECK_INT(hr_quiesce_users(&q), 0);
    CHECK(hr_quiesce_idle(&q));           /* now the unmount may proceed */

    hr_quiesce_close(&q);                 /* idempotent */
    CHECK(hr_quiesce_idle(&q));
    CHECK(!hr_quiesce_enter(&q));
}

static void test_close_with_nobody_inside(void)
{
    TEST_CASE("closing an empty gate is idle at once - the no-writer case");
    hr_quiesce_t q = HR_QUIESCE_INIT;
    hr_quiesce_close(&q);
    CHECK(hr_quiesce_idle(&q));
    CHECK(!hr_quiesce_enter(&q));
    CHECK_INT(hr_quiesce_users(&q), 0);
}

static void test_reopen(void)
{
    TEST_CASE("reopen admits again and keeps the users who never left");
    hr_quiesce_t q = HR_QUIESCE_INIT;
    CHECK(hr_quiesce_enter(&q));          /* a slow reader */
    hr_quiesce_close(&q);
    CHECK(!hr_quiesce_enter(&q));
    CHECK(!hr_quiesce_idle(&q));

    /* The closer gave up waiting (a refused reformat) and lets traffic
     * resume. The slow reader is still inside and still counted. */
    hr_quiesce_reopen(&q);
    CHECK(!hr_quiesce_closing(&q));
    CHECK(!hr_quiesce_idle(&q));
    CHECK_INT(hr_quiesce_users(&q), 1);
    CHECK(hr_quiesce_enter(&q));
    CHECK_INT(hr_quiesce_users(&q), 2);
    hr_quiesce_leave(&q);
    hr_quiesce_leave(&q);
    CHECK_INT(hr_quiesce_users(&q), 0);
}

int main(void)
{
    test_open_gate();
    test_close_waits_for_users();
    test_close_with_nobody_inside();
    test_reopen();
    return TEST_REPORT();
}
