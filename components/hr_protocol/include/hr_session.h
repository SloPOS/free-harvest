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

/*
 * A frame that began but has not been terminated after this long is not a
 * frame. Real frames are under 200 bytes and arrive in one USB transfer;
 * REQINFO itself repeats every 1-2 s. What this catches is bytes a host sent
 * BEFORE it spoke the protocol (a flasher probing the port, terminal noise),
 * which otherwise sit in the buffer and prefix the next real frame - on the
 * bench that turned the dryer's first REQINFO into an unknown verb.
 */
#define HR_PARTIAL_STALE_MS 1500UL

typedef enum {
    HR_LINK_DOWN = 0,
    HR_LINK_UP,
} hr_link_state_t;

/*
 * Transport write callback. Returns true only if all `len` bytes were handed
 * to the host, so the caller (and the web UI behind it) can tell "sent" from
 * "queued into a FIFO nobody is draining".
 */
typedef bool (*hr_tx_fn)(const char *data, size_t len, void *user);

/* Optional observer invoked for every inbound frame (logging, WiFi relay). */
typedef void (*hr_observer_fn)(const hr_frame_t *f, void *user);

/*
 * Optional observer for every complete ENCODED frame (the 6.0.644170
 * transport - see hr_protocol.h). Gets the whole frame, header included,
 * verbatim. The session itself does nothing with the contents: it counts the
 * frame, notes its length and time, and treats it as proof the dryer is
 * still there.
 */
typedef void (*hr_enc_observer_fn)(const char *frame, size_t len, void *user);

/* Everything we have learned about the attached dryer. */
typedef struct {
    /*
     * NOT a serial number. SNM carries the user-set machine NAME - the
     * development dryer answers "My Freeze Dryer". The field keeps its name
     * because /api/state and the MQTT topics have published it as "serial"
     * since the beginning and renaming it would break consumers; the UI label
     * was the thing that was wrong.
     */
    char serial[32];   /* from SNM - a NAME, see above */

    /*
     * The actual serial, from CFG field 2: PSTF000000000XXX on the
     * development machine, which matches the data-plate form "P-STF ...".
     * Inferred from that pattern rather than confirmed against the vendor
     * app, so it is reported alongside the name rather than replacing it.
     */
    char dryer_sn[32]; /* from CFG field 2 */
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
    unsigned long last_rx_ms;   /* last complete frame */
    unsigned long last_byte_ms; /* last byte of any kind, for stale partials */
    unsigned long frames_in;    /* plaintext frames parsed */
    unsigned long frames_out;
    unsigned long unknown_verbs;

    /*
     * Encoded transport, as seen by the session. Counts live in the stream
     * (stream.enc_frames / enc_bytes / enc_bad); these are the "when" and
     * "how big" of the most recent one, for /api/state. Every complete
     * encoded frame refreshes last_rx_ms (link up, heartbeat and re-ask keep
     * going), is handed raw to enc_observer, and is then DECODED (hr_enc):
     * a frame that decodes to a parseable line goes through exactly the
     * plaintext path - frames_in, REQINFO -> WIFIINFO, SNM/CFG/UID/STAT
     * bookkeeping, the observer - so a 6.0.644170 dryer behaves like the
     * plaintext firmware from here on. enc_decoded / enc_undecoded count the
     * two outcomes.
     */
    unsigned long last_enc_ms;  /* now_ms when the last encoded frame completed; 0 = never */
    size_t        last_enc_len; /* its declared (= actual) total length */
    unsigned long enc_decoded;  /* encoded frames that decoded and parsed */
    unsigned long enc_undecoded;/* encoded frames the decoder or parser refused */
    hr_enc_observer_fn enc_observer;
    void *enc_observer_user;
    /* Decode scratch. Kept off the USB task's stack; the session is fed from
     * one task only. */
    char enc_plain[HR_MAX_FRAME];
    hr_frame_t enc_frame;

    /*
     * Payload placed in the GOTIT ack. UNVERIFIED - the genuine adapter's
     * value must be captured with a USB sniffer. Defaults to a printable
     * identity string; override with hr_session_set_ack_payload().
     */
    char ack_payload[48];

    /*
     * What we tell the dryer about our network, when it asks with REQINFO.
     *
     * This is not cosmetic. Some dryer firmware will not proceed to send
     * telemetry until it gets a WIFIINFO it accepts - it just re-asks REQINFO
     * every two seconds forever, which is exactly what a user on a slightly
     * different firmware version saw: 1,300 frames in, every one of them
     * REQINFO, and never a single STAT.
     */
    struct {
        int  link;              /* association state, 1..5; 5 = connected    */
        int  rssi;              /* signal, 0 when not associated            */
        char ssid[33];
        char ap_name[33];       /* our own soft-AP name                     */
        /*
         * registered = field 3, cloud = field 6 of WIFIINFO.
         *
         * The dryer's own WiFi panel renders these: it shows the SSID from
         * field 2 but reports no server connection while field 6 is 0. Both
         * were hardcoded false for the life of this project because nothing
         * ever set them, so the dryer has never once been told the adapter
         * has internet.
         *
         * Captured from the genuine adapter:
         *     WIFIINFO 1 0 ""          0 HR_aabbccddeeff 0 0 161   early boot
         *     WIFIINFO 2 0 ""          1 HR_aabbccddeeff 0 0 7     claimed, no net
         *     WIFIINFO 5 81 "MyNetwork" 1 HR_aabbccddeeff 0 1 37   claimed + online
         */
        bool registered;        /* adapter claimed by a vendor account */
        bool cloud;             /* adapter can reach the vendor cloud  */
        bool cloud_override;    /* set by hand; stops the automatic rule */
    } wifi;

    /*
     * Handshake variants for dryer firmware 6.0.644170. Both OFF by default;
     * see hr_session_set_compat().
     */
    struct {
        bool unique_tag;   /* "UNIQUE lH" instead of a bare "UNIQUE" */
        bool reask;        /* re-send FDNAME/REQCFG/STATUS each heartbeat
                              until each has been answered */
    } compat;
} hr_session_t;

void hr_session_init(hr_session_t *s, hr_tx_fn tx, void *tx_user);

/* Register an observer for inbound frames (may be NULL). */
void hr_session_set_observer(hr_session_t *s, hr_observer_fn fn, void *user);

/* Register an observer for complete encoded frames (may be NULL). */
void hr_session_set_enc_observer(hr_session_t *s, hr_enc_observer_fn fn,
                                 void *user);

/*
 * Tell the session what to report in WIFIINFO.
 *
 * Call whenever the network state changes. `uptime_s` is not stored - the
 * frame carries seconds since boot, which the caller supplies at send time.
 */
/*
 * Introduce ourselves to the dryer, the way the genuine adapter does.
 *
 * Captured from a real HarvestRight adapter immediately after enumeration:
 *
 *     ADPT->DRYER  STATE 2 0
 *     ADPT->DRYER  UNIQUE      -> dryer answers UID
 *     ADPT->DRYER  FDNAME      -> dryer answers SNM
 *     ADPT->DRYER  REQCFG      -> dryer answers CFG
 *
 * Until now this firmware initiated NOTHING - it only ever answered. One dryer
 * streams telemetry regardless and never revealed the gap; another sends only
 * REQINFO forever and never starts, which is what this is meant to fix.
 *
 * Every verb here is a read or a status report. None changes machine state.
 */
void hr_session_hello(hr_session_t *s);

/* Number of frames in the opening handshake. */
#define HR_HELLO_STEPS 5

/*
 * Send ONE frame of the handshake, 0..HR_HELLO_STEPS-1, so the caller can
 * space them out.
 *
 * hr_session_hello() emits all four back to back, which one dryer accepts and
 * another does not. On a 6.0.644170 machine the burst went out inside 15ms and
 * only the SECOND frame was ever answered - UNIQUE returned its UID while
 * FDNAME and REQCFG drew nothing at all, repeatably. That is what a receive
 * path looks like when it takes a frame or two and discards the rest of what
 * arrived with them.
 *
 * The captured genuine adapter does not burst. It sends STATE, UNIQUE and
 * FDNAME, WAITS for the dryer's UID, and only then sends REQCFG. Pacing costs
 * a second of startup on a machine that never needed it, and is the difference
 * between working and not on one that does.
 */
void hr_session_hello_step(hr_session_t *s, unsigned step);

/*
 * Periodic STATE, which the genuine adapter emits roughly every 15 seconds.
 * The dryer's own panel shows link state and signal from these.
 */
void hr_session_heartbeat(hr_session_t *s);

/*
 * Set the two WIFIINFO connection flags. Separate from hr_session_set_wifi()
 * because they describe reachability BEYOND the local network, which the
 * association state cannot tell us.
 */
void hr_session_set_cloud(hr_session_t *s, bool registered, bool cloud);

/*
 * The automatic rule: report both flags set once the adapter is associated to
 * the user's network, clear when it is not.
 *
 * This mirrors the genuine adapter's observed transitions - 0/0 before it has
 * a network, 1/1 once it does. It is a statement about THIS adapter serving
 * the dryer, not a claim of registration with any vendor account, and it is
 * what stops a dryer sitting on its WiFi panel waiting to be told the link is
 * complete.
 *
 * Does nothing once hr_session_set_cloud() has been called, so a manual
 * override for diagnostics is not immediately undone by the main loop.
 */
void hr_session_set_cloud_auto(hr_session_t *s, bool online);

void hr_session_set_wifi(hr_session_t *s, int link, int rssi,
                         const char *ssid, const char *ap_name);

/*
 * Handshake variants for a dryer running firmware 6.0.644170.
 *
 * On 6.0.641041 the verb dispatcher and the executor have no notion of an
 * "adapter mode": UNIQUE answers UID, FDNAME answers SNM, STATUS answers STAT,
 * each unconditionally (G0641041 executor at 0x2b7e4, handlers 0x2cc68 /
 * 0x2cd20 / 0x2cdac). 6.0.644170 adds one: a mode byte that the USB layer
 * clears on attach, that the UNIQUE handler sets to 1 only when the frame's
 * first argument contains "lH" (the substring search runs on the parser's
 * argument slot 0 - 0x20003b00 is line buffer 0x20003f38 minus the 12 x 90
 * byte argument array, the same layout 641041 uses at 0x20003acc/0x20003f04),
 * and that the dispatcher consults to drop everything except UNIQUE / STATE /
 * WIFIINFO / FDNAME when it reads 2. A machine on that build answers a bare
 * UNIQUE with UID and then nothing - exactly the symptom that has been seen
 * on every 644170 machine so far, and the genuine adapter is captured sending
 * "UNIQUE lH" unprompted at power-up.
 *
 * `unique_tag` sends the argument. `reask` copies the genuine adapter's other
 * habit, measured against a simulator playing a stuck machine: FDNAME and
 * REQCFG go out again on every ~15 s heartbeat until answered, and STATUS
 * until a STAT has arrived. Both are verified harmless on 6.0.641041 (UID,
 * SNM, CFG, STAT arrive exactly as with the bare form) and both default OFF,
 * because upstream shipped them in 1.0.5.4-1.0.5.6 and backed them out again
 * without a conclusive test on a 644170 machine.
 */
void hr_session_set_compat(hr_session_t *s, bool unique_tag, bool reask);

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
