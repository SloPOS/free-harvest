#include "hr_telemetry.h"
#include "test_util.h"

static bool parse(const char *line, hr_telemetry_t *t)
{
    hr_frame_t f;
    if (!hr_frame_parse(line, &f)) {
        return false;
    }
    return hr_telemetry_from_stat(&f, t);
}

static void test_type1_idle_frame(void)
{
    /* Real capture: idle, QUALITY mode. temp=69, pressure=151882, elapsed=0 */
    TEST_CASE("type1 idle frame");
    hr_telemetry_t t;
    CHECK(parse("STAT,1,0,0,0,69,151882,0,0,38,0,1,QUALITY,v6.4,,\r", &t));
    CHECK(t.valid);
    CHECK_INT(t.type, 1);
    CHECK_INT(t.temperature_f, 69);
    CHECK_INT(t.pressure_raw, 151882);
    CHECK_INT(t.batch_elapsed_s, 0);
    CHECK_STR(t.mode, "QUALITY");
    CHECK(!t.prep_active);
}

static void test_type1_running_captures_elapsed(void)
{
    /* Later in run: elapsed=193, mode=Auto */
    TEST_CASE("type1 running elapsed");
    hr_telemetry_t t;
    CHECK(parse("STAT,1,0,0,0,69,127527,193,0,38,1,1,Auto,v6.4,,\r", &t));
    CHECK_INT(t.batch_elapsed_s, 193);
    CHECK_STR(t.mode, "Auto");
}

static void test_type17_prep_countdown(void)
{
    /* Prep frame: field[16] = seconds remaining (900 -> 0). */
    TEST_CASE("type17 prep countdown");
    hr_telemetry_t t;
    CHECK(parse("STAT,17,0,0,0,69,10000,127,0,42,Auto,1,15,0,0,5,0,773,0,\r", &t));
    CHECK(t.valid);
    CHECK_INT(t.type, 17);
    CHECK(t.prep_active);
    CHECK_INT(t.prep_remaining_s, 773);
    CHECK_INT(t.temperature_f, 69);
    CHECK_STR(t.mode, "Auto");
}

static void test_rejects_non_stat(void)
{
    TEST_CASE("rejects non-STAT");
    hr_telemetry_t t;
    hr_frame_t f;
    CHECK(hr_frame_parse("NTFY,1,0, ,0,\r", &f));
    CHECK(!hr_telemetry_from_stat(&f, &t));
    CHECK(!t.valid);
}

static void test_rejects_short_frame(void)
{
    TEST_CASE("rejects short STAT");
    hr_telemetry_t t;
    hr_frame_t f;
    CHECK(hr_frame_parse("STAT,1,2\r", &f));
    CHECK(!hr_telemetry_from_stat(&f, &t));
}

static void test_json_output(void)
{
    TEST_CASE("json output");
    hr_telemetry_t t;
    parse("STAT,1,0,0,0,69,151882,0,0,38,0,1,QUALITY,v6.4,,\r", &t);
    char buf[256];
    size_t n = hr_telemetry_to_json(&t, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK_STR(buf,
              "{\"type\":1,\"temp_f\":69,\"pressure\":151882,"
              "\"elapsed_s\":0,\"mode\":\"QUALITY\",\"prep_s\":0}");
}

/* ---- phase detection (grounded in real captures) ---- */
static hr_phase_t phase_of_line(const char *line)
{
    hr_telemetry_t t;
    if (!parse(line, &t)) {
        return HR_PHASE_UNKNOWN;
    }
    return hr_phase_of(&t);
}

static void test_phase_idle(void)
{
    TEST_CASE("phase idle");
    CHECK_INT(phase_of_line("STAT,1,0,0,0,69,151882,0,0,38,0,1,QUALITY,v6.4,,\r"),
              HR_PHASE_IDLE);
}

static void test_phase_preparing(void)
{
    TEST_CASE("phase preparing");
    CHECK_INT(phase_of_line(
                  "STAT,17,0,0,0,69,10000,127,0,42,Auto,1,15,0,0,5,0,773,0,\r"),
              HR_PHASE_PREPARING);
}

static void test_phase_running(void)
{
    /* type 1 but with a non-zero batch elapsed = batch under way */
    TEST_CASE("phase running");
    CHECK_INT(phase_of_line("STAT,1,0,0,0,69,127527,193,0,38,1,1,Auto,v6.4,,\r"),
              HR_PHASE_RUNNING);
}

static void test_phase_transition_and_views(void)
{
    TEST_CASE("phase transition and views");
    CHECK_INT(phase_of_line(
                  "STAT,2,0,0,0,69,10000,171,0,43,Auto,1,15,0,0,5,2,120,0,0,,\r"),
              HR_PHASE_TRANSITION);
    CHECK_INT(phase_of_line(
                  "STAT,31,0,0,0,69,141059,193,0,28,36087,16082,-10,0,120,120,"
                  "500,900,100,CUSTOM,90,14400,,\r"),
              HR_PHASE_RECIPE);
}

static void test_phase_labels(void)
{
    TEST_CASE("phase labels");
    CHECK_STR(hr_phase_label(HR_PHASE_IDLE), "Ready");
    CHECK_STR(hr_phase_label(HR_PHASE_PREPARING), "Preparing dryer");
    CHECK_STR(hr_phase_label(HR_PHASE_RUNNING), "Batch running");
    CHECK_STR(hr_phase_label(HR_PHASE_UNKNOWN), "Unknown");
}

/* ---- stateful running-detection (the elapsed-counter-freezes bug) ---- */
static void feed(hr_phase_tracker_t *tr, const char *line, unsigned long ms,
                 hr_telemetry_t *out)
{
    hr_frame_t f;
    hr_frame_parse(line, &f);
    hr_telemetry_from_stat(&f, out);
    hr_phase_tracker_update(tr, out, ms);
}

static void test_running_requires_advancing_elapsed(void)
{
    /*
     * Real capture: elapsed climbs 171->186->193 while running, then sits at
     * 193 for minutes while IDLE. Only the advancing part is "running".
     */
    TEST_CASE("running requires advancing elapsed");
    hr_phase_tracker_t tr;
    hr_telemetry_t t;
    hr_phase_tracker_init(&tr);

    feed(&tr, "STAT,1,0,0,0,69,127527,171,0,38,1,1,Auto,v6.4,,\r", 380000, &t);
    feed(&tr, "STAT,1,0,0,0,69,127527,186,0,38,1,1,Auto,v6.4,,\r", 395000, &t);
    CHECK_INT(hr_phase_of_tracked(&t, &tr), HR_PHASE_RUNNING);

    feed(&tr, "STAT,1,0,0,0,69,127527,193,0,38,1,1,Auto,v6.4,,\r", 404000, &t);
    CHECK_INT(hr_phase_of_tracked(&t, &tr), HR_PHASE_RUNNING);
}

static void test_stale_elapsed_becomes_idle(void)
{
    TEST_CASE("stale elapsed becomes idle");
    hr_phase_tracker_t tr;
    hr_telemetry_t t;
    hr_phase_tracker_init(&tr);

    feed(&tr, "STAT,1,0,0,0,69,127527,186,0,38,1,1,Auto,v6.4,,\r", 395000, &t);
    feed(&tr, "STAT,1,0,0,0,69,127527,193,0,38,1,1,Auto,v6.4,,\r", 404000, &t);
    CHECK_INT(hr_phase_of_tracked(&t, &tr), HR_PHASE_RUNNING);

    /* same elapsed repeatedly, as in the capture (193 held for minutes) */
    feed(&tr, "STAT,1,0,0,0,69,124331,193,0,38,1,1,Auto,v6.4,,\r", 419000, &t);
    feed(&tr, "STAT,1,0,0,0,69,119573,193,0,38,1,1,Auto,v6.4,,\r", 434000, &t);
    feed(&tr, "STAT,1,0,0,0,69,130854,193,0,38,1,1,Auto,v6.4,,\r", 479000, &t);
    /* >45s without the counter moving => no longer running */
    CHECK_INT(hr_phase_of_tracked(&t, &tr), HR_PHASE_IDLE);
    CHECK(!tr.running);
}

static void test_resumes_running_when_counter_moves_again(void)
{
    /* capture shows elapsed jumping 193 -> 265 later; that is running again */
    TEST_CASE("resumes running when counter moves");
    hr_phase_tracker_t tr;
    hr_telemetry_t t;
    hr_phase_tracker_init(&tr);

    feed(&tr, "STAT,1,0,0,0,69,127527,193,0,38,1,1,Auto,v6.4,,\r", 404000, &t);
    feed(&tr, "STAT,1,0,0,0,69,130854,193,0,38,1,1,Auto,v6.4,,\r", 609000, &t);
    CHECK_INT(hr_phase_of_tracked(&t, &tr), HR_PHASE_IDLE);

    feed(&tr, "STAT,1,0,0,0,69,151217,265,0,38,1,1,Auto,v6.4,,\r", 685000, &t);
    CHECK_INT(hr_phase_of_tracked(&t, &tr), HR_PHASE_RUNNING);
}

static void test_tick_can_expire_a_run(void)
{
    TEST_CASE("tick expires a run");
    hr_phase_tracker_t tr;
    hr_telemetry_t t;
    hr_phase_tracker_init(&tr);
    feed(&tr, "STAT,1,0,0,0,69,127527,186,0,38,1,1,Auto,v6.4,,\r", 100000, &t);
    feed(&tr, "STAT,1,0,0,0,69,127527,193,0,38,1,1,Auto,v6.4,,\r", 110000, &t);
    CHECK(tr.running);
    /* no frames at all for a long while */
    hr_phase_tracker_tick(&tr, 110000 + HR_RUN_STALE_MS + 1000);
    CHECK(!tr.running);
}

static void test_prep_still_detected_regardless_of_tracker(void)
{
    /* type 17 is prep no matter what the elapsed counter is doing */
    TEST_CASE("prep detected regardless of tracker");
    hr_phase_tracker_t tr;
    hr_telemetry_t t;
    hr_phase_tracker_init(&tr);
    feed(&tr, "STAT,17,0,0,0,69,10000,127,0,42,Auto,1,15,0,0,5,0,773,0,\r",
         50000, &t);
    CHECK_INT(hr_phase_of_tracked(&t, &tr), HR_PHASE_PREPARING);
}

int main(void)
{
    test_running_requires_advancing_elapsed();
    test_stale_elapsed_becomes_idle();
    test_resumes_running_when_counter_moves_again();
    test_tick_can_expire_a_run();
    test_prep_still_detected_regardless_of_tracker();
    test_phase_idle();
    test_phase_preparing();
    test_phase_running();
    test_phase_transition_and_views();
    test_phase_labels();
    test_type1_idle_frame();
    test_type1_running_captures_elapsed();
    test_type17_prep_countdown();
    test_rejects_non_stat();
    test_rejects_short_frame();
    test_json_output();
    return TEST_REPORT();
}
