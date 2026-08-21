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
              "\"elapsed_s\":0,\"mode\":\"QUALITY\",\"prep_s\":0,"
              "\"freeze_pct\":0,\"phase_pct\":0,\"phase_s\":0,"
              "\"vacuum_um\":0,\"vacuum_ok\":false}");
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

/* ---- freezing phase (STAT type 4), from a real capture ---- */
static void test_type4_is_freezing(void)
{
    TEST_CASE("type4 is freezing");
    hr_telemetry_t t;
    CHECK(parse("STAT,4,0,0,0,5,10000,6158,0,45,Auto,1,83,0,0,5,0,0,,\r", &t));
    CHECK(t.valid);
    CHECK_INT(t.type, 4);
    CHECK(t.freeze_active);
    CHECK_INT(t.temperature_f, 5);      /* chamber well below room temp */
    CHECK_INT(t.batch_elapsed_s, 6158);
    CHECK_STR(t.mode, "Auto");
    CHECK_INT(hr_phase_of(&t), HR_PHASE_FREEZING);
}

static void test_freeze_progress_rises_as_temp_falls(void)
{
    /*
     * Capture shows field[11] climbing 83 -> 84 -> 85 while the temperature
     * falls 5F -> 4F -> 3F. It is a progress percentage toward the freeze
     * target, NOT a mode code.
     */
    TEST_CASE("freeze progress rises as temp falls");
    hr_telemetry_t a, b, c;
    CHECK(parse("STAT,4,0,0,0,5,10000,6158,0,45,Auto,1,83,0,0,5,0,0,,\r", &a));
    CHECK(parse("STAT,4,0,0,0,4,10000,6339,0,45,Auto,1,84,0,0,5,0,0,,\r", &b));
    CHECK(parse("STAT,4,0,0,0,3,10000,6640,0,45,Auto,1,85,0,0,5,0,0,,\r", &c));

    CHECK_INT(a.freeze_pct, 83);
    CHECK_INT(b.freeze_pct, 84);
    CHECK_INT(c.freeze_pct, 85);
    CHECK(a.temperature_f > b.temperature_f);
    CHECK(b.temperature_f > c.temperature_f);
    CHECK(a.freeze_pct < c.freeze_pct);
}

static void test_freezing_counts_as_running_for_tracker(void)
{
    /* A freezing batch IS a running batch - the elapsed counter advances. */
    TEST_CASE("freezing counts as running");
    hr_phase_tracker_t tr;
    hr_telemetry_t t;
    hr_phase_tracker_init(&tr);
    feed(&tr, "STAT,4,0,0,0,5,10000,6158,0,45,Auto,1,83,0,0,5,0,0,,\r",
         10070000, &t);
    feed(&tr, "STAT,4,0,0,0,5,10000,6173,0,45,Auto,1,83,0,0,5,0,0,,\r",
         10085000, &t);
    CHECK_INT(hr_phase_of_tracked(&t, &tr), HR_PHASE_FREEZING);
    CHECK(tr.running);
}

static void test_freeze_label(void)
{
    TEST_CASE("freeze label");
    CHECK_STR(hr_phase_label(HR_PHASE_FREEZING), "Freezing");
}

/* ---- freeze ETA extrapolation ---- */
static void test_freeze_eta_needs_two_points(void)
{
    TEST_CASE("freeze eta needs two points");
    hr_phase_tracker_t tr;
    hr_telemetry_t t;
    hr_phase_tracker_init(&tr);
    feed(&tr, "STAT,4,0,0,0,5,10000,6158,0,45,Auto,1,83,0,0,5,0,0,,\r",
         10070000, &t);
    /* only one percent value seen so far - cannot estimate */
    CHECK_INT(hr_freeze_eta_s(&tr, &t), -1);
}

static void test_freeze_eta_from_real_rate(void)
{
    /*
     * Real capture: 83% at batch-elapsed 6158s, 85% at 6640s.
     * => 2 percent in 482s = 241 s/percent. 15 percent remain => ~3615s.
     */
    TEST_CASE("freeze eta from real rate");
    hr_phase_tracker_t tr;
    hr_telemetry_t t;
    hr_phase_tracker_init(&tr);
    feed(&tr, "STAT,4,0,0,0,5,10000,6158,0,45,Auto,1,83,0,0,5,0,0,,\r",
         10070000, &t);
    feed(&tr, "STAT,4,0,0,0,3,10000,6640,0,45,Auto,1,85,0,0,5,0,0,,\r",
         10552000, &t);

    long eta = hr_freeze_eta_s(&tr, &t);
    CHECK(eta > 3400 && eta < 3800); /* ~3615s */
}

static void test_freeze_eta_zero_at_100(void)
{
    TEST_CASE("freeze eta zero at 100");
    hr_phase_tracker_t tr;
    hr_telemetry_t t;
    hr_phase_tracker_init(&tr);
    feed(&tr, "STAT,4,0,0,0,5,10000,6158,0,45,Auto,1,98,0,0,5,0,0,,\r",
         10070000, &t);
    feed(&tr, "STAT,4,0,0,0,0,10000,6640,0,45,Auto,1,100,0,0,5,0,0,,\r",
         10552000, &t);
    CHECK_INT(hr_freeze_eta_s(&tr, &t), 0);
}

static void test_freeze_eta_absent_when_not_freezing(void)
{
    TEST_CASE("freeze eta absent when not freezing");
    hr_phase_tracker_t tr;
    hr_telemetry_t t;
    hr_phase_tracker_init(&tr);
    feed(&tr, "STAT,1,0,0,0,69,151882,0,0,38,0,1,QUALITY,v6.4,,\r", 1000, &t);
    CHECK_INT(hr_freeze_eta_s(&tr, &t), -1);
}

/* ---- drying phase, real pressure, phase-elapsed (2026-08-11 capture) ---- */
static void test_type5_is_drying(void)
{
    TEST_CASE("type5 is drying");
    hr_telemetry_t t;
    CHECK(parse("STAT,5,0,0,0,-18,440,10965,1,46,Auto,1,1,0,0,7,0,,\r", &t));
    CHECK(t.valid);
    CHECK_INT(t.type, 5);
    CHECK_INT(hr_phase_of(&t), HR_PHASE_DRYING);
    CHECK_INT(t.temperature_f, -18);
    CHECK_STR(t.mode, "Auto");
}

static void test_real_pressure_is_microns(void)
{
    /* Once the pump runs, [5] is a real vacuum reading, not the placeholder. */
    TEST_CASE("real pressure is microns");
    hr_telemetry_t a, b;
    CHECK(parse("STAT,4,0,0,0,-10,1209,10524,0,60,Auto,1,94,0,0,7,1,0,,\r", &a));
    CHECK(a.pressure_valid);
    CHECK_INT(a.pressure_microns, 1209);

    CHECK(parse("STAT,5,0,0,0,-18,440,10965,1,46,Auto,1,1,0,0,7,0,,\r", &b));
    CHECK(b.pressure_valid);
    CHECK_INT(b.pressure_microns, 440);
    CHECK(b.pressure_microns < a.pressure_microns); /* pulling down */
}

static void test_placeholder_pressure_is_flagged_invalid(void)
{
    /* 10000 means "pump off / no reading", not a 10000-micron vacuum. */
    TEST_CASE("placeholder pressure flagged invalid");
    hr_telemetry_t t;
    CHECK(parse("STAT,4,0,0,0,5,10000,6158,0,45,Auto,1,83,0,0,5,0,0,,\r", &t));
    CHECK(!t.pressure_valid);
    CHECK_INT(t.pressure_microns, 0);
    CHECK_INT(t.pressure_raw, 10000); /* raw value still available */
}

static void test_idle_atmospheric_reading_is_not_a_vacuum(void)
{
    /*
     * At rest the sensor reads ~120k-155k. That is not a 151882-micron
     * vacuum - it must not be presented as one.
     */
    TEST_CASE("idle atmospheric reading is not a vacuum");
    hr_telemetry_t t;
    CHECK(parse("STAT,1,0,0,0,69,151882,0,0,38,0,1,QUALITY,v6.4,,\r", &t));
    CHECK(!t.pressure_valid);
    CHECK_INT(t.pressure_microns, 0);
}

static void test_phase_elapsed_counter(void)
{
    /*
     * Field [8] counts seconds since the CURRENT phase began - it reset to 1
     * at the type-4 -> type-5 transition while batch elapsed kept climbing.
     */
    TEST_CASE("phase elapsed counter");
    hr_telemetry_t a, b;
    CHECK(parse("STAT,5,0,0,0,-18,440,10965,1,46,Auto,1,1,0,0,7,0,,\r", &a));
    CHECK_INT(a.batch_elapsed_s, 10965);
    CHECK_INT(a.phase_elapsed_s, 1);      /* just entered drying */

    CHECK(parse("STAT,5,0,0,0,-13,433,10999,35,46,Auto,1,1,0,0,7,0,,\r", &b));
    CHECK_INT(b.batch_elapsed_s, 10999);  /* +34 batch seconds */
    CHECK_INT(b.phase_elapsed_s, 35);     /* +34 phase seconds, in step */
}

static void test_phase_pct_tracks_vacuum_during_pulldown(void)
{
    /* [11] climbed 94 -> 99 as pressure fell 1209 -> 441 microns. */
    TEST_CASE("phase pct tracks vacuum pulldown");
    hr_telemetry_t a, b;
    CHECK(parse("STAT,4,0,0,0,-10,1209,10524,0,60,Auto,1,94,0,0,7,1,0,,\r", &a));
    CHECK(parse("STAT,4,0,0,0,-18,441,10961,0,60,Auto,1,99,0,0,7,1,0,,\r", &b));
    CHECK_INT(a.phase_pct, 94);
    CHECK_INT(b.phase_pct, 99);
    CHECK(b.pressure_microns < a.pressure_microns);
}

static void test_drying_label(void)
{
    TEST_CASE("drying label");
    CHECK_STR(hr_phase_label(HR_PHASE_DRYING), "Drying");
}

/* ---- vacuum unit conversions + comparisons ---- */
static void test_micron_conversions(void)
{
    /*
     * The dryer reports MICRONS (mTorr) - its own firmware says "HighmTorr".
     * 440 microns = 0.44 Torr = ~0.000587 bar.
     */
    TEST_CASE("micron conversions");
    CHECK(hr_microns_to_torr(440) > 0.439 && hr_microns_to_torr(440) < 0.441);
    CHECK(hr_microns_to_bar(440) > 0.00058 && hr_microns_to_bar(440) < 0.00060);
    CHECK(hr_microns_to_pascal(440) > 58.0 && hr_microns_to_pascal(440) < 59.5);
    /* sea level sanity: 760000 microns ~= 1 atm */
    CHECK(hr_microns_to_atm(760000) > 0.99 && hr_microns_to_atm(760000) < 1.01);
}

static void test_vacuum_comparison_tiers(void)
{
    TEST_CASE("vacuum comparison tiers");
    const char *d = NULL;
    /* a real freeze-drying vacuum is below Mars' surface pressure */
    const char *deep = hr_vacuum_comparison(440, &d);
    CHECK(deep != NULL && deep[0] != '\0');
    CHECK(d != NULL && d[0] != '\0');

    /* different levels should give different phrases */
    const char *atmos = hr_vacuum_comparison(700000, NULL);
    const char *mid   = hr_vacuum_comparison(5000, NULL);
    CHECK(strcmp(atmos, deep) != 0);
    CHECK(strcmp(atmos, mid) != 0);
    CHECK(strcmp(mid, deep) != 0);
}

static void test_vacuum_comparison_never_null(void)
{
    TEST_CASE("vacuum comparison never null");
    CHECK(hr_vacuum_comparison(0, NULL) != NULL);
    CHECK(hr_vacuum_comparison(-5, NULL) != NULL);
    CHECK(hr_vacuum_comparison(999999999, NULL) != NULL);
}

/* ---- final dry + complete (from the 25.6h full-cycle MQTT capture) ---- */
static void test_type6_is_final_dry(void)
{
    /* 12.9h stage: temp holds high (~124F) while vacuum goes deepest (204um) */
    TEST_CASE("type6 is final dry");
    hr_telemetry_t t;
    CHECK(parse("STAT,6,0,0,0,124,204,60000,100,46,Auto,1,50,0,0,7,0,,\r", &t));
    CHECK_INT(t.type, 6);
    CHECK_INT(hr_phase_of(&t), HR_PHASE_FINAL_DRY);
    CHECK_INT(t.temperature_f, 124);
    CHECK(t.pressure_valid);
    CHECK_INT(t.pressure_microns, 204);
}

static void test_type7_is_complete_and_vents(void)
{
    /* Venting: pressure jumps back to atmosphere (206 -> 148066 observed). */
    TEST_CASE("type7 is complete and vents");
    hr_telemetry_t t;
    CHECK(parse("STAT,7,0,0,0,92,148066,70000,200,46,Auto,1,1,0,0,7,0,,\r", &t));
    CHECK_INT(t.type, 7);
    CHECK_INT(hr_phase_of(&t), HR_PHASE_COMPLETE);
    CHECK(!t.pressure_valid);
}

static void test_new_phase_labels(void)
{
    TEST_CASE("new phase labels");
    CHECK_STR(hr_phase_label(HR_PHASE_FINAL_DRY), "Final dry");
    CHECK_STR(hr_phase_label(HR_PHASE_COMPLETE), "Batch complete");
}

static void test_unconfirmed_types_do_not_invent_a_mode(void)
{
    /*
     * STAT is multiplexed on the type discriminator, and field [11] means
     * different things per screen. A panel walk showed the old code
     * reporting mode as "5" on type 2, "-15" on type 31 and "5" on type 15,
     * because it fell back to the type-1 layout for anything unrecognised.
     *
     * For an unconfirmed type the mode must be EMPTY. Showing a number as a
     * mode is worse than showing nothing.
     */
    TEST_CASE("unconfirmed STAT types report no mode");
    hr_frame_t f;
    hr_telemetry_t t;

    /* Type 31 (recipe settings): [11] held -15, previously shown as mode. */
    CHECK(hr_frame_parse("STAT,31,0,0,0,69,10000,0,0,38,1,-15,x,,", &f));
    CHECK(hr_telemetry_from_stat(&f, &t));
    CHECK_INT(t.type, 31);
    CHECK_STR(t.mode, "");

    /* Type 1 still decodes normally - the confirmed layout is untouched. */
    CHECK(hr_frame_parse("STAT,1,0,0,0,69,151637,0,0,38,1,1,Auto,v6.4,,", &f));
    CHECK(hr_telemetry_from_stat(&f, &t));
    CHECK_STR(t.mode, "Auto");

    /* Type 17 keeps mode at [9]. */
    CHECK(hr_frame_parse("STAT,17,0,0,0,64,10000,36,0,42,Auto,1,1,0,0,5,0,864,0,", &f));
    CHECK(hr_telemetry_from_stat(&f, &t));
    CHECK_STR(t.mode, "Auto");
}

int main(void)
{
    test_type6_is_final_dry();
    test_type7_is_complete_and_vents();
    test_new_phase_labels();
    test_micron_conversions();
    test_vacuum_comparison_tiers();
    test_vacuum_comparison_never_null();
    test_type5_is_drying();
    test_real_pressure_is_microns();
    test_placeholder_pressure_is_flagged_invalid();
    test_idle_atmospheric_reading_is_not_a_vacuum();
    test_phase_elapsed_counter();
    test_phase_pct_tracks_vacuum_during_pulldown();
    test_drying_label();
    test_freeze_eta_needs_two_points();
    test_freeze_eta_from_real_rate();
    test_freeze_eta_zero_at_100();
    test_freeze_eta_absent_when_not_freezing();
    test_type4_is_freezing();
    test_freeze_progress_rises_as_temp_falls();
    test_freezing_counts_as_running_for_tracker();
    test_freeze_label();
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
    test_unconfirmed_types_do_not_invent_a_mode();
    return TEST_REPORT();
}
