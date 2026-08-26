#include "hr_session.h"

#include <stdio.h>
#include <string.h>

#define HR_DEFAULT_ACK_PAYLOAD "ESP32ADAPTER"

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", src);
}

/*
 * Verbs the dryer is known to emit (from decoded firmware format strings).
 * Listed so genuinely unexpected traffic can be counted separately - that
 * counter is the signal that our understanding of the protocol is incomplete.
 */
static bool verb_is_known(const char *verb)
{
    static const char *const known[] = {
        "UID",    "STAT",    "NTFY",    "SNM",         "CFG",
        "SYSINF", "SYSPREF", "BATSUM",  "TESTSUM",     "TESTHST",
        "LIM",    "SCIRCP",  "FDEXT",   "FDFILELIST",  "FDFILEBLOCK",
        "REQINFO","GOTIT",   "WRST",    "DIS",         "KHNK",
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(verb, known[i]) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * Answer REQINFO with WIFIINFO, the way the genuine adapter does.
 *
 * Captured from a real HarvestRight adapter over USB:
 *
 *     ->  REQINFO,
 *     <-  WIFIINFO 2 0 "" 1 HR_aabbccddeeff 0 0 7
 *     <-  WIFIINFO 5 81 "MyNetwork" 1 HR_aabbccddeeff 0 1 37
 *
 * Fields: link, rssi, ssid, registered, ap name, unknown, cloud, uptime.
 *
 * We used to answer with GOTIT and a payload this file admitted was invented.
 * One dryer tolerated that and sent telemetry anyway; another did not, and sat
 * re-asking REQINFO every two seconds indefinitely - 1,300 frames received and
 * not one STAT among them. GOTIT is the DRYER's acknowledgement of a CLICK, so
 * sending it here was answering a question with someone else's answer.
 *
 * The SSID is quoted and the AP name is not, matching the capture. That is not
 * a stylistic choice: an SSID may contain spaces, and these frames are
 * space-delimited.
 */
static void send_wifiinfo(hr_session_t *s)
{
    hr_builder_t b;
    hr_build_begin(&b, "WIFIINFO");
    hr_build_int(&b, s->wifi.link);
    hr_build_int(&b, s->wifi.rssi);

    char quoted[35];
    snprintf(quoted, sizeof(quoted), "\"%s\"", s->wifi.ssid);
    hr_build_str(&b, quoted);

    hr_build_int(&b, s->wifi.registered ? 1 : 0);
    hr_build_str(&b, s->wifi.ap_name[0] ? s->wifi.ap_name : "HR");
    hr_build_int(&b, 0);
    hr_build_int(&b, s->wifi.cloud ? 1 : 0);
    hr_build_int(&b, (long)(s->now_ms / 1000));
    hr_session_send(s, &b);
}

static void send_state(hr_session_t *s)
{
    hr_builder_t b;
    hr_build_begin(&b, "STATE");
    hr_build_int(&b, s->wifi.link);
    hr_build_int(&b, s->wifi.rssi);
    hr_session_send(s, &b);
}

void hr_session_heartbeat(hr_session_t *s)
{
    if (s != NULL) {
        send_state(s);
    }
}

void hr_session_hello_step(hr_session_t *s, unsigned step)
{
    if (s == NULL) {
        return;
    }
    switch (step) {
    case 0: send_state(s); break;
    case 1: hr_session_send_simple(s, "UNIQUE"); break;
    case 2: hr_session_send_simple(s, "FDNAME"); break;
    case 3: hr_session_send_simple(s, "REQCFG"); break;
    default: break;
    }
}

void hr_session_hello(hr_session_t *s)
{
    if (s == NULL) {
        return;
    }
    /*
     * Order matters: it is the order the genuine adapter uses. UNIQUE asks the
     * dryer who it is, FDNAME reads its name file, REQCFG reads its recipe
     * configuration - so after this exchange we know the machine, and the
     * dryer has been told an adapter is present.
     */
    send_state(s);
    hr_session_send_simple(s, "UNIQUE");
    hr_session_send_simple(s, "FDNAME");
    hr_session_send_simple(s, "REQCFG");
}

void hr_session_set_cloud(hr_session_t *s, bool registered, bool cloud)
{
    if (s == NULL) {
        return;
    }
    s->wifi.cloud_override = true;
    s->wifi.registered = registered;
    s->wifi.cloud = cloud;
}

void hr_session_set_cloud_auto(hr_session_t *s, bool online)
{
    if (s == NULL || s->wifi.cloud_override) {
        return;
    }
    s->wifi.registered = online;
    s->wifi.cloud = online;
}

void hr_session_set_wifi(hr_session_t *s, int link, int rssi,
                         const char *ssid, const char *ap_name)
{
    if (s == NULL) {
        return;
    }
    s->wifi.link = link;
    s->wifi.rssi = rssi;
    copy_str(s->wifi.ssid, sizeof(s->wifi.ssid), ssid);
    copy_str(s->wifi.ap_name, sizeof(s->wifi.ap_name), ap_name);
}

static void on_frame(const hr_frame_t *f, void *user)
{
    hr_session_t *s = (hr_session_t *)user;

    s->frames_in++;
    s->last_rx_ms = s->now_ms;
    s->link = HR_LINK_UP;

    if (s->observer != NULL) {
        s->observer(f, s->observer_user);
    }

    if (strcmp(f->verb, "REQINFO") == 0) {
        send_wifiinfo(s);
    } else if (strcmp(f->verb, "SNM") == 0) {
        copy_str(s->info.serial, sizeof(s->info.serial), hr_frame_field(f, 0));
    } else if (strcmp(f->verb, "CFG") == 0) {
        copy_str(s->info.dryer_sn, sizeof(s->info.dryer_sn),
                 hr_frame_field(f, 2));
    } else if (strcmp(f->verb, "UID") == 0) {
        copy_str(s->info.uid, sizeof(s->info.uid), hr_frame_field(f, 0));
        /* UNVERIFIED: field 2 looks like "maj.min.build". */
        copy_str(s->info.fw_version, sizeof(s->info.fw_version),
                 hr_frame_field(f, 2));
    } else if (strcmp(f->verb, "STAT") == 0) {
        if (hr_frame_tostring(f, s->info.last_stat, sizeof(s->info.last_stat)) >
            0) {
            s->info.have_stat = true;
        }
    } else if (!verb_is_known(f->verb)) {
        s->unknown_verbs++;
    }
}

void hr_session_init(hr_session_t *s, hr_tx_fn tx, void *tx_user)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    hr_stream_init(&s->stream);
    s->tx = tx;
    s->tx_user = tx_user;
    s->link = HR_LINK_DOWN;
    copy_str(s->ack_payload, sizeof(s->ack_payload), HR_DEFAULT_ACK_PAYLOAD);
}

void hr_session_set_observer(hr_session_t *s, hr_observer_fn fn, void *user)
{
    if (s == NULL) {
        return;
    }
    s->observer = fn;
    s->observer_user = user;
}

void hr_session_set_ack_payload(hr_session_t *s, const char *payload)
{
    if (s == NULL) {
        return;
    }
    copy_str(s->ack_payload, sizeof(s->ack_payload), payload);
}

void hr_session_rx(hr_session_t *s, const void *data, size_t n,
                   unsigned long now_ms)
{
    if (s == NULL) {
        return;
    }
    s->now_ms = now_ms;
    hr_stream_feed(&s->stream, data, n, on_frame, s);
}

void hr_session_tick(hr_session_t *s, unsigned long now_ms)
{
    if (s == NULL) {
        return;
    }
    s->now_ms = now_ms;
    if (s->link == HR_LINK_UP &&
        (now_ms - s->last_rx_ms) > HR_LINK_TIMEOUT_MS) {
        s->link = HR_LINK_DOWN;
    }
}

bool hr_session_send(hr_session_t *s, hr_builder_t *b)
{
    if (s == NULL || b == NULL || s->tx == NULL) {
        return false;
    }
    size_t len = 0;
    const char *wire = hr_build_finish(b, &len);
    if (wire == NULL) {
        return false;
    }
    s->tx(wire, len, s->tx_user);
    s->frames_out++;
    return true;
}

bool hr_session_send_simple(hr_session_t *s, const char *verb)
{
    hr_builder_t b;
    hr_build_begin(&b, verb);
    return hr_session_send(s, &b);
}

hr_cmd_class_t hr_cmd_classify(const char *verb)
{
    if (verb == NULL || verb[0] == '\0') {
        return HR_CMD_UNKNOWN;
    }
    /*
     * Explicit allow-list of verbs that only READ state or produce a benign
     * local effect (beep/click). Everything else - config writes, file ops,
     * hardware/duty-cycle control, REBOOT - is intentionally excluded so the
     * web UI cannot alter or endanger the dryer. Verified against the inbound
     * command dispatch table in the decoded v6 firmware.
     */
    static const char *const safe[] = {
        /* read-only queries */
        "REQSTAT", "REQSYSINF", "REQCFG", "REQPREF", "REQBATSUM", "REQTSUM",
        "REQTHST", "REQSCIENCE", "STATUS", "STATE", "SERIAL", "WIFIINFO",
        "MEMSIZE", "FDFILES",
        /*
         * FILEREAD - reads a file the dryer is serving.
         *
         * Honest basis: this is classified from the firmware's own verb
         * grouping (it sits among the queries) and from need, NOT from a
         * demonstration that it is inert. That is the same reasoning that
         * misfiled CLICK as benign and mislabelled SPC as a pump control.
         *
         * It is here because FDFILES - already allow-listed and proven
         * read-only - enumerates files but cannot fetch their contents,
         * and the batch history we want lives in those files. If FILEREAD
         * ever turns out to write, truncate or rename, it comes straight
         * back out. Reads are the only thing it is allowed to be.
         */
        "FILEREAD",
        /*
         * GETP / GETR - tested 2026-08-21 against a real machine and found
         * INERT: no reply bare or with page/row arguments, and no crash.
         * Kept allow-listed because they demonstrably do not act, but they
         * are not the screen-readout mechanism they looked like.
         *
         * The actual control verb is ADV, and it is deliberately NOT here -
         * see the note below.
         */
        "GETP", "GETR",
        /*
         * BEEP only. CLICK WAS HERE AND WAS THE CONTROL VERB - see the
         * note below. BEEP is genuinely inert: it sounds the buzzer and
         * replies "Thanks for Beeping!".
         */
        "BEEP",
    };
    for (size_t i = 0; i < sizeof(safe) / sizeof(safe[0]); i++) {
        if (strcmp(verb, safe[i]) == 0) {
            return HR_CMD_SAFE;
        }
    }
    /*
     * CLICK IS THE CONTROL VERB. It is not "a benign local effect", which is
     * what this list called it until 2026-08-21, and it shipped that way in
     * v0.3.5.
     *
     * Captured from the genuine app driving a real machine:
     *
     *     CLICK 1 10 54779 175300      <- Start
     *     CLICK 1  9 54780 175300      <- Custom
     *     CLICK 1  8 54781 175300      <- Candy
     *     CLICK 1  3 54782 175300      <- Config
     *
     * Fields are <screen> <button> <counter> <session>. Screen 1 is Ready, so
     * "CLICK 1 10 ..." starts a 24-hour cycle. The counter increments once per
     * distinct press and is reused across retries, which makes it an
     * idempotency key rather than a nonce.
     *
     * The exposure was real, not theoretical: hr_session_send_config() accepts
     * HR_CMD_SAFE as well as HR_CMD_CONFIG, and /api/cmd routes there whenever
     * args are present. So verb=CLICK&args=1,10,... from the web UI's raw
     * command box would have started a cycle.
     *
     * ADV IS ALSO NOT SAFE AND NOT CONFIG.
     *
     * Verified 2026-08-21: sending ADV to an idle machine moved it from Ready
     * (STAT type 1) to "Starting batch" (type 2), acknowledged by NTFY,2. It
     * acts as a press on whatever the panel currently offers, which is how the
     * stock app "mirrors every screen function".
     *
     * It therefore stays out of BOTH lists, so neither the web UI nor MQTT can
     * reach it. Exposing it needs a deliberate confirmation flow - starting a
     * 24-hour cycle by mis-tap is not an acceptable failure mode.
     */

    /*
     * Config/data writes: change settings, recipe, date, names. Recoverable
     * and, per firmware disassembly, none of these starts a drying cycle
     * (remote cycle-start is not exposed). Hardware-control verbs (DUTY, HCS,
     * SPC, XWIFI, FUZZY, MEMTEST) and REBOOT are deliberately excluded.
     */
    static const char *const config[] = {
        "SENDBATCH", "SENDCANDY", "SENDCUSTOM", "SENDSCIENCE",
        "SETPREF", "SETDATE", "SETBNAME", "FDNAME",
        /*
         * SETSN and FDRENAME WERE HERE, and both are on our own
         * never-probe list - SETSN overwrites the serial number,
         * FDRENAME renames files on the dryer. Listing a verb as too
         * dangerous to probe while leaving it sendable from /api/cmd is
         * not a policy, it is a gap.
         *
         * SETSN matters more than it looks now that the cloud binding is
         * understood: HarvestRight ties an adapter to a machine by its
         * identity, and their own app warns that changing the recorded
         * code needs a support agent to undo. A stray SETSN is not a
         * setting, it is a trip to support.
         *
         * Found 2026-08-21 while capturing FDRENAME "My Freeze Dryer" from
         * the real app - the same week CLICK was found mis-classified.
         */
    };
    for (size_t i = 0; i < sizeof(config) / sizeof(config[0]); i++) {
        if (strcmp(verb, config[i]) == 0) {
            return HR_CMD_CONFIG;
        }
    }
    return HR_CMD_UNKNOWN;
}

bool hr_session_send_safe(hr_session_t *s, const char *verb)
{
    if (hr_cmd_classify(verb) != HR_CMD_SAFE) {
        return false; /* refuse anything not explicitly allow-listed */
    }
    return hr_session_send_simple(s, verb);
}

/*
 * Send a frame that is already fully formed.
 *
 * The recipe frames are the reason this exists. Their payload is ONE argument
 * that is already quoted and comma-separated internally, so putting it through
 * hr_build_str() - which quotes empty fields and separates on spaces - would
 * take a valid recipe apart into fields the dryer reads as a different recipe.
 *
 * There is no allow-list check here because there is no verb to check: the
 * caller has already built the frame. Everything reaching this must have been
 * validated by whatever built it, which for recipes is hr_recipe_build().
 */
bool hr_session_send_raw(hr_session_t *s, const char *frame)
{
    if (s == NULL || frame == NULL || frame[0] == '\0' || s->tx == NULL) {
        return false;
    }
    size_t len = strlen(frame);
    if (len + 2 >= HR_MAX_FRAME) {
        return false;
    }
    char buf[HR_MAX_FRAME];
    memcpy(buf, frame, len);
    buf[len] = '\r';          /* same terminator every outbound frame uses */
    s->tx(buf, len + 1, s->tx_user);
    s->frames_out++;
    return true;
}

/*
 * The recipe verbs carry ONE quoted CSV argument plus a counter:
 *
 *     SENDCANDY "4,70,140,150,160,300,7200,300,CANDY,0," 100001
 *
 * The generic field builder below splits args on ',' and emits each piece as
 * its own space-delimited field, producing:
 *
 *     SENDCANDY 4 70 140 150 160 300 7200 300 CANDY 0
 *
 * That is not a malformed frame the dryer rejects - it is a DIFFERENT, still
 * parseable recipe. Sent to a machine about to run, it sets temperatures and
 * times nobody chose, and reports success. A wrong recipe that looks accepted
 * is worse than an error, so these verbs are refused rather than reshaped.
 *
 * The correct path is hr_recipe_build() + hr_session_send_raw(), which
 * validates the recipe, rejects names that collide with other verbs under the
 * dryer's substring matching, and emits the quoted form. /api/recipes/send and
 * /api/recipes/apply already use it.
 */
static bool is_recipe_verb(const char *verb)
{
    static const char *const recipe[] = {
        "SENDCANDY", "SENDCUSTOM", "SENDBATCH", "SENDSCIENCE",
    };
    for (size_t i = 0; i < sizeof(recipe) / sizeof(recipe[0]); i++) {
        if (strcmp(verb, recipe[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool hr_session_send_config(hr_session_t *s, const char *verb, const char *args)
{
    hr_cmd_class_t cls = hr_cmd_classify(verb);
    if (cls != HR_CMD_SAFE && cls != HR_CMD_CONFIG) {
        return false; /* hardware/unknown verbs are never sent */
    }
    if (verb != NULL && is_recipe_verb(verb)) {
        return false; /* see is_recipe_verb - must go through hr_recipe */
    }
    hr_builder_t b;
    hr_build_begin(&b, verb);
    if (args != NULL && args[0] != '\0') {
        /*
         * args is a pre-formatted comma-separated field list. Append it as a
         * raw run of fields: split on ',' so each becomes a proper field
         * (preserving empties), which keeps the framing consistent.
         */
        const char *p = args;
        for (;;) {
            const char *comma = strchr(p, ',');
            if (comma == NULL) {
                hr_build_str(&b, p);
                break;
            }
            char field[HR_MAX_FRAME];
            size_t len = (size_t)(comma - p);
            if (len >= sizeof(field)) {
                return false;
            }
            memcpy(field, p, len);
            field[len] = '\0';
            hr_build_str(&b, field);
            p = comma + 1;
        }
    }
    return hr_session_send(s, &b);
}
