#include "hr_session.h"
#include "test_util.h"

/* Captures everything the session transmits. */
typedef struct {
    char buf[2048];
    size_t len;
    int frames;
} tx_log_t;

static bool tx_capture(const char *data, size_t len, void *user)
{
    tx_log_t *t = (tx_log_t *)user;
    if (t->len + len < sizeof(t->buf)) {
        memcpy(t->buf + t->len, data, len);
        t->len += len;
        t->buf[t->len] = '\0';
    }
    t->frames++;
    return true;
}

/* A transport with nobody on the other end: accepts nothing. */
static bool tx_refuse(const char *data, size_t len, void *user)
{
    (void)data;
    (void)len;
    (void)user;
    return false;
}

static void test_send_reports_transport_failure(void)
{
    /*
     * hr_session_send() used to return true and count the frame as sent
     * whatever the transport did with it. hr_usb_tx() logged "short write" /
     * "flush failed" and the web UI still showed {"ok":true} for a CLICK that
     * never left the FIFO. The transport's verdict is now the session's.
     */
    TEST_CASE("send reports transport failure");
    hr_session_t s;
    hr_session_init(&s, tx_refuse, NULL);

    CHECK(!hr_session_send_simple(&s, "REQSTAT"));
    CHECK(!hr_session_send_raw(&s, "SENDCANDY \"4,70,140,150,160,300,7200,300,C,0,\" 1"));
    CHECK_INT(s.frames_out, 0);

    tx_log_t log = {0};
    hr_session_init(&s, tx_capture, &log);
    CHECK(hr_session_send_simple(&s, "REQSTAT"));
    CHECK_INT(s.frames_out, 1);
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

static void test_recipe_verbs_refused_on_generic_path(void)
{
    /*
     * SENDCANDY through the generic field builder does not fail loudly - it
     * produces a different, still-parseable recipe:
     *
     *     want: SENDCANDY "4,70,140,...,CANDY,0," 100001
     *     got:  SENDCANDY 4 70 140 ... CANDY 0
     *
     * On a machine about to run, that sets temperatures nobody chose. Refused
     * outright so /api/cmd and the MQTT command topic cannot reach it; the
     * validated hr_recipe_build() path is the only way to send one.
     */
    TEST_CASE("recipe verbs are refused on the generic config path");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    CHECK(!hr_session_send_config(&s, "SENDCANDY",
                                  "4,70,140,150,160,300,7200,300,CANDY,0,"));
    CHECK(!hr_session_send_config(&s, "SENDCUSTOM", "1,2,3"));
    CHECK(!hr_session_send_config(&s, "SENDBATCH", "1"));
    CHECK(!hr_session_send_config(&s, "SENDSCIENCE", "1"));

    /* nothing reached the wire */
    CHECK_INT(log.frames, 0);

    /* a genuine config verb still works */
    CHECK(hr_session_send_config(&s, "SETDATE", "2026,8,26"));
    CHECK_INT(log.frames, 1);
}

static void test_heartbeat_is_only_state(void)
{
    /*
     * The heartbeat is a bare STATE and nothing else.
     *
     * 1.0.5.5 and 1.0.5.6 made it re-ask FDNAME, REQCFG and STATUS until the
     * dryer answered, copying the genuine adapter, which does exactly that.
     * It was chasing a machine that answered UNIQUE and then went silent -
     * and that machine turned out to be running broken firmware the GENUINE
     * adapter could not talk to either. The retries were solving nothing, so
     * they came back out.
     *
     * Pinned as a test because "re-ask until answered" is a reasonable-looking
     * idea that would otherwise get reinvented.
     */
    TEST_CASE("heartbeat sends STATE and nothing else");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_wifi(&s, 5, 81, "MyNetwork", "HR_aabbccddeeff");

    /* Nothing answered at all - still just one frame. */
    hr_session_heartbeat(&s);
    CHECK_INT(log.frames, 1);
    CHECK(strstr(log.buf, "STATE") != NULL);
    CHECK(strstr(log.buf, "FDNAME") == NULL);
    CHECK(strstr(log.buf, "REQCFG") == NULL);
    CHECK(strstr(log.buf, "STATUS") == NULL);
}

static void test_compat_644170_reasks_until_answered(void)
{
    /*
     * With the 6.0.644170 compatibility switch ON, the heartbeat copies the
     * genuine adapter: FDNAME and REQCFG go out again on every heartbeat
     * until SNM and CFG arrive, STATUS until the first STAT. Each stops on
     * its own answer, so a healthy dryer sees at most one extra round.
     */
    TEST_CASE("compat 644170: heartbeat re-asks until each answer arrives");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_compat(&s, true, true);
    hr_session_set_wifi(&s, 5, 81, "MyNetwork", "HR_aabbccddeeff");

    hr_session_heartbeat(&s);
    CHECK_INT(log.frames, 4);
    CHECK(strstr(log.buf, "STATE 5 81\r") != NULL);
    CHECK(strstr(log.buf, "FDNAME\r") != NULL);
    CHECK(strstr(log.buf, "REQCFG\r") != NULL);
    CHECK(strstr(log.buf, "STATUS\r") != NULL);

    /* the name arrives: FDNAME stops, the other two continue */
    feed(&s, "SNM,My Freeze Dryer,\r", 1000);
    memset(&log, 0, sizeof(log));
    hr_session_heartbeat(&s);
    CHECK_INT(log.frames, 3);
    CHECK(strstr(log.buf, "FDNAME") == NULL);
    CHECK(strstr(log.buf, "REQCFG") != NULL);
    CHECK(strstr(log.buf, "STATUS") != NULL);

    /* CFG and a STAT arrive: back to a bare STATE */
    feed(&s, "CFG,1,1,PSTF000000000XXX,0,Auto,v6.4,\r", 2000);
    feed(&s, "STAT,1,0,0,0,68,151697,265,0,38,1,1,Auto,v6.4,\r", 2100);
    memset(&log, 0, sizeof(log));
    hr_session_heartbeat(&s);
    CHECK_INT(log.frames, 1);
    CHECK(strstr(log.buf, "STATE") != NULL);

    /* switching the mode off restores the pinned default immediately */
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_compat(&s, true, true);
    hr_session_set_compat(&s, false, false);
    memset(&log, 0, sizeof(log));
    hr_session_heartbeat(&s);
    CHECK_INT(log.frames, 1);
}

static void test_compat_644170_tags_unique(void)
{
    /*
     * The genuine adapter is captured sending "UNIQUE lH" unprompted at
     * power-up; 6.0.644170's UNIQUE handler sets its adapter-mode byte only
     * when that argument is present. OFF by default, the argument goes out
     * from both the stepped and the one-shot handshake when ON.
     */
    TEST_CASE("compat 644170: UNIQUE carries lH only when switched on");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    hr_session_set_compat(&s, true, false);
    for (unsigned k = 0; k < HR_HELLO_STEPS; k++) {
        hr_session_hello_step(&s, k);
    }
    CHECK_INT(log.frames, HR_HELLO_STEPS);
    CHECK(strstr(log.buf, "UNIQUE lH\r") != NULL);

    memset(&log, 0, sizeof(log));
    hr_session_hello(&s);
    CHECK(strstr(log.buf, "UNIQUE lH\r") != NULL);

    /* unique_tag alone does not turn the re-ask on */
    memset(&log, 0, sizeof(log));
    hr_session_heartbeat(&s);
    CHECK_INT(log.frames, 1);

    /* and off again is bare, terminated straight after the verb */
    hr_session_set_compat(&s, false, false);
    memset(&log, 0, sizeof(log));
    hr_session_hello_step(&s, 1);
    CHECK_STR(log.buf, "UNIQUE\r");
}

static void test_hello_is_one_burst(void)
{
    /*
     * All five handshake frames go out together. They were paced at 250ms
     * apart while chasing the broken-firmware dryer; that cost every healthy
     * machine three quarters of a second and fixed nothing.
     */
    TEST_CASE("handshake is sent in one pass");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);

    for (unsigned k = 0; k < HR_HELLO_STEPS; k++) {
        hr_session_hello_step(&s, k);
    }
    CHECK_INT(log.frames, HR_HELLO_STEPS);
    CHECK(strstr(log.buf, "STATE") != NULL);
    CHECK(strstr(log.buf, "UNIQUE") != NULL);
    CHECK(strstr(log.buf, "FDNAME") != NULL);
    CHECK(strstr(log.buf, "REQCFG") != NULL);
    CHECK(strstr(log.buf, "STATUS") != NULL);
    /* bare UNIQUE by default; "lH" only behind hr_session_set_compat() */
    CHECK(strstr(log.buf, "UNIQUE lH") == NULL);
}

static void test_cloud_flags_reach_the_wire(void)
{
    /*
     * WIFIINFO fields 3 and 6 - registered and cloud.
     *
     * Both were hardcoded false for the life of this project because nothing
     * ever wrote them, so the dryer's own WiFi panel showed the SSID from
     * field 2 and no server connection, permanently. A dryer waiting to be
     * told the link is complete sits on that panel re-asking REQINFO.
     *
     * The automatic rule sets both once the adapter is associated. Pinned
     * against the captured genuine frame:
     *
     *     WIFIINFO 5 81 "MyNetwork" 1 HR_aabbccddeeff 0 1 37
     */
    TEST_CASE("WIFIINFO carries the connection flags");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_wifi(&s, 5, 81, "MyNetwork", "HR_aabbccddeeff");
    hr_session_set_cloud_auto(&s, true);

    feed(&s, "REQINFO\r", 37000);

    CHECK_INT(log.frames, 1);
    CHECK_STR(log.buf,
              "WIFIINFO 5 81 \"MyNetwork\" 1 HR_aabbccddeeff 0 1 37\r");
}

static void test_manual_cloud_override_wins(void)
{
    /*
     * A hand-set value must survive the main loop, which calls the automatic
     * rule every pass. Without this a diagnostic override would be undone
     * within milliseconds and look like the endpoint had no effect.
     */
    TEST_CASE("manual cloud override beats the automatic rule");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_wifi(&s, 5, 81, "MyNetwork", "HR_aabbccddeeff");

    hr_session_set_cloud(&s, false, false);
    hr_session_set_cloud_auto(&s, true);   /* must be ignored */

    feed(&s, "REQINFO\r", 37000);
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

static void test_stale_partial_frame_does_not_prefix_reqinfo(void)
{
    /*
     * Bench, 2026-09-15: esptool probed CDC0 (SLIP sync: control bytes and
     * a run of 'U'), then the replay sent REQINFO. The 'U's were still in
     * the stream buffer, the frame parsed as "UUU...UREQINFO", and no
     * WIFIINFO went out for the dryer's first request.
     */
    TEST_CASE("stale partial frame does not prefix REQINFO");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_wifi(&s, 5, 81, "MyNetwork", "HR_aabbccddeeff");

    static const unsigned char probe[] = {0xc0, 0x00, 0x08, 0x01, 0x00,
                                          0x55, 0x55, 0x55, 0x55};
    hr_session_rx(&s, probe, sizeof(probe), 10000);
    /* Two seconds of silence, then the dryer speaks. */
    feed(&s, "REQINFO\r", 12000);
    CHECK_INT(log.frames, 1);
    CHECK(strncmp(log.buf, "WIFIINFO 5 81", 13) == 0);
    CHECK_INT(s.frames_in, 1);
    CHECK_INT(s.unknown_verbs, 0);
    CHECK_INT(s.stream.frames_bad, 1);          /* the "UUUU" was reported */

    /* A frame legitimately split across two transfers a few ms apart is
     * still reassembled: the rule is about seconds, not chunks. */
    hr_session_rx(&s, "REQI", 4, 13000);
    hr_session_rx(&s, "NFO\r", 4, 13020);
    CHECK_INT(log.frames, 2);
    CHECK_INT(s.frames_in, 2);
}

/* --- the 6.0.644170 encoded transport, as the session sees it ------------ */

/* Real frames from the 2026-09-15 dryer session (see test_hr_enc.c). */
static const char ENC_20[] = ")S#7':CP`QB*MBJKUZab";
static const char ENC_36[] = ")S#GSIY84F/ObJG2E6:MW\\QO^]H'bJB5@M!!";

typedef struct {
    int count;
    size_t last_len;
    char last[128];
} enc_seen_t;

static void enc_observer(const char *frame, size_t len, void *user)
{
    enc_seen_t *e = (enc_seen_t *)user;
    e->count++;
    e->last_len = len;
    snprintf(e->last, sizeof(e->last), "%.*s", (int)len, frame);
}

static void test_encoded_frames_keep_the_link_up(void)
{
    /*
     * Dryer 2026-09-15, compat ON: after "UNIQUE lH" the machine answered
     * every request and volunteered a status frame every 15 s - all in the
     * ")S" transport. The session saw none of it as a frame, so 45 s later
     * the link went DOWN and with it the heartbeat and the re-ask. A
     * complete encoded frame is the dryer talking: it must hold the link
     * exactly as a plaintext frame does, while counting separately and
     * never as bad.
     */
    TEST_CASE("encoded frames keep the link up and are counted apart");
    tx_log_t log = {0};
    enc_seen_t seen = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_compat(&s, true, true);
    hr_session_set_enc_observer(&s, enc_observer, &seen);
    hr_session_set_wifi(&s, 5, 81, "MyNetwork", "HR_aabbccddeeff");

    feed(&s, "REQINFO,\r", 1000);              /* plaintext brings it up */
    CHECK_INT(s.link, HR_LINK_UP);
    CHECK_INT(s.frames_in, 1);

    /* 40 s of nothing but encoded frames, every 10-15 s */
    unsigned long t = 1000;
    for (int i = 0; i < 4; i++) {
        t += 10000;
        hr_session_tick(&s, t);
        feed(&s, ENC_20, t);
        CHECK_INT(s.link, HR_LINK_UP);
    }
    hr_session_tick(&s, t + 30000);              /* 30 s after the last one */
    CHECK_INT(s.link, HR_LINK_UP);
    CHECK_INT(s.last_rx_ms, t);
    CHECK_INT(s.last_enc_ms, t);
    CHECK_INT(s.last_enc_len, 20);

    /* counted as encoded, never as bad or unknown; the raw observer saw
     * each one. ENC_20 is a real REQINFO, so with the decoder in the
     * session each also decoded and counted as a frame in. */
    CHECK_INT(s.stream.enc_frames, 4);
    CHECK_INT(s.stream.enc_bytes, 80);
    CHECK_INT(s.stream.enc_bad, 0);
    CHECK_INT(s.enc_decoded, 4);
    CHECK_INT(s.enc_undecoded, 0);
    CHECK_INT(s.frames_in, 5);
    CHECK_INT(s.stream.frames_bad, 0);
    CHECK_INT(s.unknown_verbs, 0);
    CHECK_INT(seen.count, 4);
    CHECK_INT(seen.last_len, 20);
    CHECK_STR(seen.last, ENC_20);

    /* each decoded REQINFO was answered with WIFIINFO, like plaintext -
     * this is what keeps the dryer's WiFi screen from showing the adapter
     * as disconnected in the encoded mode */
    CHECK_INT(log.frames, 5);
    CHECK(strstr(log.buf, "WIFIINFO 5 81 \"MyNetwork\" 0 HR_aabbccddeeff 0 0 41\r") != NULL);

    /* the re-ask keeps going: FDNAME/REQCFG/STATUS are still unanswered
     * (only REQINFO frames arrived), and the heartbeat is what the main loop
     * runs while UP */
    memset(&log, 0, sizeof(log));
    hr_session_heartbeat(&s);
    CHECK_INT(log.frames, 4);
    CHECK(strstr(log.buf, "FDNAME\r") != NULL);
    CHECK(strstr(log.buf, "REQCFG\r") != NULL);
    CHECK(strstr(log.buf, "STATUS\r") != NULL);

    /* and the link still drops when the dryer is really gone */
    hr_session_tick(&s, t + HR_LINK_TIMEOUT_MS + 1000);
    CHECK_INT(s.link, HR_LINK_DOWN);
}

static void test_encoded_partial_is_stale_like_any_other(void)
{
    /*
     * The declared length never completes. The session's stale rule (1.5 s
     * with nothing pending finished) throws it away as "enc partial" and the
     * next header is taken clean - no frames_bad, no lost frame after it.
     */
    TEST_CASE("encoded partial frame times out and the next one is clean");
    tx_log_t log = {0};
    enc_seen_t seen = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_enc_observer(&s, enc_observer, &seen);

    hr_session_rx(&s, ENC_36, 20, 10000);       /* 16 short */
    CHECK_INT((int)s.stream.enc_need, 36);
    CHECK_INT(seen.count, 0);

    feed(&s, ENC_20, 12000);                    /* two seconds later */
    CHECK_INT(seen.count, 1);
    CHECK_STR(seen.last, ENC_20);
    CHECK_INT(s.stream.enc_bad, 1);
    CHECK_INT(s.stream.frames_bad, 0);
    CHECK_INT(s.link, HR_LINK_UP);              /* the complete one counts */

    /* a frame split across two transfers milliseconds apart is one frame */
    hr_session_rx(&s, ENC_36, 20, 13000);
    hr_session_rx(&s, ENC_36 + 20, 16, 13020);
    CHECK_INT(seen.count, 2);
    CHECK_STR(seen.last, ENC_36);
    CHECK_INT(s.stream.enc_bad, 1);
}

static void test_compat_on_still_answers_plaintext_reqinfo(void)
{
    /*
     * Regression guard for the WIFIINFO path. On the dryer, compat ON left
     * the machine's WiFi screen showing the adapter disconnected - because
     * REQINFO itself had moved into the encoded transport, so there was no
     * plaintext REQINFO to answer. The switch must not have suppressed the
     * answer itself: a 6.0.641041 machine with the option ON, or a 644170
     * machine before "UNIQUE lH" takes effect, still sends REQINFO in
     * plaintext and must still get WIFIINFO - with encoded frames arriving
     * around it or not.
     */
    TEST_CASE("compat ON: a plaintext REQINFO is still answered with WIFIINFO");
    tx_log_t log = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_compat(&s, true, true);
    hr_session_set_wifi(&s, 5, 81, "MyNetwork", "HR_aabbccddeeff");

    feed(&s, "REQINFO,\r", 37000);
    CHECK_INT(log.frames, 1);
    CHECK_STR(log.buf,
              "WIFIINFO 5 81 \"MyNetwork\" 0 HR_aabbccddeeff 0 0 37\r");

    /* between encoded frames, even glued to one in the same transfer. The
     * encoded one is itself a REQINFO (decoded), so two answers: one per
     * REQINFO, encoded or not. */
    memset(&log, 0, sizeof(log));
    feed(&s, ENC_20, 47000);
    CHECK_INT(log.frames, 1);
    memset(&log, 0, sizeof(log));
    char glued[80];
    snprintf(glued, sizeof(glued), "%sREQINFO,\r", ENC_20);
    feed(&s, glued, 57000);
    CHECK_INT(log.frames, 2);
    CHECK_STR(log.buf,
              "WIFIINFO 5 81 \"MyNetwork\" 0 HR_aabbccddeeff 0 0 57\r"
              "WIFIINFO 5 81 \"MyNetwork\" 0 HR_aabbccddeeff 0 0 57\r");
    CHECK_INT(s.stream.enc_frames, 2);
    CHECK_INT(s.enc_decoded, 2);
    CHECK_INT(s.frames_in, 4);
    CHECK_INT(s.stream.frames_bad, 0);

    /* and with the switch OFF, unchanged - the pinned 641041 behaviour */
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_compat(&s, false, false);
    hr_session_set_wifi(&s, 5, 81, "MyNetwork", "HR_aabbccddeeff");
    memset(&log, 0, sizeof(log));
    feed(&s, "REQINFO,\r", 37000);
    CHECK_INT(log.frames, 1);
    CHECK_STR(log.buf,
              "WIFIINFO 5 81 \"MyNetwork\" 0 HR_aabbccddeeff 0 0 37\r");
}

/*
 * Synthetic frames: plaintext of the shapes the dryer sends, encoded with the
 * same cipher hr_enc decodes (arbitrary nonces). Synthetic so no machine's
 * UID or serial number sits in a public test; the cipher itself is covered by
 * real captured REQINFO frames in test_hr_enc_decode.c.
 */
static const char SYN_UID[] =
    ")S$C-]^F2HN8-UA#XND8.LD'JEb<>B=S0@Y=X/T$-\\E1O4-A2&;,Y1FSF=(A0^'_9*0&#6[T"
    ":=Ca9E#\\E5bF21H$N1JOB)M$";
    /* UID,0-00000000-11111111-22222222,5,6.0.644170,0,5,3,1,132,186,65, */
static const char SYN_SNM[] = ")S#G.7--WJK%T+84R^6WJ=P6@0Z3(>?N;3!!";
    /* SNM,-System Name-, */
static const char SYN_CFG[] =
    ")S$+.P;TY,`^<RRA=:A<V^XVC_Z5M3L^W:B]EL:M@I;G0&3Ma?IOG`X+F7'WQa4LHb)7RS!!";
    /* CFG,0000-0000-0000,HL-6D~05,HLG0000000000000, */
static const char SYN_STAT[] =
    ")S$3/)J;GQ_:M_9SSA#3I#FaEK^_%(6D+<=FW?%6JL>b%AL<1@2KO9@<7JG&A=4V'1F79ZV>J@/"
    "$3'6F";
    /* STAT,4,0,0,0,52,48415,3494,0,45,Auto,1,20,0,0,5,0,0,, */

typedef struct {
    int count;
    char verbs[256];
    char last_raw[HR_MAX_FRAME];
} frames_seen_t;

static void frame_observer(const hr_frame_t *f, void *user)
{
    frames_seen_t *o = (frames_seen_t *)user;
    o->count++;
    size_t n = strlen(o->verbs);
    snprintf(o->verbs + n, sizeof(o->verbs) - n, "%s%s", n ? " " : "", f->verb);
    snprintf(o->last_raw, sizeof(o->last_raw), "%s", f->raw);
}

static void test_encoded_frames_decode_into_the_plaintext_path(void)
{
    /*
     * The other half of the 6.0.644170 story. Framing alone kept the link up
     * but left the dryer's answers unread: the re-ask ran forever, the
     * plaintext observer never saw a STAT, and the encoded REQINFO went
     * unanswered. With hr_enc in the session, an encoded frame that decodes
     * is handled exactly as if the dryer had sent the plaintext: info
     * fields, have_stat, WIFIINFO, the observer, frames_in.
     */
    TEST_CASE("encoded frames decode and take the plaintext path");
    tx_log_t log = {0};
    enc_seen_t raw = {0};
    frames_seen_t seen = {0};
    hr_session_t s;
    hr_session_init(&s, tx_capture, &log);
    hr_session_set_compat(&s, true, true);
    hr_session_set_enc_observer(&s, enc_observer, &raw);
    hr_session_set_observer(&s, frame_observer, &seen);
    hr_session_set_wifi(&s, 5, 81, "MyNetwork", "HR_aabbccddeeff");

    /* the hello reply, one burst: UID + SNM + CFG + STAT (+ REQINFO slot) */
    char burst[400];
    snprintf(burst, sizeof(burst), "%s%s%s%s%s", SYN_UID, SYN_SNM, SYN_CFG,
             SYN_STAT, ENC_20);
    feed(&s, burst, 5000);

    CHECK_INT(s.stream.enc_frames, 5);
    CHECK_INT(s.stream.enc_bad, 0);
    CHECK_INT(s.enc_decoded, 5);
    CHECK_INT(s.enc_undecoded, 0);
    CHECK_INT(raw.count, 5);                     /* raw ring/capture still fed */
    CHECK_INT(s.frames_in, 5);
    CHECK_INT(s.stream.frames_bad, 0);
    CHECK_INT(s.unknown_verbs, 0);
    CHECK_INT(s.link, HR_LINK_UP);

    /* the observer saw ordinary frames, in order, with plaintext raw */
    CHECK_INT(seen.count, 5);
    CHECK_STR(seen.verbs, "UID SNM CFG STAT REQINFO");
    CHECK_STR(seen.last_raw, "REQINFO,");

    /* dryer info filled in from the decoded frames */
    CHECK_STR(s.info.uid, "0-00000000-11111111-22222222");
    CHECK_STR(s.info.fw_version, "6.0.644170");
    CHECK_STR(s.info.serial, "-System Name-");
    CHECK_STR(s.info.dryer_sn, "HLG0000000000000");
    CHECK(s.info.have_stat);
    CHECK_STR(s.info.last_stat,
              "STAT,4,0,0,0,52,48415,3494,0,45,Auto,1,20,0,0,5,0,0,,");

    /* the encoded REQINFO was answered */
    CHECK_INT(log.frames, 1);
    CHECK_STR(log.buf, "WIFIINFO 5 81 \"MyNetwork\" 0 HR_aabbccddeeff 0 0 5\r");

    /* and the re-ask is over: everything it asked for has arrived */
    memset(&log, 0, sizeof(log));
    hr_session_heartbeat(&s);
    CHECK_INT(log.frames, 1);
    CHECK_STR(log.buf, "STATE 5 81\r");

    /* a frame that does not decode is counted and otherwise harmless: link
     * refreshed, raw observer fed, nothing parsed, nothing sent */
    memset(&log, 0, sizeof(log));
    feed(&s, ")S#7!!!!!!!!!!!!!!!!", 20000);   /* valid header, pad-only body */
    CHECK_INT(s.stream.enc_frames, 6);
    CHECK_INT(s.enc_decoded, 5);
    CHECK_INT(s.enc_undecoded, 1);
    CHECK_INT(s.frames_in, 5);
    CHECK_INT(s.stream.frames_bad, 0);
    CHECK_INT(raw.count, 6);
    CHECK_INT(seen.count, 5);
    CHECK_INT(log.frames, 0);
    CHECK_INT(s.last_rx_ms, 20000);
    CHECK_INT(s.link, HR_LINK_UP);

    /* plaintext firmware is untouched: no ")S", no decoder */
    feed(&s, "STAT,1,0,0,0,70,760000,0,0,0,Auto,0,0,0,0,0,0,0,,\r", 21000);
    CHECK_INT(s.stream.enc_frames, 6);
    CHECK_INT(s.frames_in, 6);
    CHECK_INT(seen.count, 6);
    CHECK_STR(s.info.last_stat,
              "STAT,1,0,0,0,70,760000,0,0,0,Auto,0,0,0,0,0,0,0,,");
}

int main(void)
{
    test_encoded_frames_decode_into_the_plaintext_path();
    test_encoded_frames_keep_the_link_up();
    test_encoded_partial_is_stale_like_any_other();
    test_compat_on_still_answers_plaintext_reqinfo();
    test_stale_partial_frame_does_not_prefix_reqinfo();
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
    test_recipe_verbs_refused_on_generic_path();
    test_heartbeat_is_only_state();
    test_compat_644170_reasks_until_answered();
    test_compat_644170_tags_unique();
    test_hello_is_one_burst();
    test_cloud_flags_reach_the_wire();
    test_manual_cloud_override_wins();
    test_captures_serial_number();
    test_captures_uid();
    test_records_latest_stat();
    test_link_comes_up_then_times_out();
    test_unknown_verb_is_counted_not_fatal();
    test_send_simple_emits_terminated_frame();
    test_observer_sees_inbound_frames();
    test_send_reports_transport_failure();
    return TEST_REPORT();
}
