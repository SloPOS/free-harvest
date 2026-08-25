#include "hr_session.h"
#include "test_util.h"

/* Captures everything the session transmits. */
typedef struct {
    char buf[2048];
    size_t len;
    int frames;
} tx_log_t;

static void tx_capture(const char *data, size_t len, void *user)
{
    tx_log_t *t = (tx_log_t *)user;
    if (t->len + len < sizeof(t->buf)) {
        memcpy(t->buf + t->len, data, len);
        t->len += len;
        t->buf[t->len] = '\0';
    }
    t->frames++;
}

static void feed(hr_session_t *s, const char *frame, unsigned long t_ms)
{
    hr_session_rx(s, frame, strlen(frame), t_ms);
}

static void test_acks_reqinfo_with_gotit(void)
{
    /*
     * The genuine adapter answers REQINFO with WIFIINFO. Captured over USB:
     *
     *     WIFIINFO 5 81 "MyNetwork" 1 HR_aabbccddeeff 0 1 37
     *
     * We used to answer GOTIT, with a payload this project admitted was
     * invented. One dryer tolerated it and sent telemetry anyway; another
     * never sent any, re-asking REQINFO every two seconds indefinitely -
     * 1,300 frames received and not one STAT among them.
     *
     * Pinned to the captured shape rather than to our idea of it, so the
     * regression cannot come back quietly.
     */
    TEST_CASE("answers REQINFO with WIFIINFO");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_wifi(&s, 5, 81, "MyNetwork", "HR_aabbccddeeff");

    feed(&s, "REQINFO\r", 37000);

    CHECK_INT(log.frames, 1);
    CHECK_STR(log.buf,
              "WIFIINFO 5 81 \"MyNetwork\" 0 HR_aabbccddeeff 0 0 37\r");
}

static void test_reqinfo_before_wifi_is_up(void)
{
    /*
     * Asked before the network is up, the answer must still be a
     * well-formed WIFIINFO. An empty SSID is quoted as an empty pair
     * rather than vanishing - in a space-delimited frame a field that
     * disappears shifts every field after it.
     */
    TEST_CASE("WIFIINFO is well formed with no network");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    feed(&s, "REQINFO\r", 1000);

    CHECK_INT(log.frames, 1);
    CHECK_STR(log.buf, "WIFIINFO 0 0 \"\" 0 HR 0 0 1\r");
}

static void test_captures_serial_number(void)
{
    TEST_CASE("captures serial number from SNM");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    feed(&s, "SNM,FD1234567,\r", 1000);

    CHECK_STR(s.info.serial, "FD1234567");
}

static void test_captures_uid(void)
{
    TEST_CASE("captures uid from UID frame");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    feed(&s, "UID,1A2B3C4D-5E6F-7788-99AA,3,6.0.641041,1,0,0,0,0,0,0,\r", 1000);

    CHECK_STR(s.info.uid, "1A2B3C4D-5E6F-7788-99AA");
}

static void test_records_latest_stat(void)
{
    TEST_CASE("records latest STAT");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    feed(&s, "STAT,1,DRY,20,-30,1200,86400,3,a,b,\r", 1000);

    CHECK(s.info.have_stat);
    CHECK_STR(s.info.last_stat, "STAT,1,DRY,20,-30,1200,86400,3,a,b,");
}

static void test_link_comes_up_then_times_out(void)
{
    TEST_CASE("link comes up then times out");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    CHECK_INT(s.link, HR_LINK_DOWN);

    feed(&s, "STAT,1,IDLE,0,0,0,0,0,x,y,\r", 1000);
    CHECK_INT(s.link, HR_LINK_UP);

    /* still within the window */
    hr_session_tick(&s, 1000 + HR_LINK_TIMEOUT_MS - 1);
    CHECK_INT(s.link, HR_LINK_UP);

    /* past the window with no traffic */
    hr_session_tick(&s, 1000 + HR_LINK_TIMEOUT_MS + 1);
    CHECK_INT(s.link, HR_LINK_DOWN);
}

static void test_unknown_verb_is_counted_not_fatal(void)
{
    TEST_CASE("unknown verb is counted not fatal");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    feed(&s, "WOBBLE,1,2,\r", 1000);

    CHECK_INT(s.unknown_verbs, 1);
    CHECK_INT(s.frames_in, 1);
    CHECK_INT(s.link, HR_LINK_UP); /* traffic is traffic */
    CHECK_INT(log.frames, 0);      /* nothing sent in reply */
}

static void test_send_simple_emits_terminated_frame(void)
{
    TEST_CASE("send_simple emits CR-terminated frame");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    CHECK(hr_session_send_simple(&s, "REQSTAT"));
    CHECK_STR(log.buf, "REQSTAT\r");
    CHECK_INT(s.frames_out, 1);
}

typedef struct {
    int count;
    char last_verb[HR_MAX_VERB];
} obs_log_t;

static void observe(const hr_frame_t *f, void *user)
{
    obs_log_t *o = (obs_log_t *)user;
    o->count++;
    snprintf(o->last_verb, sizeof(o->last_verb), "%s", f->verb);
}

static void test_observer_sees_inbound_frames(void)
{
    TEST_CASE("observer sees inbound frames");
    tx_log_t log = {0};
    obs_log_t obs = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_observer(&s, observe, &obs);

    feed(&s, "NTFY,1,2,msg,3,\r", 1000);

    CHECK_INT(obs.count, 1);
    CHECK_STR(obs.last_verb, "NTFY");
    CHECK_INT(s.frames_in, 1);
}

static void test_classify_safe_verbs(void)
{
    TEST_CASE("classify safe verbs");
    CHECK_INT(hr_cmd_classify("REQSTAT"), HR_CMD_SAFE);
    CHECK_INT(hr_cmd_classify("REQSYSINF"), HR_CMD_SAFE);
    CHECK_INT(hr_cmd_classify("STATUS"), HR_CMD_SAFE);
    CHECK_INT(hr_cmd_classify("BEEP"), HR_CMD_SAFE);
    /* CLICK is the control verb - "CLICK 1 10 <n> <s>" presses Start on
     * the Ready screen. It must never be reachable from the web UI. */
    CHECK_INT(hr_cmd_classify("CLICK"), HR_CMD_UNKNOWN);
}

static void test_classify_refuses_dangerous_and_unknown(void)
{
    TEST_CASE("classify refuses dangerous and unknown");
    /* Hardware-control and reboot must stay UNKNOWN (never sendable). */
    CHECK_INT(hr_cmd_classify("REBOOT"), HR_CMD_UNKNOWN);
    CHECK_INT(hr_cmd_classify("DUTY"), HR_CMD_UNKNOWN);
    /* File-delete is intentionally NOT in the config allow-list. */
    CHECK_INT(hr_cmd_classify("DEL"), HR_CMD_UNKNOWN);
    /* garbage */
    CHECK_INT(hr_cmd_classify("HACKTHEGIBSON"), HR_CMD_UNKNOWN);
    CHECK_INT(hr_cmd_classify(""), HR_CMD_UNKNOWN);
    /* Config verbs are allowed as CONFIG, but crucially NOT as SAFE, so the
     * verb-only "safe" path can never fire them. */
    CHECK(hr_cmd_classify("SETDATE") != HR_CMD_SAFE);
    /*
     * SETSN and FDRENAME must be UNKNOWN, not merely "not SAFE".
     * The old assertion passed while both sat in the CONFIG list, which
     * made them fully sendable with arguments from /api/cmd - a test
     * that cannot fail for the case that matters is not a test.
     */
    CHECK_INT(hr_cmd_classify("SETSN"), HR_CMD_UNKNOWN);
    CHECK_INT(hr_cmd_classify("FDRENAME"), HR_CMD_UNKNOWN);
}

static void test_send_safe_transmits_only_allowlisted(void)
{
    TEST_CASE("send_safe transmits only allowlisted");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    CHECK(hr_session_send_safe(&s, "REQSTAT"));
    CHECK_STR(log.buf, "REQSTAT\r");
    CHECK_INT(s.frames_out, 1);
}

static void test_send_safe_refuses_dangerous_sends_nothing(void)
{
    TEST_CASE("send_safe refuses dangerous, sends nothing");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    CHECK(!hr_session_send_safe(&s, "REBOOT"));
    CHECK(!hr_session_send_safe(&s, "DUTY"));
    CHECK(!hr_session_send_safe(&s, "SETSN"));
    /* With ARGS is the path that actually mattered: /api/cmd routes to
     * send_config whenever args are present, so these are the calls the
     * web UI could really have made. */
    CHECK(!hr_session_send_config(&s, "SETSN", "12345"));
    CHECK(!hr_session_send_config(&s, "FDRENAME", "whatever"));
    CHECK(!hr_session_send_config(&s, "CLICK", "1,10,54779,175300"));
    /* nothing was transmitted and no frame counted */
    CHECK_INT(log.frames, 0);
    CHECK_INT(log.len, 0);
    CHECK_INT(s.frames_out, 0);
}

static void test_classify_config_verbs(void)
{
    TEST_CASE("classify config verbs");
    CHECK_INT(hr_cmd_classify("SENDBATCH"), HR_CMD_CONFIG);
    CHECK_INT(hr_cmd_classify("SENDCUSTOM"), HR_CMD_CONFIG);
    CHECK_INT(hr_cmd_classify("SETPREF"), HR_CMD_CONFIG);
    CHECK_INT(hr_cmd_classify("SETDATE"), HR_CMD_CONFIG);
    CHECK_INT(hr_cmd_classify("SETBNAME"), HR_CMD_CONFIG);
}

static void test_hardware_verbs_stay_unknown(void)
{
    /* These MUST never be classified safe or config - they drive hardware. */
    TEST_CASE("hardware verbs stay unknown");
    CHECK_INT(hr_cmd_classify("DUTY"), HR_CMD_UNKNOWN);
    CHECK_INT(hr_cmd_classify("HCS"), HR_CMD_UNKNOWN);
    CHECK_INT(hr_cmd_classify("SPC"), HR_CMD_UNKNOWN);
    CHECK_INT(hr_cmd_classify("REBOOT"), HR_CMD_UNKNOWN);
    CHECK_INT(hr_cmd_classify("XWIFI"), HR_CMD_UNKNOWN);
}

static void test_send_config_transmits_with_args(void)
{
    TEST_CASE("send_config transmits with args");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    CHECK(hr_session_send_config(&s, "SETBNAME", "MyBatch"));
    CHECK_STR(log.buf, "SETBNAME MyBatch\r");

    log.buf[0] = '\0';
    log.len = 0;
    CHECK(hr_session_send_config(&s, "REQSTAT", NULL)); /* SAFE also allowed */
    CHECK_STR(log.buf, "REQSTAT\r");
}

static void test_send_config_refuses_hardware(void)
{
    TEST_CASE("send_config refuses hardware verbs");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    CHECK(!hr_session_send_config(&s, "DUTY", "100"));
    CHECK(!hr_session_send_config(&s, "REBOOT", NULL));
    CHECK(!hr_session_send_config(&s, "NOTACOMMAND", "x"));
    CHECK_INT(log.frames, 0);
    CHECK_INT(log.len, 0);
}

static void test_link_survives_normal_idle_frame_gap(void)
{
    /*
     * Regression: the dryer sends idle STAT frames every ~15,021 ms - just
     * OVER a 15,000 ms timeout - so the link expired a few ms before each
     * frame arrived and the UI flapped disconnected/ready every 15 seconds.
     * The timeout must comfortably exceed the real frame cadence.
     */
    TEST_CASE("link survives normal idle frame gap");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    unsigned long t = 100000;
    feed(&s, "STAT,1,0,0,0,69,151882,0,0,38,0,1,QUALITY,v6.4,,\r", t);
    CHECK_INT(s.link, HR_LINK_UP);

    /* real idle gaps measured from a capture, ticking in between as main() does */
    const unsigned long gaps[] = {15021, 15020, 15021, 15041, 15021};
    for (int i = 0; i < 5; i++) {
        for (unsigned long k = 250; k < gaps[i]; k += 250) {
            hr_session_tick(&s, t + k);
        }
        t += gaps[i];
        hr_session_tick(&s, t);
        CHECK_INT(s.link, HR_LINK_UP); /* must NOT drop between frames */
        feed(&s, "STAT,1,0,0,0,69,151882,0,0,38,0,1,QUALITY,v6.4,,\r", t);
    }
    CHECK_INT(s.link, HR_LINK_UP);
}

static void test_link_still_drops_when_dryer_really_gone(void)
{
    /* The timeout must still detect a genuinely unplugged dryer. */
    TEST_CASE("link drops when dryer really gone");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    feed(&s, "STAT,1,0,0,0,69,151882,0,0,38,0,1,QUALITY,v6.4,,\r", 100000);
    CHECK_INT(s.link, HR_LINK_UP);
    hr_session_tick(&s, 100000 + HR_LINK_TIMEOUT_MS + 1000);
    CHECK_INT(s.link, HR_LINK_DOWN);
}

int main(void)
{
    test_link_survives_normal_idle_frame_gap();
    test_link_still_drops_when_dryer_really_gone();
    test_classify_config_verbs();
    test_hardware_verbs_stay_unknown();
    test_send_config_transmits_with_args();
    test_send_config_refuses_hardware();
    test_classify_safe_verbs();
    test_classify_refuses_dangerous_and_unknown();
    test_send_safe_transmits_only_allowlisted();
    test_send_safe_refuses_dangerous_sends_nothing();
    test_acks_reqinfo_with_gotit();
    test_reqinfo_before_wifi_is_up();
    test_captures_serial_number();
    test_captures_uid();
    test_records_latest_stat();
    test_link_comes_up_then_times_out();
    test_unknown_verb_is_counted_not_fatal();
    test_send_simple_emits_terminated_frame();
    test_observer_sees_inbound_frames();
    return TEST_REPORT();
}
