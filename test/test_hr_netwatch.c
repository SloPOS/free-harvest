/*
 * The no-IP watchdog: a station that is associated but has no address.
 *
 * The property that matters most is the one the bug report is about: an
 * association that keeps its RSSI but loses its DHCP lease must be reported
 * as NOT connected, and must be acted on - DHCP restarted, then the network
 * rejoined - within bounded time, with the rejoins backing off rather than
 * hammering. And an ordinary join, where the address arrives in a second or
 * two, must trip nothing.
 */
#include "hr_netwatch.h"
#include "test_util.h"

/* Short timings so the tests read in seconds: 2 s grace, DHCP restart at
 * 5 s, rejoin at 10 s, rejoin back-off capped at 40 s. */
static const hr_netwatch_cfg_t CFG = {
    .grace_ms = 2000,
    .dhcp_restart_ms = 5000,
    .reconnect_ms = 10000,
    .reconnect_max_ms = 40000,
};

#define S(x) ((uint32_t)((x) * 1000u))

/* Advance the clock one second at a time, ticking with a fixed have_ip, and
 * return the first action seen (or NONE). Stops at `until`. */
static hr_netwatch_action_t run_until(hr_netwatch_t *w, bool have_ip,
                                      uint32_t *t, uint32_t until)
{
    while (*t < until) {
        *t += 1000;
        hr_netwatch_action_t a = hr_netwatch_tick(w, have_ip, *t);
        if (a != HR_NETWATCH_NONE) {
            return a;
        }
    }
    return HR_NETWATCH_NONE;
}

static void test_defaults(void)
{
    TEST_CASE("zero config fills in the defaults, cap never below the base");
    hr_netwatch_t w;
    hr_netwatch_init(&w, NULL);
    CHECK_INT(w.cfg.grace_ms, 5000);
    CHECK_INT(w.cfg.dhcp_restart_ms, 15000);
    CHECK_INT(w.cfg.reconnect_ms, 45000);
    CHECK_INT(w.cfg.reconnect_max_ms, 360000);

    hr_netwatch_cfg_t big = {.reconnect_ms = 900000, .reconnect_max_ms = 1000};
    hr_netwatch_init(&w, &big);
    CHECK_INT(w.cfg.reconnect_max_ms, 900000);
}

static void test_normal_join_is_quiet(void)
{
    TEST_CASE("a join whose address arrives within the grace trips nothing");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = S(100);
    CHECK_INT(hr_netwatch_tick(&w, false, t), HR_NETWATCH_NONE); /* idle */
    CHECK(!hr_netwatch_no_ip(&w, t));

    hr_netwatch_on_assoc(&w, t);
    t += 1500; /* DHCP took 1.5 s */
    CHECK(!hr_netwatch_no_ip(&w, t));
    CHECK_INT(hr_netwatch_tick(&w, true, t), HR_NETWATCH_NONE);
    CHECK(!hr_netwatch_no_ip(&w, t));
    CHECK_INT(hr_netwatch_noip_for_ms(&w, t), 0);
    CHECK_INT(w.episodes, 0);

    /* Hours of renewals later, still fine. */
    CHECK_INT(run_until(&w, true, &t, S(4000)), HR_NETWATCH_NONE);
    CHECK_INT(w.episodes, 0);
    CHECK_INT(w.dhcp_restarts, 0);
    CHECK_INT(w.reconnects, 0);
}

static void test_lost_lease_is_reported_then_repaired(void)
{
    TEST_CASE("associated + ip==0 -> NO_IP after grace, DHCP restart, rejoin");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = S(10);
    hr_netwatch_on_assoc(&w, t);
    t += 1000;
    hr_netwatch_tick(&w, true, t); /* got an address */

    /* The lease expires: lwIP drops the address, the association stays. */
    uint32_t lost = t + S(3600);
    t = lost;
    CHECK_INT(hr_netwatch_tick(&w, false, t), HR_NETWATCH_NONE);
    CHECK(!hr_netwatch_no_ip(&w, t)); /* inside the grace: not yet */
    CHECK(w.associated);

    t = lost + S(2);
    hr_netwatch_tick(&w, false, t);
    CHECK(hr_netwatch_no_ip(&w, t)); /* grace over: report it */
    CHECK_INT(hr_netwatch_noip_for_ms(&w, t), S(2));
    CHECK_INT(w.episodes, 1);

    /* First remedy at 5 s: restart the DHCP client, once. */
    t = lost + S(2);
    CHECK_INT(run_until(&w, false, &t, lost + S(20)),
              HR_NETWATCH_DHCP_RESTART);
    CHECK_INT(t, lost + S(5));
    CHECK_INT(w.dhcp_restarts, 1);

    /* Second remedy at 10 s: rejoin. Nothing else in between. */
    CHECK_INT(run_until(&w, false, &t, lost + S(20)), HR_NETWATCH_RECONNECT);
    CHECK_INT(t, lost + S(10));
    CHECK_INT(w.reconnects, 1);
    CHECK(hr_netwatch_no_ip(&w, t)); /* still not connected */

    /* The driver reports the disassociation; we are no longer "no IP",
     * we are simply not associated. */
    hr_netwatch_on_disassoc(&w, t);
    CHECK(!hr_netwatch_no_ip(&w, t));
    CHECK_INT(hr_netwatch_noip_for_ms(&w, t), 0);
    CHECK_INT(hr_netwatch_tick(&w, false, t), HR_NETWATCH_NONE);

    /* Rejoined, DHCP answers this time: episode over, back-off reset. */
    t += 1000;
    hr_netwatch_on_assoc(&w, t);
    t += 1000;
    CHECK_INT(hr_netwatch_tick(&w, true, t), HR_NETWATCH_NONE);
    CHECK(!hr_netwatch_no_ip(&w, t));
    CHECK_INT(w.backoff_n, 0);
    CHECK_INT(hr_netwatch_reconnect_delay_ms(&w), S(10));
    CHECK_INT(w.episodes, 1);
}

static void test_persistent_no_dhcp_backs_off(void)
{
    TEST_CASE("a network that never answers is rejoined with doubling waits");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = 0;
    uint32_t expected_wait[] = {S(10), S(20), S(40), S(40), S(40)};
    for (unsigned i = 0; i < 5; i++) {
        uint32_t joined = t;
        hr_netwatch_on_assoc(&w, t);
        hr_netwatch_action_t a = run_until(&w, false, &t, joined + S(300));
        CHECK_INT(a, HR_NETWATCH_DHCP_RESTART);
        CHECK_INT(t, joined + S(5)); /* DHCP restart timing never grows */
        a = run_until(&w, false, &t, joined + S(300));
        CHECK_INT(a, HR_NETWATCH_RECONNECT);
        CHECK_INT(t - joined, expected_wait[i]); /* the rejoin wait does */
        CHECK_INT(w.reconnects, i + 1);
        hr_netwatch_on_disassoc(&w, t);
        t += 1000;
    }
    CHECK_INT(w.dhcp_restarts, 5);
    CHECK_INT(w.episodes, 5);
    /* Every episode was reported as NO_IP once the grace had passed. */
}

static void test_join_that_never_gets_an_address(void)
{
    TEST_CASE("a first join with no DHCP answer is handled the same way");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = S(5);
    hr_netwatch_on_assoc(&w, t);
    hr_netwatch_action_t a = run_until(&w, false, &t, S(60));
    CHECK_INT(a, HR_NETWATCH_DHCP_RESTART);
    CHECK_INT(t, S(10));
    CHECK(hr_netwatch_no_ip(&w, t));
    a = run_until(&w, false, &t, S(60));
    CHECK_INT(a, HR_NETWATCH_RECONNECT);
    CHECK_INT(t, S(15));
}

static void test_reconnect_repeats_if_disconnect_never_comes(void)
{
    TEST_CASE("if the driver never reports the leave, ask again after the wait");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = 0;
    hr_netwatch_on_assoc(&w, t);
    run_until(&w, false, &t, S(100)); /* DHCP restart */
    run_until(&w, false, &t, S(100)); /* rejoin #1 at 10 s */
    CHECK_INT(t, S(10));
    /* No on_disassoc(). The next request comes after the doubled wait. */
    hr_netwatch_action_t a = run_until(&w, false, &t, S(100));
    CHECK_INT(a, HR_NETWATCH_RECONNECT);
    CHECK_INT(t, S(30));
    CHECK_INT(w.reconnects, 2);
    a = run_until(&w, false, &t, S(200));
    CHECK_INT(a, HR_NETWATCH_RECONNECT);
    CHECK_INT(t, S(70)); /* 40 s: the cap */
}

static void test_not_associated_is_never_no_ip(void)
{
    TEST_CASE("without an association there is nothing to watch");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = 0;
    CHECK_INT(run_until(&w, false, &t, S(600)), HR_NETWATCH_NONE);
    CHECK(!hr_netwatch_no_ip(&w, t));
    CHECK_INT(w.episodes, 0);
    /* An address report while not associated (stale event) is ignored. */
    CHECK_INT(hr_netwatch_tick(&w, true, t), HR_NETWATCH_NONE);
    CHECK(!w.have_ip);
}

static void test_clock_wrap(void)
{
    TEST_CASE("a millisecond clock wrapping through zero does not confuse it");
    hr_netwatch_t w;
    hr_netwatch_init(&w, &CFG);
    uint32_t t = 0xFFFFFFFFu - S(3);
    hr_netwatch_on_assoc(&w, t);
    /* Step by hand: run_until()'s "t < until" cannot cross the wrap. */
    hr_netwatch_action_t a = HR_NETWATCH_NONE;
    int steps = 0;
    while (a == HR_NETWATCH_NONE && steps < 30) {
        t += 1000;
        steps++;
        a = hr_netwatch_tick(&w, false, t);
    }
    CHECK_INT(a, HR_NETWATCH_DHCP_RESTART);
    CHECK_INT(steps, 5);
    CHECK_INT(t, S(2) - 1); /* 5 s after the assoc, across the wrap */
    CHECK(hr_netwatch_no_ip(&w, t));
}

int main(void)
{
    test_defaults();
    test_normal_join_is_quiet();
    test_lost_lease_is_reported_then_repaired();
    test_persistent_no_dhcp_backs_off();
    test_join_that_never_gets_an_address();
    test_reconnect_repeats_if_disconnect_never_comes();
    test_not_associated_is_never_no_ip();
    test_clock_wrap();
    return TEST_REPORT();
}
