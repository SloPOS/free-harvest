/*
 * hr_session - link state machine for the dryer <-> adapter conversation.
 *
 * Sits above hr_protocol (framing) and below the transport (USB CDC). Has no
 * ESP-IDF dependencies so it is unit-testable on a host PC.
 *
 * NOTE ON UNVERIFIED FIELDS: the frame *shapes* below are taken from format
 * strings in the decoded v6 firmware and are reliable. The *semantics* of
 * individual fields (and what the dryer requires inside a GOTIT ack) are
 * inferred and must be confirmed against a USB capture of the genuine
 * adapter. Anything provisional is marked "UNVERIFIED".
 */
#ifndef HR_SESSION_H
#define HR_SESSION_H

#include "hr_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Link is considered down after this long without an inbound frame.
 *
 * MUST comfortably exceed the dryer's real frame cadence. It sends idle STAT
 * frames every ~15,021 ms - fractionally MORE than 15 s - so a 15,000 ms
 * timeout expired a few milliseconds before each frame arrived and the UI
 * flapped "disconnected"/"ready" every 15 seconds. Gaps are longer still
 * mid-run (76 s observed). 45 s gives three idle intervals of headroom while
 * still noticing a genuinely unplugged dryer promptly.
 */
#define HR_LINK_TIMEOUT_MS 45000UL

typedef enum {
    HR_LINK_DOWN = 0,
    HR_LINK_UP,
} hr_link_state_t;

/* Transport write callback. Must transmit all `len` bytes. */
typedef void (*hr_tx_fn)(const char *data, size_t len, void *user);

/* Optional observer invoked for every inbound frame (logging, WiFi relay). */
typedef void (*hr_observer_fn)(const hr_frame_t *f, void *user);

/* Everything we have learned about the attached dryer. */
typedef struct {
    char serial[32];   /* from SNM */
    char uid[64];      /* from UID field 0 */
    char fw_version[24]; /* from UID - UNVERIFIED field index */
    bool have_stat;
    char last_stat[HR_MAX_FRAME]; /* raw body of the most recent STAT */
} hr_dryer_info_t;

typedef struct {
    hr_stream_t stream;
    hr_tx_fn tx;
    void *tx_user;
    hr_observer_fn observer;
    void *observer_user;

    hr_link_state_t link;
    hr_dryer_info_t info;

    unsigned long now_ms;
    unsigned long last_rx_ms;
    unsigned long frames_in;
    unsigned long frames_out;
    unsigned long unknown_verbs;

    /*
     * Payload placed in the GOTIT ack. UNVERIFIED - the genuine adapter's
     * value must be captured with a USB sniffer. Defaults to a printable
     * identity string; override with hr_session_set_ack_payload().
     */
    char ack_payload[48];
} hr_session_t;

void hr_session_init(hr_session_t *s, hr_tx_fn tx, void *tx_user);

/* Register an observer for inbound frames (may be NULL). */
void hr_session_set_observer(hr_session_t *s, hr_observer_fn fn, void *user);

/* Override the GOTIT ack payload. */
void hr_session_set_ack_payload(hr_session_t *s, const char *payload);

/* Feed received bytes with a monotonic millisecond timestamp. */
void hr_session_rx(hr_session_t *s, const void *data, size_t n,
                   unsigned long now_ms);

/* Advance time without new data; drives the link timeout. */
void hr_session_tick(hr_session_t *s, unsigned long now_ms);

/* Finish and transmit a built frame. Returns false if it did not go out. */
bool hr_session_send(hr_session_t *s, hr_builder_t *b);

/* Convenience: send a verb-only command such as REQSTAT or BEEP. */
bool hr_session_send_simple(hr_session_t *s, const char *verb);

/*
 * Safety classification for outbound command verbs.
 *   HR_CMD_SAFE   - read-only queries + BEEP. No state change. CLICK is
 *                   NOT in this class: it is the control verb the app
 *                   uses to press on-screen buttons.
 *   HR_CMD_CONFIG - writes config/recipe/date/name. Recoverable, does NOT run
 *                   a cycle (remote cycle-start is not exposed by the firmware).
 *   HR_CMD_UNKNOWN- not allow-listed (incl. hardware verbs DUTY/HCS/SPC and
 *                   REBOOT); refused by default.
 *
 * The web/MQTT layers gate on these so hardware-control and unknown verbs can
 * never be sent, no matter what a client asks for.
 */
typedef enum {
    HR_CMD_UNKNOWN = 0,
    HR_CMD_SAFE,
    HR_CMD_CONFIG,
} hr_cmd_class_t;

/* Classify a verb. Case-sensitive; verb only (no fields). */
hr_cmd_class_t hr_cmd_classify(const char *verb);

/*
 * Send a verb-only command ONLY if it classifies as HR_CMD_SAFE. Returns
 * false (and sends nothing) for any other verb. This is the entry point the
 * web UI uses - the allow-list is enforced here, in tested code, not in the
 * HTTP handler.
 */
bool hr_session_send_safe(hr_session_t *s, const char *verb);

/*
 * Send a command with a pre-formatted argument string (comma-separated fields,
 * no verb, no CR), allowed only if the verb classifies as HR_CMD_SAFE or
 * HR_CMD_CONFIG. `args` may be NULL/empty for verb-only. Refuses (sends
 * nothing, returns false) for UNKNOWN/hardware verbs. The frame sent is
 * "VERB,args\r" (or "VERB\r" when args is empty).
 */
/* Send an already-formed frame verbatim. For payloads the field
 * builder would corrupt - see hr_recipe_build(). */
bool hr_session_send_raw(hr_session_t *s, const char *frame);

bool hr_session_send_config(hr_session_t *s, const char *verb,
                            const char *args);

#ifdef __cplusplus
}
#endif

#endif /* HR_SESSION_H */
