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

static void send_gotit(hr_session_t *s)
{
    /*
     * UNVERIFIED: the genuine adapter's GOTIT payload is unknown. The frame
     * shape "GOTIT,%s," comes from the firmware; the contents do not.
     */
    hr_builder_t b;
    hr_build_begin(&b, "GOTIT");
    hr_build_str(&b, s->ack_payload);
    hr_build_str(&b, "");
    hr_session_send(s, &b);
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
        send_gotit(s);
    } else if (strcmp(f->verb, "SNM") == 0) {
        copy_str(s->info.serial, sizeof(s->info.serial), hr_frame_field(f, 0));
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
         * GETP / GETR - tested 2026-08-21 against a real machine and found
         * INERT: no reply bare or with page/row arguments, and no crash.
         * Kept allow-listed because they demonstrably do not act, but they
         * are not the screen-readout mechanism they looked like.
         *
         * The actual control verb is ADV, and it is deliberately NOT here -
         * see the note below.
         */
        "GETP", "GETR",
        /* benign local effects (no state change) */
        "BEEP", "CLICK",
    };
    for (size_t i = 0; i < sizeof(safe) / sizeof(safe[0]); i++) {
        if (strcmp(verb, safe[i]) == 0) {
            return HR_CMD_SAFE;
        }
    }
    /*
     * ADV IS NOT SAFE AND IS NOT CONFIG. It is the control verb.
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
        "SETPREF", "SETDATE", "SETBNAME", "SETSN", "FDNAME", "FDRENAME",
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

bool hr_session_send_config(hr_session_t *s, const char *verb, const char *args)
{
    hr_cmd_class_t cls = hr_cmd_classify(verb);
    if (cls != HR_CMD_SAFE && cls != HR_CMD_CONFIG) {
        return false; /* hardware/unknown verbs are never sent */
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
