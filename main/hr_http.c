#include "hr_capture.h"
#include "hr_control.h"
#include "hr_recipe.h"
#include "hr_batchstore.h"
#include "hr_http.h"
#include "hr_log.h"
#include "hr_mqtt.h"
#include "hr_reboot.h"
#include "hr_telemetry.h"
#include "hr_trend.h"
#include "hr_usb.h"
#include "hr_wifi.h"
#include "hr_compat.h"

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hr_http";

/* Embedded single-page UI (see main/www/index.html). */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* Phase artwork shown inside the progress ring (see main/www/img). */
extern const uint8_t idle_png_start[] asm("_binary_idle_png_start");
extern const uint8_t idle_png_end[] asm("_binary_idle_png_end");
extern const uint8_t freeze_png_start[] asm("_binary_freeze_png_start");
extern const uint8_t freeze_png_end[] asm("_binary_freeze_png_end");
extern const uint8_t dry_png_start[] asm("_binary_dry_png_start");
extern const uint8_t dry_png_end[] asm("_binary_dry_png_end");
extern const uint8_t heat_png_start[] asm("_binary_heat_png_start");
extern const uint8_t heat_png_end[] asm("_binary_heat_png_end");

static hr_session_t *s_session;
static hr_history_t *s_history;
static httpd_handle_t s_httpd;

/* Latest decoded telemetry, published by the frame observer in main.c. */
static hr_telemetry_t s_tel;
static bool s_tel_valid;
static uint32_t s_tel_ms; /* when it arrived, for the staleness check */
/*
 * Running/idle is decided by whether the dryer's elapsed counter is actually
 * ADVANCING - it retains the previous batch's value when idle, so a non-zero
 * elapsed alone does not mean a batch is running.
 */
static hr_phase_tracker_t s_tracker;
static bool s_tracker_ready;
/* 30s graph series, owned by main.c and guarded by the shared s_lock. */
static hr_trend_t *s_trend;
/* Recent encoded frames, owned by main.c, guarded by the shared s_lock. */
static hr_encring_t *s_encring;

/* Learns how often drying had to be extended - see hr_recipe.h. */
static hr_dry_tracker_t s_dry = {-1, 0};

int32_t hr_http_extra_dry_s(void)
{
    /* Called from on_inbound() on the USB task, outside the lock; the
     * tracker is two ints and the read is racy only against itself. */
    return hr_dry_extra_s(&s_dry);
}

/*
 * The frame history is written from the USB RX task and read from httpd
 * tasks, so guard it. The app supplies this mutex (it is the writer) via
 * hr_http_use_lock() so both sides serialise on one lock.
 *
 * The same lock covers s_tel / s_tracker / s_dry: they are ~80-100 byte
 * structs written by the USB task (and s_tracker by the main loop every
 * 250 ms) and read by httpd. Without it a reader could see half of one STAT
 * and half of the next - and live_screen() is what decides whether a CLICK
 * goes out. The setters may run before hr_http_use_lock(); they skip locking
 * until it exists.
 */
static SemaphoreHandle_t s_lock;
#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)
#define LOCK_IF() do { if (s_lock) LOCK(); } while (0)
#define UNLOCK_IF() do { if (s_lock) UNLOCK(); } while (0)

void hr_http_set_telemetry(const hr_telemetry_t *t)
{
    if (t != NULL && t->valid) {
        LOCK_IF();
        s_tel = *t;
        s_tel_valid = true;
        s_tel_ms = (uint32_t)(esp_timer_get_time() / 1000);
        /* Watch the SCREEN rather than our own commands: most
         * More Dry Time presses happen on the panel by hand. */
        hr_dry_observe(&s_dry, (int)t->type);
        UNLOCK_IF();
    }
}

void hr_http_set_trend(hr_trend_t *tr)
{
    s_trend = tr;
}

void hr_http_set_encring(hr_encring_t *r)
{
    s_encring = r;
}

void hr_http_set_tracker(const hr_phase_tracker_t *tr)
{
    if (tr != NULL) {
        LOCK_IF();
        s_tracker = *tr;
        s_tracker_ready = true;
        UNLOCK_IF();
    }
}

/* -------------------------------------------------------------------- */
/* Small helpers                                                         */
/* -------------------------------------------------------------------- */
static esp_err_t send_json(httpd_req_t *req, const char *json, size_t len)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, len);
}

/*
 * One scratch area for every handler that needs more than the 8 KB httpd
 * stack can carry.
 *
 * esp_http_server runs every handler on its single worker task, one after
 * another, so no two of these can ever be live at the same time - yet each
 * handler used to own a static buffer of its own: two 12.5 KB history
 * snapshots, a 9.6 KB verb table, a 5.7 KB recipe body, a 4.5 KB batch ring
 * and a 1 KB stream buffer, 46 KB of DRAM for a peak need of 12.5 KB. On a
 * board without PSRAM that difference is the whole Wi-Fi stack's working
 * heap. A union costs the largest member once. Nothing here survives the
 * handler that filled it, and nothing may: a handler that returns still
 * holding a pointer into s_scratch is a bug.
 */
#define RCP_SLOTS  8    /* recipe slots in NVS - see the recipe code below */
#define BATCH_SHOW 40   /* logbook entries /api/batches renders */
static union {
    hr_hist_entry_t hist[HR_HIST_CAP];         /* h_history, h_capture */
    hr_hist_verb_t  verbs[HR_HIST_VERBS];      /* h_verbs */
    char            recipes[RCP_SLOTS * 700 + 64]; /* h_recipes */
    hr_batch_t      batches[BATCH_SHOW];       /* h_batches */
    char            bytes[1024];               /* h_capture, h_batches_csv */
    hr_encring_t    enc;                       /* h_enc snapshot, ~5.4 KB */
} s_scratch;

/* -------------------------------------------------------------------- */
/* GET /  -> embedded HTML                                               */
/* -------------------------------------------------------------------- */
static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

/* -------------------------------------------------------------------- */
/* GET /api/state                                                        */
/* -------------------------------------------------------------------- */
static const char *wifi_status_str(void)
{
    switch (hr_wifi_status()) {
    case HR_WIFI_CONNECTED: return "connected";
    case HR_WIFI_CONNECTING: return "connecting";
    case HR_WIFI_AP_SETUP: return "setup-ap";
    /* Associated, no address: the lease was lost and DHCP is not answering.
     * Deliberately not "connected" - nothing is reachable in this state. */
    case HR_WIFI_NO_IP: return "no-ip";
    default: return "booting";
    }
}

/*
 * Why the chip last started.
 *
 * A restart is invisible over WiFi unless you happen to catch a counter going
 * backwards, which is exactly how the STATUS crash on 2026-08-21 was found -
 * frames_in fell 49 -> 6 between two curls. Uptime and reset reason turn that
 * from a lucky observation into a reading, and the reason separates a firmware
 * panic from a brownout on the dryer's USB rail, which need opposite fixes.
 */
/* The mapping lives in hr_capture.c, which writes it into the log at boot;
 * one answer, so /api/state and the capture can never disagree. */
#define reset_reason_str hr_reset_reason_str

/* Defined with the rest of the control code further down; /api/state needs
 * it here to report whether control is switched on. */
static bool ctrl_enabled(void);
static bool pin_is_set(char *out, size_t cap);
static bool pin_present(void);
/* True while /api/state is leaving out a last_stat too big to send. */
static bool s_state_trimmed;
/* Defined with the PIN code below; the control endpoints above need it.
 * Returns true when the request may proceed, and has already sent the
 * refusal when it returns false. */
static bool pin_guard(httpd_req_t *req, const char *body);
static bool pin_guard_small(httpd_req_t *req);

static esp_err_t h_state(httpd_req_t *req)
{
    char ip[16], ssid[33], serial[64], uid[128];
    hr_wifi_ip(ip, sizeof(ip));
    hr_wifi_current_ssid(ssid, sizeof(ssid));
    hr_wifi_noip_stats_t noip;
    hr_wifi_noip_stats(&noip);

    char laststat[HR_MAX_FRAME * 2];
    LOCK();
    hr_json_escape(s_session->info.last_stat, laststat, sizeof(laststat));
    hr_json_escape(s_session->info.serial, serial, sizeof(serial));
    hr_json_escape(s_session->info.uid, uid, sizeof(uid));
    char dryer_sn[64];
    hr_json_escape(s_session->info.dryer_sn, dryer_sn, sizeof(dryer_sn));
    char fwver[48];
    hr_json_escape(s_session->info.fw_version, fwver, sizeof(fwver));
    unsigned long fin = s_session->frames_in, fout = s_session->frames_out;
    unsigned long unk = s_session->unknown_verbs;
    unsigned long bad = s_session->stream.frames_bad;
    /* The 6.0.644170 encoded transport: how much of it, and how recently. */
    unsigned long enc_frames = s_session->stream.enc_frames;
    unsigned long enc_bytes = s_session->stream.enc_bytes;
    unsigned long enc_bad = s_session->stream.enc_bad;
    unsigned long enc_last_ms = s_session->last_enc_ms;
    unsigned enc_last_len = (unsigned)s_session->last_enc_len;
    unsigned long enc_decoded = s_session->enc_decoded;
    unsigned long enc_undecoded = s_session->enc_undecoded;
    const char *link = s_session->link == HR_LINK_UP ? "up" : "down";
    uint32_t latest = hr_history_latest_seq(s_history);
    /* Snapshot the telemetry under the same lock its writers take. */
    hr_telemetry_t tel = s_tel;
    bool tel_valid = s_tel_valid;
    hr_phase_tracker_t tracker = s_tracker;
    bool tracker_ready = s_tracker_ready;
    UNLOCK();

    /* Cycle phase + live readings (empty/idle values when nothing seen yet). */
    hr_phase_t ph = tel_valid
                        ? hr_phase_of_tracked(&tel,
                                              tracker_ready ? &tracker : NULL)
                        : HR_PHASE_UNKNOWN;
    char mode_esc[32];
    hr_json_escape(tel_valid ? tel.mode : "", mode_esc, sizeof(mode_esc));

    /*
     * The actions the machine is offering RIGHT NOW. The UI renders from this
     * rather than from a hardcoded list, so a screen we have never captured
     * shows no buttons at all instead of guessed ones.
     */
    char acts[384];
    size_t ai = 0;
    acts[ai++] = '[';
    const hr_action_t *av[8];
    size_t an = hr_control_for_screen(tel_valid ? (int)tel.type : -1, av, 8);
    for (size_t i = 0; i < an; i++) {
        int w = snprintf(acts + ai, sizeof(acts) - ai,
                         "%s{\"name\":\"%s\",\"label\":\"%s\",\"sev\":%d}",
                         i ? "," : "", av[i]->name, av[i]->label,
                         (int)av[i]->sev);
        if (w < 0 || (size_t)w >= sizeof(acts) - ai) {
            break;
        }
        ai += (size_t)w;
    }
    if (ai < sizeof(acts) - 1) {
        acts[ai++] = ']';
    }
    acts[ai] = '\0';

    multi_heap_info_t heap;
    heap_caps_get_info(&heap, MALLOC_CAP_INTERNAL);

    /* Age of the last encoded frame in ms; -1 when none has arrived. */
    long enc_age_ms = -1;
    if (enc_frames > 0) {
        unsigned long now = (unsigned long)(esp_timer_get_time() / 1000);
        enc_age_ms = (long)(now - enc_last_ms);
    }

    bool pin_set = pin_present();
    char body[2432];
    int n = snprintf(body, sizeof(body),
                     "{\"link\":\"%s\",\"serial\":\"%s\",\"uid\":\"%s\","
                     "\"dryer_sn\":\"%s\","
                     /* The dryer's own firmware build, from UID field 2, and
                      * whether the 6.0.644170 handshake variant is on. */
                     "\"fw_version\":\"%s\",\"compat644170\":%s,"
                     "\"frames_in\":%lu,\"frames_out\":%lu,"
                     "\"unknown_verbs\":%lu,\"frames_bad\":%lu,"
                     /* Encoded transport (")S" + length, 6.0.644170 after
                      * "UNIQUE lH"): complete frames framed and stored,
                      * their bytes, frames abandoned as partial/cut, and the
                      * length and age of the most recent one. See /api/enc.
                      * enc_decoded / enc_undecoded: how many of those frames
                      * decoded to a plaintext line (and went through the
                      * normal path - they are in frames_in too) or did not. */
                     "\"enc_frames\":%lu,\"enc_bytes\":%lu,\"enc_bad\":%lu,"
                     "\"enc_last_len\":%u,\"enc_last_age_ms\":%ld,"
                     "\"enc_decoded\":%lu,\"enc_undecoded\":%lu,"
                     "\"latest_seq\":%" PRIu32 ",\"wifi\":\"%s\",\"ip\":\"%s\","
                     "\"ssid\":\"%s\","
                     /* The no-IP watchdog (hr_netwatch.h): the driver's
                      * view of the link, seconds associated without an
                      * address, and the remedies issued since boot. A
                      * "connected" wifi with sta_assoc:true and ip
                      * 0.0.0.0 is the bug this exists to make visible;
                      * it now reads wifi:"no-ip" with noip_s counting. */
                     "\"sta_assoc\":%s,\"noip_s\":%lu,\"noip_episodes\":%u,"
                     "\"noip_dhcp_restarts\":%u,\"noip_reconnects\":%u,"
                     "\"phase\":%d,\"phase_label\":\"%s\",\"have_tel\":%s,"
                     "\"temp_f\":%ld,\"pressure\":%ld,\"elapsed_s\":%ld,"
                     "\"prep_s\":%ld,\"mode\":\"%s\",\"stat_type\":%d,"
                     "\"freeze_pct\":%ld,\"freeze_eta_s\":%ld,"
                     "\"phase_pct\":%ld,\"phase_s\":%ld,"
                     "\"vacuum_um\":%ld,\"vacuum_ok\":%s,"
                     /* Seconds left in the pump purge the dryer runs before
                      * a defrost (STAT type 8); 0 on every other screen. */
                     "\"purge_s\":%ld,"
                     /* USB-level diagnostics: reachable over WiFi while the
                      * adapter is plugged into the dryer, which the serial
                      * console is not. See hr_usb.h for how to read them. */
                     "\"usb_mounted\":%s,\"usb_suspended\":%s,"
                     "\"usb_mounts\":%u,\"usb_rx_bytes\":%lu,"
                     /* Internal heap: free now, the low-water mark since
                      * boot, and the largest block still allocatable. The
                      * last two are what a slow leak and fragmentation
                      * look like from outside - see the 60 s heap line in
                      * main.c. */
                     "\"heap_free\":%u,\"heap_min\":%u,\"heap_largest\":%u,"
                     "\"uptime_s\":%lu,\"reset_reason\":\"%s\","
                     "\"control\":%s,\"actions\":%s,\"pin\":%s,",
                     link, serial, uid, dryer_sn, fwver,
                     hr_compat_644170() ? "true" : "false",
                     fin, fout, unk, bad,
                     enc_frames, enc_bytes, enc_bad, enc_last_len, enc_age_ms,
                     enc_decoded, enc_undecoded,
                     latest,
                     wifi_status_str(), ip, ssid,
                     noip.associated ? "true" : "false", noip.noip_s,
                     noip.episodes, noip.dhcp_restarts, noip.reconnects,
                     (int)ph, hr_phase_label(ph), tel_valid ? "true" : "false",
                     tel_valid ? tel.temperature_f : 0,
                     tel_valid ? tel.pressure_raw : 0,
                     tel_valid ? tel.batch_elapsed_s : 0,
                     tel_valid ? tel.prep_remaining_s : 0,
                     mode_esc, tel_valid ? tel.type : 0,
                     tel_valid ? tel.freeze_pct : 0,
                     (tel_valid && tracker_ready)
                         ? hr_freeze_eta_s(&tracker, &tel)
                         : -1,
                     tel_valid ? tel.phase_pct : 0,
                     tel_valid ? tel.phase_elapsed_s : 0,
                     tel_valid ? tel.pressure_microns : 0,
                     (tel_valid && tel.pressure_valid) ? "true" : "false",
                     tel_valid ? tel.purge_remaining_s : 0,
                     hr_usb_mounted() ? "true" : "false",
                     hr_usb_suspended() ? "true" : "false",
                     hr_usb_mount_events(), hr_usb_rx_bytes(),
                     (unsigned)heap.total_free_bytes,
                     (unsigned)heap.minimum_free_bytes,
                     (unsigned)heap.largest_free_block,
                     (unsigned long)(esp_timer_get_time() / 1000000),
                     reset_reason_str(), ctrl_enabled() ? "true" : "false",
                     acts, pin_set ? "true" : "false");
    /*
     * snprintf reports the length it WANTED, and until 1.2.1 this handler sent
     * that many bytes whether or not they fit - reading past body, into its own
     * stack. Everything above is bounded well under the buffer. last_stat is
     * not: up to ~1 KB escaped straight off the wire. So it goes last, on its
     * own, and is left out rather than cut when it will not fit - the dashboard
     * keeps working and only the recipe editor loses its seed for that reply.
     */
    if (n < 0 || n >= (int)sizeof(body)) {
        return httpd_resp_send_500(req);
    }
    size_t room = sizeof(body) - (size_t)n;
    /* Raw frame: the config screens carry the live recipe in fields we do not
     * decode here, and the editor seeds itself from what is on the panel
     * rather than from a remembered default. */
    int t = snprintf(body + n, room,
                     "\"last_stat\":\"%s\",\"version\":\"" FREEHARVEST_VERSION
                     "\"}", laststat);
    if (t < 0 || (size_t)t >= room) {
        if (!s_state_trimmed) { /* once per episode, not once per poll */
            ESP_LOGW(TAG, "/api/state: last_stat (%u bytes escaped) does not "
                          "fit; sending the state without it",
                     (unsigned)strlen(laststat));
        }
        s_state_trimmed = true;
        t = snprintf(body + n, room,
                     "\"last_stat\":\"\",\"version\":\"" FREEHARVEST_VERSION
                     "\"}");
        if (t < 0 || (size_t)t >= room) {
            return httpd_resp_send_500(req);
        }
    } else {
        s_state_trimmed = false;
    }
    return send_json(req, body, (size_t)n + (size_t)t);
}

/* -------------------------------------------------------------------- */
/* GET /api/history?since=N                                              */
/* -------------------------------------------------------------------- */
static uint32_t query_since(httpd_req_t *req)
{
    char q[48];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
            since = (uint32_t)strtoul(v, NULL, 10);
        }
    }
    return since;
}

static esp_err_t h_history(httpd_req_t *req)
{
    uint32_t since = query_since(req);
    hr_hist_entry_t *out = s_scratch.hist; /* too big for the stack */
    int n;

    LOCK();
    n = hr_history_since(s_history, since, out, HR_HIST_CAP);
    UNLOCK();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "[");
    char obj[HR_HIST_BODY * 3];
    for (int i = 0; i < n; i++) {
        size_t len = hr_hist_entry_json(&out[i], obj, sizeof(obj));
        if (len == 0) {
            continue;
        }
        if (i) {
            httpd_resp_sendstr_chunk(req, ",");
        }
        httpd_resp_send_chunk(req, obj, len);
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* -------------------------------------------------------------------- */
/* GET /api/verbs                                                        */
/* -------------------------------------------------------------------- */
static esp_err_t h_verbs(httpd_req_t *req)
{
    /*
     * Snapshot under the lock, stream outside it - the same shape as
     * h_history(). This handler used to hold s_lock across up to 48
     * httpd_resp_send_chunk() calls, each of which can sit in a 5 s send
     * timeout on a slow or vanished client. The USB RX task takes the same
     * lock with portMAX_DELAY for every frame, so one stuck browser stalled
     * the TinyUSB task, which the dryer's host stack treats as a dead device.
     */
    hr_hist_verb_t *snap = s_scratch.verbs; /* ~10 KB */
    LOCK();
    int nv = s_history->nverbs;
    if (nv > HR_HIST_VERBS) {
        nv = HR_HIST_VERBS;
    }
    memcpy(snap, s_history->verbs, (size_t)nv * sizeof(snap[0]));
    UNLOCK();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "[");

    for (int i = 0; i < nv; i++) {
        const hr_hist_verb_t *v = &snap[i];
        char body[HR_HIST_BODY * 2], verb[HR_MAX_VERB * 2];
        hr_json_escape(v->last_body, body, sizeof(body));
        hr_json_escape(v->verb, verb, sizeof(verb));
        char obj[HR_HIST_BODY * 3];
        int n = snprintf(obj, sizeof(obj),
                         "%s{\"verb\":\"%s\",\"count\":%" PRIu32
                         ",\"last_seq\":%" PRIu32 ",\"n\":%u,\"changed\":%"
                         PRIu32 ",\"last\":\"%s\"}",
                         i ? "," : "", verb, v->count, v->last_seq,
                         (unsigned)v->nfields, v->changed_mask, body);
        if (n < 0 || (size_t)n >= sizeof(obj)) {
            continue;
        }
        if (httpd_resp_send_chunk(req, obj, n) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* -------------------------------------------------------------------- */
/* GET /api/capture  -> plain-text log download                          */
/* -------------------------------------------------------------------- */
static esp_err_t h_capture(httpd_req_t *req)
{
    bool degraded = false;      /* the flash log was there and would not read */
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=hr_capture.txt");

    /*
     * Prefer the persistent flash log - it holds a whole cycle. Fall back to
     * the small RAM ring only if the capture partition is unavailable.
     */
    if (hr_capture_ready() && hr_capture_size() > 0) {
        void *h = hr_capture_open();
        ESP_LOGI(TAG, "capture download: ready=%d size=%u open=%s",
                 (int)hr_capture_ready(), (unsigned)hr_capture_size(),
                 h ? "ok" : "FAILED");
        if (h != NULL) {
            char *buf = s_scratch.bytes;
            const size_t bufsz = sizeof(s_scratch.bytes);
            int n;
            int first = hr_capture_read(h, buf, bufsz);
            if (first <= 0) {
                /*
                 * stat() said there were bytes and the file opened, yet it
                 * reads empty - SPIFFS metadata and data disagree. Returning
                 * the empty body here is what made this look like a working
                 * download of nothing for days. Fall through to the RAM ring
                 * instead, which at least has the recent frames.
                 */
                ESP_LOGE(TAG, "capture log stats %u bytes but reads empty - "
                              "falling back to the RAM ring",
                         (unsigned)hr_capture_size());
                hr_capture_close(h);
                degraded = true;
                goto ram_fallback;
            }
            if (httpd_resp_send_chunk(req, buf, first) != ESP_OK) {
                hr_capture_close(h);
                return ESP_FAIL;
            }
            while ((n = hr_capture_read(h, buf, bufsz)) > 0) {
                if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
                    hr_capture_close(h);
                    return ESP_FAIL;
                }
            }
            hr_capture_close(h);
            return httpd_resp_sendstr_chunk(req, NULL);
        }
    }

ram_fallback:;
    /*
     * SAY SO. This path serves the small in-memory ring - minutes, not hours.
     * It used to do that silently, so a download of a 1.6 MB log arrived as
     * sixty lines of idle chatter and looked like the whole log. Somebody sent
     * that file on as a full capture of a 28-hour run, because nothing in it
     * said otherwise.
     */
    if (degraded) {
        static const char k_warn[] =
            "### INCOMPLETE - THIS IS NOT THE FULL LOG ###\n"
            "### The flash capture log exists but could not be read,\n"
            "### so what follows is only the in-memory ring: the last\n"
            "### few minutes, not the run you are looking for.\n"
            "### Power-cycle the adapter and download again - the\n"
            "### flash log usually reads cleanly after a restart.\n"
            "###\n";
        httpd_resp_send_chunk(req, k_warn, sizeof(k_warn) - 1);
    }

    hr_hist_entry_t *out = s_scratch.hist;
    int n;
    LOCK();
    n = hr_history_since(s_history, 0, out, HR_HIST_CAP);
    UNLOCK();

    char line[HR_HIST_BODY + 32];
    for (int i = 0; i < n; i++) {
        int len = snprintf(line, sizeof(line), "%" PRIu32 "\t%" PRIu32 "\t%s\n",
                           out[i].seq, out[i].t_ms, out[i].body);
        httpd_resp_send_chunk(req, line, len);
    }
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* GET /api/capture/info -> {"ready":..,"bytes":..,"capacity":..} */
static esp_err_t h_capture_info(httpd_req_t *req)
{
    char body[256];
    int n = snprintf(body, sizeof(body),
                     "{\"ready\":%s,\"bytes\":%u,\"capacity\":%u,"
                     "\"active\":%u,\"segs\":[",
                     hr_capture_ready() ? "true" : "false",
                     (unsigned)hr_capture_size(),
                     (unsigned)hr_capture_capacity(),
                     hr_capture_seg_active());
    for (unsigned i = 0; i < hr_capture_seg_count() && n > 0 &&
                         n < (int)sizeof(body); i++) {
        n += snprintf(body + n, sizeof(body) - (size_t)n, "%s%u",
                      i ? "," : "", (unsigned)hr_capture_seg_bytes(i));
    }
    if (n > 0 && n < (int)sizeof(body)) {
        n += snprintf(body + n, sizeof(body) - (size_t)n, "]}");
    }
    return send_json(req, body, n);
}

/* POST /api/capture/clear -> erase the persistent log */
static esp_err_t h_capture_clear(httpd_req_t *req)
{
    if (!pin_guard_small(req)) {
        return ESP_OK;
    }
    bool ok = hr_capture_clear();
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                     ok ? 11 : 12);
}

/* -------------------------------------------------------------------- */
/* POST /api/usb/reattach -> force a USB detach/attach                   */
/* -------------------------------------------------------------------- */
/*
 * Recovery for a dropped dryer link that does not require power-cycling the
 * machine. Safe to use mid-batch: the adapter is a passive monitor (the
 * protocol exposes no cycle control at all), and the dryer already handles
 * detach/attach - that is exactly what it sees whenever we reboot for an OTA.
 */
/* -------------------------------------------------------------------- */
/* GET /api/enc -> the last encoded frames (6.0.644170 transport)         */
/* -------------------------------------------------------------------- */
/*
 * Read-only view of the ring main.c fills from the USB task:
 *
 *   {"frames":N,"bytes":B,"bad":X,"last_len":L,"last_age_ms":A,"held":K,
 *    "items":[{"ms":123456,"len":80,"raw":")S$3..."},...]}
 *
 * `frames`/`bytes`/`bad` are the stream's lifetime counters (same as
 * /api/state), `held` is how many frames the ring has, oldest first in
 * `items`. `raw` is the frame exactly as received, header included; `trunc`
 * appears (true) only if the ring kept fewer bytes than `len` - the capture
 * log always has the whole frame. Nothing here decodes anything.
 *
 * Snapshot under the lock, stream outside it - the same shape as h_verbs(),
 * for the same reason: a stuck client must never hold the USB task.
 */
static esp_err_t h_enc(httpd_req_t *req)
{
    if (s_encring == NULL || s_session == NULL) {
        const char *empty = "{\"frames\":0,\"bytes\":0,\"bad\":0,"
                            "\"last_len\":0,\"last_age_ms\":-1,\"held\":0,"
                            "\"items\":[]}";
        return send_json(req, empty, strlen(empty));
    }

    hr_encring_t *snap = &s_scratch.enc;
    unsigned long frames, bytes, bad, last_ms;
    unsigned last_len;
    LOCK();
    *snap = *s_encring;
    frames = s_session->stream.enc_frames;
    bytes = s_session->stream.enc_bytes;
    bad = s_session->stream.enc_bad;
    last_ms = s_session->last_enc_ms;
    last_len = (unsigned)s_session->last_enc_len;
    UNLOCK();

    long age_ms = -1;
    if (frames > 0) {
        age_ms = (long)((unsigned long)(esp_timer_get_time() / 1000) - last_ms);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char head[192];
    int n = snprintf(head, sizeof(head),
                     "{\"frames\":%lu,\"bytes\":%lu,\"bad\":%lu,"
                     "\"last_len\":%u,\"last_age_ms\":%ld,\"held\":%u,"
                     "\"items\":[",
                     frames, bytes, bad, last_len, age_ms,
                     hr_encring_count(snap));
    if (n < 0 || (size_t)n >= sizeof(head) ||
        httpd_resp_send_chunk(req, head, n) != ESP_OK) {
        return ESP_FAIL;
    }

    unsigned held = hr_encring_count(snap);
    for (unsigned i = 0; i < held; i++) {
        hr_enc_rec_t rec;
        if (!hr_encring_get(snap, i, &rec)) {
            break;
        }
        /* Worst case every byte escapes to \uXXXX (6x) - the alphabet is
         * printable so in practice only backslash and quote ever do. */
        char raw[HR_ENCRING_RAW * 6 + 8];
        hr_json_escape(rec.raw, raw, sizeof(raw));
        char obj[sizeof(raw) + 80];
        int w = snprintf(obj, sizeof(obj),
                         "%s{\"ms\":%" PRIu32 ",\"len\":%u,%s\"raw\":\"%s\"}",
                         i ? "," : "", rec.t_ms, (unsigned)rec.len,
                         rec.kept < rec.len ? "\"trunc\":true," : "", raw);
        if (w < 0 || (size_t)w >= sizeof(obj)) {
            continue;
        }
        if (httpd_resp_send_chunk(req, obj, w) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* -------------------------------------------------------------------- */
/* GET /api/trend -> the 30s temperature/pressure series                 */
/* -------------------------------------------------------------------- */
/*
 * Streamed in chunks rather than built in one buffer: a full run is ~2600
 * points, and the httpd worker stack cannot hold that as a string.
 *
 * Downsampled to at most HR_TREND_MAX_POINTS by taking every Nth point, and the
 * stride is reported so the client can reconstruct real time. Phones get a small
 * payload; the full-resolution data stays available in the capture log.
 *
 * Gaps are emitted as JSON null, never as a fabricated value - a break in the
 * line is information.
 */
#define HR_TREND_MAX_POINTS 360

static esp_err_t h_trend(httpd_req_t *req)
{
    if (s_trend == NULL) {
        const char *empty = "{\"bucket_s\":30,\"stride\":1,\"n\":0,"
                            "\"temp\":[],\"smooth\":[],\"press\":[]}";
        return send_json(req, empty, strlen(empty));
    }

    LOCK();
    size_t total = hr_trend_count(s_trend);
    size_t stride = (total + HR_TREND_MAX_POINTS - 1) / HR_TREND_MAX_POINTS;
    if (stride == 0) {
        stride = 1;
    }
    size_t emitted = (total + stride - 1) / stride;
    UNLOCK();

    httpd_resp_set_type(req, "application/json");
    char head[128];
    int hn = snprintf(head, sizeof(head),
                      "{\"bucket_s\":%lu,\"stride\":%u,\"n\":%u,\"temp\":[",
                      (unsigned long)(HR_TREND_BUCKET_MS / 1000),
                      (unsigned)stride, (unsigned)emitted);
    if (httpd_resp_send_chunk(req, head, hn) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Three passes so each array streams without holding the whole series. */
    for (int pass = 0; pass < 3; pass++) {
        if (pass > 0) {
            const char *sep = (pass == 1) ? "],\"smooth\":[" : "],\"press\":[";
            if (httpd_resp_send_chunk(req, sep, strlen(sep)) != ESP_OK) {
                return ESP_FAIL;
            }
        }
        char buf[256];
        int n = 0;
        bool first = true;
        for (size_t i = 0; i < total; i += stride) {
            hr_trend_point_t pt;
            LOCK();
            bool ok = hr_trend_get(s_trend, i, &pt);
            UNLOCK();
            if (!ok) {
                break;
            }
            int w;
            if (pass == 0) {
                if (pt.temp_raw_f == HR_TREND_NO_TEMP) {
                    w = snprintf(buf + n, sizeof(buf) - n, "%snull",
                                 first ? "" : ",");
                } else {
                    w = snprintf(buf + n, sizeof(buf) - n, "%s%d",
                                 first ? "" : ",", (int)pt.temp_raw_f);
                }
            } else if (pass == 1) {
                if (pt.temp_smooth_cf == HR_TREND_NO_TEMP) {
                    w = snprintf(buf + n, sizeof(buf) - n, "%snull",
                                 first ? "" : ",");
                } else {
                    /* Hundredths of a degree; the client divides by 100. */
                    w = snprintf(buf + n, sizeof(buf) - n, "%s%d",
                                 first ? "" : ",", (int)pt.temp_smooth_cf);
                }
            } else {
                if (pt.pressure_raw == 0) {
                    w = snprintf(buf + n, sizeof(buf) - n, "%snull",
                                 first ? "" : ",");
                } else {
                    w = snprintf(buf + n, sizeof(buf) - n, "%s%lu",
                                 first ? "" : ",",
                                 (unsigned long)pt.pressure_raw);
                }
            }
            if (w < 0) {
                break;
            }
            first = false;
            n += w;
            if (n > (int)sizeof(buf) - 24) {
                /* Bail out on a vanished client. Without this the loop
                 * keeps pushing into a dead socket and the response is
                 * never terminated, leaking the socket - after
                 * max_open_sockets of those the server refuses everything. */
                if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
                    return ESP_FAIL;
                }
                n = 0;
            }
        }
        if (n > 0 && httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    if (httpd_resp_send_chunk(req, "]}", 2) != ESP_OK) {
        return ESP_FAIL;
    }
    /* Zero-length chunk terminates the response; required or the socket
     * stays open. */
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_usb_reattach(httpd_req_t *req)
{
    if (!pin_guard_small(req)) {
        return ESP_OK;
    }
    bool ok = hr_usb_bus_reattach();
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                     ok ? 11 : 12);
}

/* -------------------------------------------------------------------- */
/* GET /events  -> Server-Sent Events                                    */
/* -------------------------------------------------------------------- */
/*
 * ESP-IDF's httpd has a small worker pool; a long-lived SSE handler ties up
 * one worker. We keep the handler simple: it polls history for new frames
 * and writes them out, sending a heartbeat comment periodically. The socket
 * write returning an error tells us the client went away.
 */
/*
 * NOTE: an earlier version served frames via a long-lived SSE stream here.
 * ESP-IDF's httpd runs a SINGLE handler thread, so an infinite-loop handler
 * monopolises it and every other request (scan, wifi POST) queues behind it
 * forever. The browser now POLLS /api/history?since=N instead, which returns
 * immediately and cannot starve the server. This endpoint is kept only as a
 * fast redirect for any stale client still requesting it.
 */
static esp_err_t h_events(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

/* -------------------------------------------------------------------- */
/* WiFi setup endpoints                                                  */
/* -------------------------------------------------------------------- */
static esp_err_t h_scan(httpd_req_t *req)
{
    hr_wifi_scan_start();
    /* Return whatever the previous scan found; UI polls again shortly. */
    char json[1024];
    size_t n = hr_wifi_scan_result_json(json, sizeof(json));
    return send_json(req, json, n);
}

/*
 * Reads a small form body whole, or fails. `cap` must cover the largest legal
 * body: the previous version silently clipped the body to its buffer and
 * ignored ESP_ERR_HTTPD_RESULT_TRUNC from httpd_query_key_value(), so a
 * 63-character passphrase with a few percent-encoded characters was saved
 * truncated and the adapter sat at "connecting…" for good.
 */
static int read_form(httpd_req_t *req, char *buf, size_t cap)
{
    int total = req->content_len;
    if (total < 0 || total >= (int)cap) {
        return -1;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    buf[got] = '\0';
    return got;
}

/* A form field, or false if it is longer than `cap` allows (never clipped). */
static bool form_field(const char *body, const char *key, char *out,
                       size_t cap)
{
    esp_err_t e = httpd_query_key_value(body, key, out, cap);
    if (e == ESP_ERR_NOT_FOUND) {
        out[0] = '\0';
        return true;
    }
    if (e != ESP_OK) {
        out[0] = '\0';
        return false; /* ESP_ERR_HTTPD_RESULT_TRUNC or worse */
    }
    hr_url_decode(out);
    return true;
}

static esp_err_t h_wifi_post(httpd_req_t *req)
{
    /* SSID 32 bytes and passphrase 64, each up to 3x when percent-encoded,
     * plus keys and a PIN. */
    char buf[400];
    if (read_form(req, buf, sizeof(buf)) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"bad body\"}");
    }

    /* Pointing the adapter at another network is a takeover primitive, so it
     * sits behind the same PIN as the control endpoints. */
    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }

    char ssid[33 * 3 + 1] = {0}, pw[64 * 3 + 1] = {0};
    if (!form_field(buf, "ssid", ssid, sizeof(ssid)) ||
        !form_field(buf, "password", pw, sizeof(pw))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"reason\":\"value too long\"}");
    }

    if (!hr_wifi_set_credentials(ssid, pw)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    return send_json(req, "{\"ok\":true}", 11);
}

static esp_err_t h_forget(httpd_req_t *req)
{
    if (!pin_guard_small(req)) {
        return ESP_OK;
    }
    hr_wifi_forget();
    return send_json(req, "{\"ok\":true}", 11);
}

/*
 * POST /api/cmd  body: verb=REQSTAT
 * Sends a command to the dryer, but ONLY if hr_session_send_safe() accepts it
 * (read-only queries + BEEP). The allow-list is enforced in the tested
 * session layer; anything else is rejected here with 403.
 */
/* -------------------------------------------------------------------- */
/* POST /api/probe -> BENCH ONLY: send a verb bypassing the allow-list     */
/* -------------------------------------------------------------------- */
/*
 * Exists to map which verbs actually DO something, which cannot be learned
 * from an allow-list that refuses them. Compiled out by default and MUST stay
 * that way in anything released: it is a deliberate hole in the safety model
 * that otherwise keeps hardware-control verbs unreachable from the network.
 *
 * Enable only for a supervised bench session on an idle machine.
 */
#ifndef HR_ENABLE_PROBE
#define HR_ENABLE_PROBE 0
#endif

#if HR_ENABLE_PROBE
static esp_err_t h_probe(httpd_req_t *req)
{
    char buf[128];
    int total = req->content_len < (int)sizeof(buf) - 1 ? req->content_len
                                                        : (int)sizeof(buf) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            return httpd_resp_send_500(req);
        }
        got += r;
    }
    buf[got] = 0;

    char verb[HR_MAX_VERB] = {0}, args[64] = {0};
    httpd_query_key_value(buf, "verb", verb, sizeof(verb));
    httpd_query_key_value(buf, "args", args, sizeof(args));
    hr_url_decode(verb);
    hr_url_decode(args);

    hr_builder_t b;
    hr_build_begin(&b, verb);
    if (args[0]) {
        hr_build_str(&b, args);
    }
    bool ok = hr_session_send(s_session, &b);
    ESP_LOGW(TAG, "PROBE %s %s -> %s", verb, args, ok ? "sent" : "failed");
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                     ok ? 11 : 12);
}
#endif /* HR_ENABLE_PROBE */

static esp_err_t h_cmd(httpd_req_t *req)
{
    char buf[96];
    int total = req->content_len < (int)sizeof(buf) - 1 ? req->content_len
                                                        : (int)sizeof(buf) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            return httpd_resp_send_500(req);
        }
        got += r;
    }
    buf[got] = '\0';

    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }

    char verb[HR_MAX_VERB] = {0};
    httpd_query_key_value(buf, "verb", verb, sizeof(verb));
    hr_url_decode(verb);

    /*
     * Arguments were parsed by the UI and then silently dropped here, so the
     * raw-command box's args field did nothing at all. Several verbs are
     * useless without one - GETP/GETR almost certainly take a page or row
     * number - so pass them through.
     */
    char args[64] = {0};
    httpd_query_key_value(buf, "args", args, sizeof(args));
    hr_url_decode(args);

    /*
     * SAFE verbs are reads. CONFIG verbs are recoverable writes - the dryer's
     * clock, its preferences, its name - and the UI offers one directly:
     * Settings -> "Set dryer clock to this device's time" sends SETDATE.
     *
     * This gate admitted SAFE only, so every CONFIG verb came back "not
     * allowed" and that button had never once worked. The comment immediately
     * below has always said the class is re-checked inside
     * hr_session_send_config(), which only makes sense if CONFIG verbs were
     * expected to reach it. The gate and the code beneath it disagreed, and
     * the gate won silently.
     *
     * Widening it is not a net loosening. The recipe verbs that also live in
     * CONFIG are refused inside send_config(), because the generic field
     * builder reshapes them into a different VALID recipe rather than an
     * error. Hardware verbs, REBOOT, SETSN and FDRENAME are in neither class
     * and stay unreachable. pin_guard() above still applies to all of it.
     */
    hr_cmd_class_t cls = hr_cmd_classify(verb);
    if (cls != HR_CMD_SAFE && cls != HR_CMD_CONFIG) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"not allowed\"}");
    }
    /*
     * CONFIG verbs change the dryer's settings, clock and names. Settings says
     * nothing changes the machine until control is switched on; make that true
     * here as well, not only for /api/control. SAFE verbs are reads.
     */
    if (cls == HR_CMD_CONFIG && !ctrl_enabled()) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"control is disabled in settings\"}");
    }

    /* send_config carries the args and re-checks the class itself, so the
     * allow-list stays enforced in the tested core rather than here. */
    bool ok = (args[0] != '\0')
                  ? hr_session_send_config(s_session, verb, args)
                  : hr_session_send_safe(s_session, verb);
    ESP_LOGI(TAG, "web command %s -> %s", verb, ok ? "sent" : "failed");
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                     ok ? 11 : 12);
}

/*
 * POST /api/ota  body: raw firmware .bin (application/octet-stream)
 * Streams the image into the inactive OTA slot, validates it, sets it as the
 * next boot partition and reboots. Lets you update over WiFi without
 * unplugging the board from the dryer.
 *
 * The restart is deferred slightly so the HTTP 200 reaches the browser first,
 * and goes through hr_reboot_request() so the capture filesystem is quiesced
 * and unmounted before esp_restart() - see hr_capture_shutdown() for what an
 * unclean unmount, and an unmount under a busy writer, each did.
 */
static esp_err_t h_ota(httpd_req_t *req)
{
    /*
     * Firmware replacement is the one action that can undo every other
     * safeguard in this file - a hostile image is free to send DUTY/HCS/SPC.
     * The README has always said the PIN gates updates; now it does. The body
     * is the raw image, so the PIN arrives in the X-HR-Pin header.
     */
    if (!pin_guard(req, NULL)) {
        return ESP_OK;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"no ota slot\"}");
    }
    ESP_LOGI(TAG, "OTA start -> partition %s (%lu bytes incoming)",
             target->label, (unsigned long)req->content_len);

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"ota begin\"}");
    }

    char buf[1024];
    int remaining = req->content_len;
    int written = 0;
    int stalls = 0;
    bool checked = false;
    /*
     * The first bytes of an ESP image are the image header (24 bytes), the
     * first segment header (8 bytes) and then esp_app_desc_t - the same
     * structure esp_app_get_description() returns for the running build. The
     * head of the upload is accumulated here until that much has arrived, so
     * the project name can be compared before the image is accepted. A
     * random ESP32 binary starts with 0xE9 too; the name is what tells
     * "another build of this firmware" from "some other project".
     */
    uint8_t head[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                 sizeof(esp_app_desc_t)];
    size_t head_len = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < (int)sizeof(buf)
                                              ? remaining
                                              : (int)sizeof(buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            /*
             * Retry, but bounded. Each timeout is one recv_wait_timeout (5s by
             * default), so this tolerates ~2.5 minutes of a genuinely slow link
             * without spinning forever on a client that has silently vanished.
             */
            if (++stalls > 30) {
                esp_ota_abort(ota);
                ESP_LOGE(TAG, "OTA stalled after %d of %lu bytes", written,
                         (unsigned long)req->content_len);
                httpd_resp_set_status(req, "408 Request Timeout");
                return httpd_resp_sendstr(
                    req, "{\"ok\":false,\"reason\":\"upload stalled\"}");
            }
            continue;
        }
        stalls = 0;
        if (r <= 0) {
            esp_ota_abort(ota);
            ESP_LOGE(TAG, "OTA recv error after %d of %lu bytes", written,
                     (unsigned long)req->content_len);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(
                req, "{\"ok\":false,\"reason\":\"connection dropped mid-upload\"}");
        }
        /* Sanity-check the very first bytes look like an ESP app image. */
        if (!checked) {
            checked = true;
            if ((uint8_t)buf[0] != 0xE9) { /* ESP image magic */
                esp_ota_abort(ota);
                ESP_LOGE(TAG, "not an ESP firmware image (magic 0x%02x)",
                         (uint8_t)buf[0]);
                httpd_resp_set_status(req, "400 Bad Request");
                return httpd_resp_sendstr(
                    req, "{\"ok\":false,\"reason\":\"not a firmware image\"}");
            }
        }
        if (head_len < sizeof(head)) {
            size_t take = sizeof(head) - head_len;
            if ((size_t)r < take) {
                take = (size_t)r;
            }
            memcpy(head + head_len, buf, take);
            head_len += take;
            if (head_len == sizeof(head)) {
                const esp_app_desc_t *incoming =
                    (const esp_app_desc_t *)(head + sizeof(esp_image_header_t) +
                                             sizeof(esp_image_segment_header_t));
                const esp_app_desc_t *running = esp_app_get_description();
                if (incoming->magic_word != ESP_APP_DESC_MAGIC_WORD ||
                    strncmp(incoming->project_name, running->project_name,
                            sizeof(incoming->project_name)) != 0) {
                    esp_ota_abort(ota);
                    ESP_LOGE(TAG, "OTA image is not this project (magic 0x%08lx, "
                                  "name \"%.32s\")",
                             (unsigned long)incoming->magic_word,
                             incoming->project_name);
                    httpd_resp_set_status(req, "400 Bad Request");
                    return httpd_resp_sendstr(
                        req, "{\"ok\":false,\"reason\":\"image is not this "
                             "project\"}");
                }
            }
        }
        err = esp_ota_write(ota, buf, r);
        if (err != ESP_OK) {
            esp_ota_abort(ota);
            ESP_LOGE(TAG, "esp_ota_write at %d bytes: %s", written,
                     esp_err_to_name(err));
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"write\"}");
        }
        written += r;
        remaining -= r;
    }
    ESP_LOGI(TAG, "OTA received %d bytes, verifying image", written);

    if (head_len < sizeof(head)) {
        /* Too short to even carry an app descriptor - not a firmware image. */
        esp_ota_abort(ota);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"reason\":\"image too short\"}");
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"reason\":\"image invalid\"}");
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"set boot\"}");
    }

    ESP_LOGI(TAG, "OTA complete, will boot %s", target->label);
    send_json(req, "{\"ok\":true}", 11);
    hr_reboot_request("new firmware installed", 1200);
    return ESP_OK;
}

/*
 * GET (or POST) /api/log -> recent device log lines (debug view).
 *
 * Streamed in chunks, one line at a time. It used to build the whole document
 * in a single 8KB static buffer, which silently capped the response at roughly
 * 80 lines - about 45 seconds of a busy link - no matter how large the ring
 * buffer behind it was. Growing the ring alone would have changed nothing.
 *
 * POST is accepted as well as GET. The endpoint is read-only either way, and
 * the usual way to reach it is pasting a curl line next to the POST that sends
 * a command; getting the method wrong there returned a bare 405 that looked
 * like a firmware fault rather than a typo.
 */
static esp_err_t h_log(httpd_req_t *req)
{
    char line[HR_LOG_LINE_MAX];
    char esc[HR_LOG_LINE_MAX * 2 + 8];

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, "[", 1);

    size_t n = hr_log_count();
    for (size_t i = 0; i < n; i++) {
        if (!hr_log_line(i, line, sizeof(line))) {
            continue;
        }
        char body[HR_LOG_LINE_MAX * 2 + 4];
        hr_log_escape(line, body, sizeof(body));
        int len = snprintf(esc, sizeof(esc), "%s\"%s\"", i ? "," : "", body);
        if (len > 0) {
            httpd_resp_send_chunk(req, esc, (size_t)len);
        }
    }

    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/*
 * POST /api/wififlags  body: registered=0|1&cloud=0|1
 *
 * The two WIFIINFO fields the dryer's own WiFi panel renders as "connected to
 * HarvestRight". Both have been hardcoded 0 since the project started, so the
 * panel has never had anything to show. Exposed as a live toggle rather than a
 * compile-time constant so the effect can be watched on the machine's screen
 * without a reflash between attempts.
 *
 * This asserts something about reachability we do not actually verify, which
 * is why it is a deliberate switch and not a default.
 */
static esp_err_t h_wififlags(httpd_req_t *req)
{
    char buf[64];
    int total = req->content_len < (int)sizeof(buf) - 1 ? req->content_len
                                                        : (int)sizeof(buf) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            return httpd_resp_send_500(req);
        }
        got += r;
    }
    buf[got] = '\0';

    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }

    char reg[4] = {0}, cld[4] = {0};
    httpd_query_key_value(buf, "registered", reg, sizeof(reg));
    httpd_query_key_value(buf, "cloud", cld, sizeof(cld));

    bool registered = (reg[0] == '1');
    bool cloud = (cld[0] == '1');
    hr_session_set_cloud(s_session, registered, cloud);
    ESP_LOGW(TAG, "WIFIINFO flags set: registered=%d cloud=%d",
             (int)registered, (int)cloud);

    char out[64];
    int n = snprintf(out, sizeof(out),
                     "{\"ok\":true,\"registered\":%d,\"cloud\":%d}",
                     (int)registered, (int)cloud);
    return send_json(req, out, (size_t)n);
}

/*
 * GET  /api/compat                 -> {"compat644170":bool}
 * POST /api/compat  compat644170=0|1
 *
 * The 6.0.644170 handshake variant (hr_session_set_compat): "UNIQUE lH" and
 * the genuine adapter's re-ask cadence. Persisted in NVS; the main loop applies
 * it and re-introduces the adapter to the dryer, so an A/B test on a live
 * machine is one POST followed by a look at /api/capture for SNM / CFG / STAT.
 */
static esp_err_t h_compat_get(httpd_req_t *req)
{
    char out[48];
    int n = snprintf(out, sizeof(out), "{\"compat644170\":%s}",
                     hr_compat_644170() ? "true" : "false");
    return send_json(req, out, (size_t)n);
}

static esp_err_t h_compat_post(httpd_req_t *req)
{
    char buf[64];
    int total = req->content_len < (int)sizeof(buf) - 1 ? req->content_len
                                                        : (int)sizeof(buf) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            return httpd_resp_send_500(req);
        }
        got += r;
    }
    buf[got] = '\0';

    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }

    char v[4] = {0};
    if (httpd_query_key_value(buf, "compat644170", v, sizeof(v)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"compat644170=0|1 required\"}");
    }
    bool on = (v[0] == '1');
    bool stored = hr_compat_set_644170(on);

    char out[80];
    int n = snprintf(out, sizeof(out),
                     "{\"ok\":true,\"compat644170\":%s,\"stored\":%s}",
                     on ? "true" : "false", stored ? "true" : "false");
    return send_json(req, out, (size_t)n);
}

/*
 * POST /api/dryer/reboot   body: confirm=REBOOT
 *
 * The ONLY route that may send REBOOT, and it sends it with
 * hr_session_send_raw() - deliberately bypassing hr_cmd_classify(), which
 * places REBOOT in neither SAFE nor CONFIG.
 *
 * Why a dedicated route rather than adding REBOOT to a verb class: widening a
 * class would expose it to /api/cmd AND the MQTT command topic at once, and
 * MQTT has no confirmation step and no human in front of it. One explicit
 * route keeps the blast radius to exactly this handler, and keeps the raw
 * command box and MQTT unable to reach it at all.
 *
 * Guarded three ways: the control PIN, a literal confirm=REBOOT in the body so
 * a stray POST cannot trigger it, and a confirmation dialog in the UI that
 * says plainly the command is untested.
 */
static esp_err_t h_dryer_reboot(httpd_req_t *req)
{
    /* Rebooting the dryer mid-batch is about as state-changing as it gets;
     * it was the one control route that skipped the control switch. */
    if (!ctrl_enabled()) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"control is disabled in settings\"}");
    }
    char buf[96];
    int total = req->content_len < (int)sizeof(buf) - 1 ? req->content_len
                                                        : (int)sizeof(buf) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            return httpd_resp_send_500(req);
        }
        got += r;
    }
    buf[got] = '\0';

    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }

    char confirm[16] = {0};
    httpd_query_key_value(buf, "confirm", confirm, sizeof(confirm));
    if (strcmp(confirm, "REBOOT") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"confirmation required\"}");
    }

    LOCK();
    bool ok = hr_session_send_raw(s_session, "REBOOT");
    UNLOCK();
    ESP_LOGW(TAG, "DRYER REBOOT sent from the web UI -> %s",
             ok ? "sent" : "failed");
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                     ok ? 11 : 12);
}

/* GET /api/mqtt -> current broker/connection status. */
static esp_err_t h_mqtt_get(httpd_req_t *req)
{
    char json[256];
    size_t n = hr_mqtt_status_json(json, sizeof(json));
    return send_json(req, json, n);
}

/* POST /api/mqtt  body: host=..&port=..&user=..&password=.. */
static esp_err_t h_mqtt_post(httpd_req_t *req)
{
    char buf[640];
    if (read_form(req, buf, sizeof(buf)) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"bad body\"}");
    }

    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }

    /* Sized to what hr_mqtt stores (host 64, user 64, pass 96) times the
     * percent-encoding expansion, so a legal value is never clipped. */
    char host[64 * 3] = {0}, ports[8] = {0}, user[64 * 3] = {0},
         pass[96 * 3] = {0};
    if (!form_field(buf, "host", host, sizeof(host)) ||
        !form_field(buf, "port", ports, sizeof(ports)) ||
        !form_field(buf, "user", user, sizeof(user)) ||
        !form_field(buf, "password", pass, sizeof(pass))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"reason\":\"value too long\"}");
    }
    if (strlen(host) >= 64 || strlen(user) >= 64 || strlen(pass) >= 96) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"reason\":\"value too long\"}");
    }
    int port = ports[0] ? atoi(ports) : 1883;

    if (!hr_mqtt_set_broker(host, port, user, pass)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    return send_json(req, "{\"ok\":true}", 11);
}

/* GET /img/<name>.png -> embedded phase artwork (cached hard, never changes) */
static esp_err_t h_img(httpd_req_t *req)
{
    const uint8_t *start = NULL, *end = NULL;
    const char *u = req->uri;
    if (strstr(u, "idle")) { start = idle_png_start; end = idle_png_end; }
    else if (strstr(u, "freeze")) { start = freeze_png_start; end = freeze_png_end; }
    else if (strstr(u, "heat")) { start = heat_png_start; end = heat_png_end; }
    else if (strstr(u, "dry")) { start = dry_png_start; end = dry_png_end; }
    if (start == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    return httpd_resp_send(req, (const char *)start, end - start);
}

/* Captive-portal: redirect common probe URLs to the setup page. */
static esp_err_t h_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

/* -------------------------------------------------------------------- */
/* Control                                                               */
/* -------------------------------------------------------------------- */
#define CTRL_NVS_NS      "hrctrl"

/*
 * The trailing CLICK field. Captured as 175300 in every session over three
 * days, with counters in the 26k, 48k and 49k ranges, so it is a fixed
 * protocol constant and not a session token.
 */
#define HR_CLICK_SESSION 175300u

/*
 * Where our command counter starts.
 *
 * Retries from the real app reuse the same counter, so the dryer most likely
 * ignores a repeat of the last value rather than requiring strict monotonicity.
 * But that is inference, not measurement. Starting above every counter ever
 * observed (max 49060) is safe under BOTH readings: distinct from the last
 * value, and greater than it. Cheap insurance against a hypothesis we have not
 * tested.
 */
#define HR_SEQ_START     100000u

static bool ctrl_enabled(void)
{
    nvs_handle_t nh;
    if (nvs_open(CTRL_NVS_NS, NVS_READONLY, &nh) != ESP_OK) {
        return false; /* absent config means OFF, never on */
    }
    uint8_t v = 0;
    nvs_get_u8(nh, "on", &v);
    nvs_close(nh);
    return v != 0;
}

bool hr_http_control_enabled(void)
{
    return ctrl_enabled();
}

static bool ctrl_set_enabled(bool on)
{
    nvs_handle_t nh;
    if (nvs_open(CTRL_NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_u8(nh, "on", on ? 1 : 0) == ESP_OK &&
              nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    return ok;
}

/*
 * Next counter value, persisted so it does not restart after a reboot and
 * collide with values the dryer has already seen this power cycle.
 */
/*
 * Returns 0 if the counter could not be persisted. Callers must then refuse
 * to send: the dryer treats a repeated counter as a retry of the previous
 * press and ignores it, so a counter that silently restarted at HR_SEQ_START
 * on every call (the old behaviour when nvs_open failed) made every CLICK
 * after the first a no-op while the API kept answering ok:true.
 */
static uint32_t ctrl_next_seq(void)
{
    nvs_handle_t nh;
    uint32_t seq = HR_SEQ_START;
    esp_err_t err = nvs_open(CTRL_NVS_NS, NVS_READWRITE, &nh);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "click counter: nvs_open %s", esp_err_to_name(err));
        return 0;
    }
    if (nvs_get_u32(nh, "seq", &seq) != ESP_OK) {
        seq = HR_SEQ_START;
    }
    seq++;
    err = nvs_set_u32(nh, "seq", seq);
    if (err == ESP_OK) {
        err = nvs_commit(nh);
    }
    nvs_close(nh);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "click counter: could not persist %lu: %s",
                 (unsigned long)seq, esp_err_to_name(err));
        return 0;
    }
    return seq;
}

static esp_err_t refuse_no_counter(httpd_req_t *req)
{
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_sendstr(
        req, "{\"ok\":false,\"reason\":\"could not persist the command "
             "counter (NVS)\"}");
}

/*
 * The screen currently on the panel, or -1 when we genuinely do not know.
 *
 * "Know" means: a STAT has been decoded, the protocol link is up, the host has
 * us enumerated, and the STAT is recent. s_tel_valid alone is not enough - it
 * is set by the first STAT ever and never cleared, so after the dryer rebooted
 * or an hour of silence a CLICK was still built against the screen last seen.
 * Call under LOCK(); s_session->link is read here.
 */
static int live_screen(void)
{
    if (!s_tel_valid) {
        return -1;
    }
    if (s_session == NULL || s_session->link != HR_LINK_UP) {
        return -1;
    }
    if (!hr_usb_mounted()) {
        return -1;
    }
    uint32_t age = (uint32_t)(esp_timer_get_time() / 1000) - s_tel_ms;
    if (age > HR_LINK_TIMEOUT_MS) {
        return -1;
    }
    return (int)s_tel.type;
}

/*
 * Screens from which a recipe frame may carry the start flag. Ready (1) is
 * where the genuine app is used; the recipe editors (31 Custom, 43 Candy) are
 * where the setup panel seeds itself from. A run in progress (17, 2, 4-7),
 * diagnostics, or no live telemetry at all are refused: the effect of a
 * "start now" recipe on a machine already running is undocumented, and
 * without a live screen we cannot tell which case we are in.
 */
static bool recipe_start_allowed(int live)
{
    return live == 1 || live == 31 || live == 43;
}

/*
 * POST /api/control   action=<name>&screen=<believed>&confirm=<0|1>
 *
 * Never takes a button number. The caller names an action and states which
 * screen it was looking at; hr_control_check() refuses if the machine has moved
 * on, because button numbers mean different things on different screens - End
 * Batch is button 4 on Freezing and button 1 on Drying.
 */
static esp_err_t h_control(httpd_req_t *req)
{
    if (!ctrl_enabled()) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"control is disabled in settings\"}");
    }

    char buf[256];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"bad body\"}");
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            return httpd_resp_send_500(req);
        }
        got += r;
    }
    buf[got] = '\0';

    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }

    char action[32] = {0}, screen_s[8] = {0}, confirm_s[8] = {0};
    httpd_query_key_value(buf, "action", action, sizeof(action));
    httpd_query_key_value(buf, "screen", screen_s, sizeof(screen_s));
    httpd_query_key_value(buf, "confirm", confirm_s, sizeof(confirm_s));
    hr_url_decode(action);

    int believed = screen_s[0] ? atoi(screen_s) : -1;
    bool confirmed = (confirm_s[0] == '1');

    LOCK();
    int live = live_screen();
    UNLOCK();

    const hr_action_t *a = NULL;
    hr_ctrl_result_t r = hr_control_check(action, believed, live, confirmed, &a);
    if (r != HR_CTRL_OK) {
        char body[192];
        int n = snprintf(body, sizeof(body),
                         "{\"ok\":false,\"reason\":\"%s\",\"live_screen\":%d}",
                         hr_ctrl_result_str(r), live);
        /* A stale view or a missing confirmation is the caller's to resolve, so
         * 409 rather than 400 - it tells the UI to re-read and ask again. */
        httpd_resp_set_status(req, (r == HR_CTRL_STALE_VIEW ||
                                    r == HR_CTRL_NEEDS_CONFIRM)
                                       ? "409 Conflict" : "400 Bad Request");
        ESP_LOGW(TAG, "control %s refused: %s (believed %d, live %d)",
                 action, hr_ctrl_result_str(r), believed, live);
        return send_json(req, body, n);
    }

    /* One counter per press. Taken once - calling ctrl_next_seq() twice would
     * burn a value and, worse, send a different number than we logged. */
    uint32_t seq = ctrl_next_seq();
    if (seq == 0) {
        return refuse_no_counter(req);
    }

    LOCK();
    hr_builder_t b;
    hr_build_begin(&b, "CLICK");
    hr_build_int(&b, a->screen);
    hr_build_int(&b, a->button);
    hr_build_int(&b, (long)seq);
    hr_build_int(&b, (long)HR_CLICK_SESSION);
    bool ok = hr_session_send(s_session, &b);
    UNLOCK();

    ESP_LOGI(TAG, "control %s -> CLICK %d %d %lu %lu : %s", a->name, a->screen,
             a->button, (unsigned long)seq, (unsigned long)HR_CLICK_SESSION,
             ok ? "sent" : "failed");

    char body[160];
    int n = snprintf(body, sizeof(body),
                     "{\"ok\":%s,\"action\":\"%s\",\"screen\":%d,\"button\":%d}",
                     ok ? "true" : "false", a->name, a->screen, a->button);
    return send_json(req, body, n);
}

/* POST /api/control/enable   on=<0|1> */
static esp_err_t h_control_enable(httpd_req_t *req)
{
    char buf[64];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            return httpd_resp_send_500(req);
        }
        got += r;
    }
    buf[got] = '\0';
    /* Switching control on is itself a control action. */
    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }
    char on_s[8] = {0};
    httpd_query_key_value(buf, "on", on_s, sizeof(on_s));
    bool on = (on_s[0] == '1');
    bool ok = ctrl_set_enabled(on);
    ESP_LOGW(TAG, "remote control %s", on ? "ENABLED" : "disabled");
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                     ok ? 11 : 12);
}

/* -------------------------------------------------------------------- */
/* Recipes                                                               */
/* -------------------------------------------------------------------- */
#define RCP_NVS_NS "hrrcp"
/* RCP_SLOTS is defined with s_scratch near the top of the file. */

static bool rcp_load(int slot, hr_recipe_t *out)
{
    if (slot < 0 || slot >= RCP_SLOTS || out == NULL) {
        return false;
    }
    nvs_handle_t nh;
    if (nvs_open(RCP_NVS_NS, NVS_READONLY, &nh) != ESP_OK) {
        return false;
    }
    char k[8];
    snprintf(k, sizeof(k), "r%d", slot);
    size_t len = sizeof(*out);
    bool ok = nvs_get_blob(nh, k, out, &len) == ESP_OK && len == sizeof(*out);
    nvs_close(nh);
    return ok && out->used;
}

static bool rcp_store(int slot, const hr_recipe_t *r)
{
    if (slot < 0 || slot >= RCP_SLOTS) {
        return false;
    }
    nvs_handle_t nh;
    if (nvs_open(RCP_NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        return false;
    }
    char k[8];
    snprintf(k, sizeof(k), "r%d", slot);
    bool ok = true;
    if (r == NULL) {
        esp_err_t e = nvs_erase_key(nh, k);
        ok = (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND);
    } else {
        ok = nvs_set_blob(nh, k, r, sizeof(*r)) == ESP_OK;
    }
    ok = (nvs_commit(nh) == ESP_OK) && ok;
    nvs_close(nh);
    return ok;
}

static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    int total = req->content_len;
    if (total <= 0 || total >= (int)cap) {
        return -1;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    buf[got] = '\0';
    return got;
}

/* GET /api/recipes */
/*
 * One slot serialises to at most ~650 bytes (escaped 191-byte notes, ten
 * numbers). Each slot is rendered into its own buffer first and appended only
 * if it fits whole, so the static body can never be overrun. The earlier
 * version added snprintf's return value unchecked and tested the size *after*
 * writing a slot; a slot begun near the end of the buffer wrote past it and
 * send_json() then shipped the following .bss to the client.
 */
static esp_err_t h_recipes(httpd_req_t *req)
{
    char *body = s_scratch.recipes;
    const size_t bodysz = sizeof(s_scratch.recipes);
    size_t at = 0;
    int w = snprintf(body, bodysz, "{\"extra_dry_s\":%ld,\"slots\":[",
                     (long)hr_dry_extra_s(&s_dry));
    if (w < 0 || (size_t)w >= bodysz) {
        return httpd_resp_send_500(req);
    }
    at = (size_t)w;
    bool first = true;
    for (int i = 0; i < RCP_SLOTS; i++) {
        hr_recipe_t r;
        if (!rcp_load(i, &r)) {
            continue;
        }
        char nm[HR_RECIPE_NAME_MAX * 2 + 2], nt[HR_RECIPE_NOTES_MAX * 2 + 2];
        hr_json_escape(r.name, nm, sizeof(nm));
        hr_json_escape(r.notes, nt, sizeof(nt));
        char slot[700];
        int n = snprintf(slot, sizeof(slot),
                         "%s{\"slot\":%d,\"family\":%d,\"name\":\"%s\","
                         "\"notes\":\"%s\",\"runs\":%lu,\"nnum\":%u,"
                         "\"suggest_dry_s\":%ld,\"num\":[",
                         first ? "" : ",", i, (int)r.family, nm, nt,
                         (unsigned long)r.runs, (unsigned)r.nnum,
                         (long)hr_recipe_suggested_dry_s(&r, &s_dry));
        if (n < 0 || (size_t)n >= sizeof(slot)) {
            continue;
        }
        bool fits = true;
        for (uint8_t k = 0; k < r.nnum && k < HR_RECIPE_MAX_NUM; k++) {
            int m = snprintf(slot + n, sizeof(slot) - (size_t)n, "%s%ld",
                             k ? "," : "", (long)r.num[k]);
            if (m < 0 || (size_t)m >= sizeof(slot) - (size_t)n) {
                fits = false;
                break;
            }
            n += m;
        }
        if (!fits || (size_t)n + 2 >= sizeof(slot)) {
            continue;
        }
        slot[n++] = ']';
        slot[n++] = '}';
        slot[n] = '\0';
        /* Reserve the closing "]}" and the NUL. */
        if (at + (size_t)n + 3 > bodysz) {
            break;
        }
        memcpy(body + at, slot, (size_t)n);
        at += (size_t)n;
        first = false;
    }
    body[at++] = ']';
    body[at++] = '}';
    body[at] = '\0';
    return send_json(req, body, at);
}

/* POST /api/recipes/save   slot,family,name,notes,num=csv */
static esp_err_t h_recipe_save(httpd_req_t *req)
{
    char buf[1024];
    if (read_body(req, buf, sizeof(buf)) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"bad body\"}");
    }
    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }
    char slot_s[8] = {0}, fam_s[8] = {0}, nums[256] = {0};
    hr_recipe_t r;
    memset(&r, 0, sizeof(r));
    httpd_query_key_value(buf, "slot", slot_s, sizeof(slot_s));
    httpd_query_key_value(buf, "family", fam_s, sizeof(fam_s));
    httpd_query_key_value(buf, "name", r.name, sizeof(r.name));
    httpd_query_key_value(buf, "notes", r.notes, sizeof(r.notes));
    httpd_query_key_value(buf, "num", nums, sizeof(nums));
    hr_url_decode(r.name);
    hr_url_decode(r.notes);
    hr_url_decode(nums);

    int slot = atoi(slot_s);
    r.family = (hr_family_t)atoi(fam_s);
    r.used = true;

    /* Keep the run count across an edit - it is the record of what worked. */
    hr_recipe_t old;
    if (rcp_load(slot, &old)) {
        r.runs = old.runs;
    }

    char *p = nums;
    r.nnum = 0;
    while (*p && r.nnum < HR_RECIPE_MAX_NUM) {
        r.num[r.nnum++] = (int32_t)strtol(p, &p, 10);
        if (*p == ',') {
            p++;
        } else {
            break;
        }
    }

    hr_recipe_err_t e = hr_recipe_validate(&r);
    if (e != HR_RECIPE_OK) {
        char out[224];
        int n = snprintf(out, sizeof(out), "{\"ok\":false,\"reason\":\"%s\"}",
                         hr_recipe_err_str(e));
        httpd_resp_set_status(req, "400 Bad Request");
        ESP_LOGW(TAG, "recipe save refused: %s", hr_recipe_err_str(e));
        return send_json(req, out, n);
    }
    bool ok = rcp_store(slot, &r);
    ESP_LOGI(TAG, "recipe slot %d saved as %s -> %s", slot, r.name,
             ok ? "ok" : "FAILED");
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", ok ? 11 : 12);
}

/* POST /api/recipes/delete   slot */
static esp_err_t h_recipe_delete(httpd_req_t *req)
{
    char buf[64];
    if (read_body(req, buf, sizeof(buf)) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }
    char slot_s[8] = {0};
    httpd_query_key_value(buf, "slot", slot_s, sizeof(slot_s));
    bool ok = rcp_store(atoi(slot_s), NULL);
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", ok ? 11 : 12);
}

/*
 * POST /api/recipes/send   slot, start=<0|1>, confirm=<0|1>
 *
 * With start=1 this begins a batch, so it passes the same gates as a button:
 * control must be switched on, and starting needs an explicit confirmation.
 *
 * Note what is DIFFERENT from a CLICK. A CLICK is screen-relative and can be
 * validated against live telemetry; a recipe frame is accepted from wherever
 * the machine happens to be, so there is no equivalent check to make. The
 * confirmation is the only barrier, which is exactly why it is not optional.
 */
static esp_err_t h_recipe_send(httpd_req_t *req)
{
    if (!ctrl_enabled()) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"control is disabled in settings\"}");
    }
    char buf[128];
    if (read_body(req, buf, sizeof(buf)) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }
    char slot_s[8] = {0}, start_s[8] = {0}, conf_s[8] = {0};
    httpd_query_key_value(buf, "slot", slot_s, sizeof(slot_s));
    httpd_query_key_value(buf, "start", start_s, sizeof(start_s));
    httpd_query_key_value(buf, "confirm", conf_s, sizeof(conf_s));
    bool start = (start_s[0] == '1');
    bool confirmed = (conf_s[0] == '1');

    if (start && !confirmed) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"confirmation required\"}");
    }

    LOCK();
    int live = live_screen();
    UNLOCK();
    if (live < 0) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"no live telemetry from the dryer\"}");
    }
    if (start && !recipe_start_allowed(live)) {
        char body[128];
        int n = snprintf(body, sizeof(body),
                         "{\"ok\":false,\"reason\":\"dryer is not on the Ready "
                         "screen\",\"live_screen\":%d}", live);
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, body, n);
    }

    hr_recipe_t r;
    if (!rcp_load(atoi(slot_s), &r)) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"no recipe\"}");
    }

    char frame[320];
    uint32_t seq = ctrl_next_seq();
    if (seq == 0) {
        return refuse_no_counter(req);
    }
    if (hr_recipe_build(&r, start, seq, frame, sizeof(frame)) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"recipe failed validation\"}");
    }

    /*
     * Sent raw. The payload is one pre-quoted argument; the ordinary field
     * builder would split it on commas and re-quote the pieces, which the
     * dryer would read as a different recipe rather than as an error.
     */
    LOCK();
    bool ok = hr_session_send_raw(s_session, frame);
    UNLOCK();
    /* The full frame and the live screen, so a capture can be matched to
     * what the machine did next. */
    ESP_LOGW(TAG, "recipe %s sent%s (live screen %d): %s -> %s", r.name,
             start ? " WITH START" : "", live, frame, ok ? "ok" : "failed");

    char out[160];
    int n = snprintf(out, sizeof(out),
                     "{\"ok\":%s,\"name\":\"%s\",\"started\":%s}",
                     ok ? "true" : "false", r.name, start ? "true" : "false");
    return send_json(req, out, n);
}


/*
 * POST /api/recipes/apply   family,name,num=csv,start=<0|1>,confirm=<0|1>
 *
 * Sends a recipe built from values supplied in the request rather than from a
 * saved slot. This is what the setup panel uses: the operator adjusts controls,
 * presses Submit, and those exact values go to the dryer without being stored
 * as a saved recipe first.
 *
 * Same gates as /send. start=1 begins a batch and needs confirmation; there is
 * no screen to validate against because a recipe frame is accepted wherever the
 * machine is, so the confirmation is the only barrier.
 */
static esp_err_t h_recipe_apply(httpd_req_t *req)
{
    if (!ctrl_enabled()) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"control is disabled in settings\"}");
    }
    char buf[512];
    if (read_body(req, buf, sizeof(buf)) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"bad body\"}");
    }
    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }
    char fam_s[8] = {0}, nums[256] = {0}, start_s[8] = {0}, conf_s[8] = {0};
    hr_recipe_t r;
    memset(&r, 0, sizeof(r));
    httpd_query_key_value(buf, "family", fam_s, sizeof(fam_s));
    httpd_query_key_value(buf, "name", r.name, sizeof(r.name));
    httpd_query_key_value(buf, "num", nums, sizeof(nums));
    httpd_query_key_value(buf, "start", start_s, sizeof(start_s));
    httpd_query_key_value(buf, "confirm", conf_s, sizeof(conf_s));
    hr_url_decode(r.name);
    hr_url_decode(nums);

    bool start = (start_s[0] == '1');
    bool confirmed = (conf_s[0] == '1');
    if (start && !confirmed) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"confirmation required\"}");
    }

    LOCK();
    int live = live_screen();
    UNLOCK();
    if (live < 0) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"no live telemetry from the dryer\"}");
    }
    if (start && !recipe_start_allowed(live)) {
        char out[128];
        int n = snprintf(out, sizeof(out),
                         "{\"ok\":false,\"reason\":\"dryer is not on the Ready "
                         "screen\",\"live_screen\":%d}", live);
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, out, n);
    }

    r.family = (hr_family_t)atoi(fam_s);
    r.used = true;
    char *p = nums;
    r.nnum = 0;
    while (*p && r.nnum < HR_RECIPE_MAX_NUM) {
        r.num[r.nnum++] = (int32_t)strtol(p, &p, 10);
        if (*p == ',') {
            p++;
        } else {
            break;
        }
    }

    hr_recipe_err_t e = hr_recipe_validate(&r);
    if (e != HR_RECIPE_OK) {
        char out[224];
        int n = snprintf(out, sizeof(out), "{\"ok\":false,\"reason\":\"%s\"}",
                         hr_recipe_err_str(e));
        httpd_resp_set_status(req, "400 Bad Request");
        ESP_LOGW(TAG, "recipe apply refused: %s", hr_recipe_err_str(e));
        return send_json(req, out, n);
    }

    char frame[320];
    uint32_t seq = ctrl_next_seq();
    if (seq == 0) {
        return refuse_no_counter(req);
    }
    if (hr_recipe_build(&r, start, seq, frame, sizeof(frame)) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"build failed\"}");
    }
    LOCK();
    bool ok = hr_session_send_raw(s_session, frame);
    UNLOCK();
    ESP_LOGW(TAG, "recipe applied %s%s (live screen %d): %s -> %s", r.name,
             start ? " WITH START" : "", live, frame, ok ? "ok" : "failed");

    char out[160];
    int n = snprintf(out, sizeof(out),
                     "{\"ok\":%s,\"started\":%s}",
                     ok ? "true" : "false", start ? "true" : "false");
    return send_json(req, out, n);
}

/* -------------------------------------------------------------------- */
/* Control PIN                                                           */
/* -------------------------------------------------------------------- */

/*
 * A PIN on the endpoints that can change the machine. Monitoring stays open.
 *
 * The decision that network security was sufficient was made when this adapter
 * was READ-ONLY. It can now start and end 24-hour cycles, and anything on the
 * LAN can post to /api/control. That is a different risk, so it gets a
 * different answer.
 *
 * This is deliberately not a login system - no accounts, no cookies, no
 * sessions. The threat being addressed is "someone else on the network taps
 * Start", not a determined attacker, and the UI says so rather than implying
 * the device is secured.
 */
#define PIN_NVS_NS   "hrpin"
#define PIN_MAX      9
#define PIN_TRIES    5
#define PIN_LOCK_MS  60000u

static uint8_t  s_pin_fails;
static uint32_t s_pin_locked_until;

static bool pin_is_set(char *out, size_t cap)
{
    nvs_handle_t nh;
    if (nvs_open(PIN_NVS_NS, NVS_READONLY, &nh) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    bool ok = nvs_get_str(nh, "pin", out, &len) == ESP_OK && out[0] != '\0';
    nvs_close(nh);
    return ok;
}

/*
 * Whether a PIN is stored, without reading it: with a NULL buffer nvs_get_str
 * reports only the length, NUL included, so an empty PIN is 1. For callers
 * that need the yes/no and must not hold the PIN itself - /api/state, which
 * anyone may read, used to load it onto the same stack as its reply.
 */
static bool pin_present(void)
{
    nvs_handle_t nh;
    if (nvs_open(PIN_NVS_NS, NVS_READONLY, &nh) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    bool ok = nvs_get_str(nh, "pin", NULL, &len) == ESP_OK && len > 1;
    nvs_close(nh);
    return ok;
}

static uint32_t ms_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/*
 * Compare without an early exit.
 *
 * Timing analysis of a 4-digit PIN over HTTP is not a realistic attack, but a
 * length-independent compare costs one line and removes the question.
 */
static bool pin_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < la && i < lb; i++) {
        diff |= (unsigned)(a[i] ^ b[i]);
    }
    return diff == 0;
}

typedef enum {
    PIN_OK = 0,
    PIN_WRONG,
    PIN_LOCKED,
} pin_result_t;

static pin_result_t pin_check(const char *supplied)
{
    char want[PIN_MAX + 1] = {0};
    if (!pin_is_set(want, sizeof(want))) {
        return PIN_OK;              /* no PIN configured: nothing to enforce */
    }
    /*
     * Signed difference so the comparison survives the 32-bit millisecond
     * wrap at 49.7 days of uptime; `now < until` there either ends the lock
     * early or extends it for another 49 days.
     */
    if (s_pin_locked_until != 0) {
        if ((int32_t)(ms_now() - s_pin_locked_until) < 0) {
            return PIN_LOCKED;
        }
        s_pin_locked_until = 0; /* lock expired: start counting afresh */
        s_pin_fails = 0;
    }
    if (supplied == NULL || supplied[0] == '\0') {
        /*
         * No PIN offered at all. The web UI's first request in a session is
         * exactly this - it asks for the PIN only after being told one is
         * needed - so it must not spend one of the five attempts.
         */
        return PIN_WRONG;
    }
    if (pin_equal(supplied, want)) {
        s_pin_fails = 0;
        return PIN_OK;
    }
    /*
     * Lock out after a handful of wrong answers. A 4-digit PIN is 10,000
     * guesses; unthrottled that is seconds over a LAN, and at five tries a
     * minute it is weeks.
     */
    if (++s_pin_fails >= PIN_TRIES) {
        s_pin_fails = 0;
        s_pin_locked_until = ms_now() + PIN_LOCK_MS;
        ESP_LOGW(TAG, "too many wrong PINs; control locked for %us",
                 (unsigned)(PIN_LOCK_MS / 1000));
        return PIN_LOCKED;
    }
    return PIN_WRONG;
}

/*
 * Guard for a control endpoint. Returns true when the request may proceed;
 * otherwise it has already sent the refusal.
 *
 * The PIN is read from the form body (pin=...) or, for requests whose body is
 * not a form - the OTA upload is a raw binary - from an X-HR-Pin header.
 */
static bool pin_guard(httpd_req_t *req, const char *body)
{
    char supplied[PIN_MAX + 1] = {0};
    if (body != NULL) {
        httpd_query_key_value(body, "pin", supplied, sizeof(supplied));
    }
    if (supplied[0] == '\0') {
        httpd_req_get_hdr_value_str(req, "X-HR-Pin", supplied,
                                    sizeof(supplied));
    }
    switch (pin_check(supplied)) {
    case PIN_OK:
        return true;
    case PIN_LOCKED:
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"reason\":\"too many wrong PINs - wait a minute\","
            "\"pin\":true}");
        return false;
    default:
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"reason\":\"PIN required\",\"pin\":true}");
        return false;
    }
}

/*
 * pin_guard() for handlers that carry no form fields of their own (forget,
 * clear, format, reattach). Reads whatever small body the client sent so the
 * PIN can travel in it, then applies the same check.
 */
static bool pin_guard_small(httpd_req_t *req)
{
    char buf[64] = {0};
    int total = req->content_len;
    if (total > 0 && total < (int)sizeof(buf)) {
        int got = 0;
        while (got < total) {
            int r = httpd_req_recv(req, buf + got, total - got);
            if (r <= 0) {
                break;
            }
            got += r;
        }
        buf[got > 0 ? got : 0] = '\0';
    }
    return pin_guard(req, buf);
}

/* GET /api/pin -> whether one is set. Never returns the PIN itself. */
static esp_err_t h_pin_state(httpd_req_t *req)
{
    char cur[PIN_MAX + 1] = {0};
    bool set = pin_is_set(cur, sizeof(cur));
    char out[64];
    int n = snprintf(out, sizeof(out), "{\"set\":%s}", set ? "true" : "false");
    return send_json(req, out, n);
}

/*
 * POST /api/pin   new=<digits>&pin=<current>
 *
 * Changing or clearing an existing PIN requires the current one, or anyone on
 * the network could simply replace it.
 */
static esp_err_t h_pin_set(httpd_req_t *req)
{
    char buf[128];
    if (read_body(req, buf, sizeof(buf)) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    if (!pin_guard(req, buf)) {
        return ESP_OK;
    }

    char fresh[PIN_MAX + 1] = {0};
    httpd_query_key_value(buf, "new", fresh, sizeof(fresh));

    size_t len = strlen(fresh);
    if (len != 0 && (len < 4 || len > 8)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"reason\":\"PIN must be 4 to 8 digits\"}");
    }
    for (size_t i = 0; i < len; i++) {
        if (fresh[i] < '0' || fresh[i] > '9') {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req,
                "{\"ok\":false,\"reason\":\"digits only\"}");
        }
    }

    nvs_handle_t nh;
    if (nvs_open(PIN_NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        return httpd_resp_send_500(req);
    }
    bool ok = nvs_set_str(nh, "pin", fresh) == ESP_OK &&
              nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    ESP_LOGW(TAG, "control PIN %s", len ? "changed" : "removed");
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                     ok ? 11 : 12);
}

/*
 * POST /api/pin/verify   pin=<pin>  (or the X-HR-Pin header)
 *
 * Confirms a PIN with NO side effect, so the web app can gate a control screen
 * up front - ask for the PIN before opening it or sending anything - rather
 * than have the user act and be refused. Same check and the same lockout as
 * every other guarded endpoint; when no PIN is set it simply passes.
 */
static esp_err_t h_pin_verify(httpd_req_t *req)
{
    if (!pin_guard_small(req)) {
        return ESP_OK;   /* pin_guard already sent {ok:false,pin:true} */
    }
    return send_json(req, "{\"ok\":true}", 11);
}

/* -------------------------------------------------------------------- */
/* Logbook and clock                                                     */
/* -------------------------------------------------------------------- */

/*
 * POST /api/time   epoch=<unix seconds>
 *
 * The adapter has no RTC and deliberately does not use SNTP, so the browser
 * tells it the time - the same arrangement SETDATE already uses for the dryer.
 * Posted on every page load, so the clock is right as soon as anyone looks at
 * the app, and drift between visits does not matter at logbook resolution.
 */
static esp_err_t h_time(httpd_req_t *req)
{
    char buf[64];
    if (read_body(req, buf, sizeof(buf)) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    char e[16] = {0};
    httpd_query_key_value(buf, "epoch", e, sizeof(e));
    uint32_t epoch = (uint32_t)strtoul(e, NULL, 10);
    /* Report whether THIS value was accepted, not merely whether a clock has
     * ever been set - otherwise posting nonsense returns ok while being
     * silently discarded. */
    bool accepted = hr_time_set(epoch);

    char out[128];
    int n = snprintf(out, sizeof(out),
                     "{\"ok\":%s,\"now\":%lu,\"clock\":%s}",
                     accepted ? "true" : "false",
                     (unsigned long)hr_time_now(),
                     hr_time_known() ? "true" : "false");
    if (!accepted) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    return send_json(req, out, n);
}

/* GET /api/batches.csv - both segments, streamed, oldest first. */
static esp_err_t h_batches_csv(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=batches.csv");
    void *h = hr_batchstore_open();
    if (h == NULL) {
        return httpd_resp_sendstr(req, "");
    }
    char *buf = s_scratch.bytes;
    int n;
    while ((n = hr_batchstore_read(h, buf, sizeof(s_scratch.bytes))) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            hr_batchstore_close(h);
            return ESP_FAIL;
        }
    }
    hr_batchstore_close(h);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/*
 * GET /api/batches - the logbook as JSON, newest first.
 *
 * Streamed a record at a time. With no PSRAM there is no holding a thousand
 * batches in memory to sort them, so the file is read once into a bounded
 * ring of the most recent entries and only those are rendered.
 * BATCH_SHOW is defined with s_scratch near the top of the file.
 */
static esp_err_t h_batches(httpd_req_t *req)
{
    hr_batch_t *ring = s_scratch.batches;
    size_t have = 0, next = 0;

    void *h = hr_batchstore_open();
    if (h != NULL) {
        char chunk[512];
        char line[HR_BATCH_LINE_MAX];
        size_t at = 0;
        int n;
        while ((n = hr_batchstore_read(h, chunk, sizeof(chunk))) > 0) {
            for (int i = 0; i < n; i++) {
                char c = chunk[i];
                if (c == '\n' || at + 1 >= sizeof(line)) {
                    line[at] = '\0';
                    hr_batch_t b;
                    if (at > 0 && hr_batch_decode(line, &b)) {
                        ring[next] = b;
                        next = (next + 1) % BATCH_SHOW;
                        if (have < BATCH_SHOW) {
                            have++;
                        }
                    }
                    at = 0;
                } else if (c != '\r') {
                    line[at++] = c;
                }
            }
        }
        hr_batchstore_close(h);
    }

    httpd_resp_set_type(req, "application/json");
    char head[128];
    int hn = snprintf(head, sizeof(head),
                      "{\"clock\":%s,\"now\":%lu,\"bytes\":%u,\"batches\":[",
                      hr_time_known() ? "true" : "false",
                      (unsigned long)hr_time_now(),
                      (unsigned)hr_batchstore_bytes());
    httpd_resp_send_chunk(req, head, hn);

    /* Newest first: walk the ring backwards from the most recent write. */
    for (size_t k = 0; k < have; k++) {
        size_t idx = (next + BATCH_SHOW - 1 - k) % BATCH_SHOW;
        const hr_batch_t *b = &ring[idx];
        char nm[64];
        hr_json_escape(b->name, nm, sizeof(nm));
        char row[448];      /* the three phase times pushed this past 352 */
        int rn = snprintf(row, sizeof(row),
                          "%s{\"start\":%lu,\"duration_s\":%lu,\"name\":\"%s\","
                          "\"family\":%u,\"extra_dry_s\":%ld,\"min_f\":%d,"
                          "\"max_f\":%d,\"vacuum_um\":%ld,\"pulldown_s\":%lu,"
                          "\"freeze_s\":%lu,\"dry_s\":%lu,\"final_s\":%lu,"
                          "\"outcome\":\"%s\"}",
                          k ? "," : "",
                          (unsigned long)b->start_epoch,
                          (unsigned long)b->duration_s, nm,
                          (unsigned)b->family, (long)b->extra_dry_s,
                          (int)b->min_temp_f, (int)b->max_temp_f,
                          (long)b->best_vacuum_um,
                          (unsigned long)b->pulldown_s,
                          (unsigned long)b->freeze_s,
                          (unsigned long)b->dry_s,
                          (unsigned long)b->final_s,
                          hr_outcome_str(b->outcome));
        if (rn > 0 && httpd_resp_send_chunk(req, row, rn) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    /*
     * How long the next run is likely to take, from the runs above. Sent with
     * the logbook because it is derived from it - a caller that has the
     * history should not have to ask a second endpoint for the conclusion.
     */
    hr_batch_estimate_t est;
    hr_batch_estimate(ring, have, &est);
    char tail[192];
    int tn = snprintf(tail, sizeof(tail),
                      "%s{\"freeze_s\":%lu,\"dry_s\":%lu,\"final_s\":%lu,\"total_s\":%lu,\"samples\":%u}",
                      "],\"estimate\":",
                      (unsigned long)est.freeze_s, (unsigned long)est.dry_s,
                      (unsigned long)est.final_s, (unsigned long)est.total_s,
                      (unsigned)est.samples);
    if (tn > 0) {
        httpd_resp_send_chunk(req, tail, tn);
    }
    httpd_resp_send_chunk(req, "}", 1);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/*
 * POST /api/storage/format - last-resort recovery.
 *
 * For the state where SPIFFS reports free space and refuses every write. Wipes
 * the capture log, the trend and the logbook, then re-mounts and re-initialises
 * both stores so recording resumes without a reboot.
 */
static esp_err_t h_storage_format(httpd_req_t *req)
{
    if (!pin_guard_small(req)) {
        return ESP_OK;
    }
    bool ok = hr_capture_format();
    if (ok) {
        hr_batchstore_init();
    }
    ESP_LOGW(TAG, "storage format requested -> %s", ok ? "ok" : "FAILED");
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                     ok ? 11 : 12);
}

/* POST /api/batches/clear */
static esp_err_t h_batches_clear(httpd_req_t *req)
{
    if (!pin_guard_small(req)) {
        return ESP_OK;
    }
    bool ok = hr_batchstore_clear();
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", ok ? 11 : 12);
}

/* -------------------------------------------------------------------- */
/* Registration                                                          */
/* -------------------------------------------------------------------- */
/*
 * Register a route, and SAY SO when it fails.
 *
 * This has now silently overflowed max_uri_handlers twice. Both times the only
 * evidence was one buried httpd warning, and the visible symptom was a handful
 * of routes 404ing for no apparent reason - the second time taking the captive
 * portal endpoints with them, which is how a phone finds the setup hotspot.
 *
 * A route that fails to register is a broken build, not a warning, so it is
 * logged at ERROR with its own name and the running count.
 */
static int s_routes;

/*
 * Cross-site request forgery.
 *
 * Every POST here is a "simple request" in browser terms - form body, no
 * custom header - so a page in another tab can fetch("http://<adapter>/api/…",
 * {method:"POST", body}) without a CORS preflight, and the browser sends it.
 * With no PIN configured that is the whole control surface, driven from any
 * website the owner's phone happens to open on the same LAN.
 *
 * Browsers attach an Origin header to every cross-origin POST (and modern ones
 * to same-origin POSTs as well). If it is present it has to name this host; if
 * the client is a script with no Origin at all - curl, Home Assistant - it is
 * not a browser and CSRF does not apply. Only the host part is compared, so
 * reaching the adapter by IP or by a DHCP hostname both work, and the port is
 * whatever the browser put in both headers.
 */
static bool same_origin(httpd_req_t *req)
{
    char origin[128];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) !=
        ESP_OK) {
        return true; /* no Origin header: not a browser cross-site POST */
    }
    if (strcmp(origin, "null") == 0) {
        return false; /* sandboxed / file:// page */
    }
    char host[96];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    const char *p = strstr(origin, "://");
    if (p == NULL) {
        return false;
    }
    p += 3;
    return strcmp(p, host) == 0;
}

typedef esp_err_t (*route_fn_t)(httpd_req_t *);
static route_fn_t s_post_fns[40];
static int s_post_count;

static esp_err_t h_post_guarded(httpd_req_t *req)
{
    route_fn_t fn = *(route_fn_t *)req->user_ctx;
    if (!same_origin(req)) {
        ESP_LOGW(TAG, "refused cross-origin POST to %s", req->uri);
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"reason\":\"cross-origin request refused\"}");
    }
    return fn(req);
}

static void reg(const char *uri, httpd_method_t method,
                esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = {.uri = uri, .method = method, .handler = fn};
    if (method == HTTP_POST &&
        s_post_count < (int)(sizeof(s_post_fns) / sizeof(s_post_fns[0]))) {
        s_post_fns[s_post_count] = fn;
        u.handler = h_post_guarded;
        u.user_ctx = &s_post_fns[s_post_count];
        s_post_count++;
    }
    esp_err_t err = httpd_register_uri_handler(s_httpd, &u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ROUTE LOST: %s (#%d) - %s. Raise max_uri_handlers.",
                 uri, s_routes + 1, esp_err_to_name(err));
    }
    s_routes++;
}

void hr_http_use_lock(void *mutex)
{
    s_lock = (SemaphoreHandle_t)mutex;
}

void hr_http_start(hr_session_t *session, hr_history_t *history)
{
    s_session = session;
    s_history = history;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex(); /* fallback if app didn't supply one */
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /*
     * MUST be >= the number of reg() calls below. There are 25, and this said
     * 20 - so five handlers failed to register at boot with nothing but a
     * warning buried in the log, and whichever routes fell off the end simply
     * 404ed. Adding the two control routes is what pushed it over, but the
     * margin had already gone.
     */
    /*
     * MUST exceed the number of reg() calls below - there are 37, and this has
     * been overrun twice already. 64 is not extravagance: each slot is a few
     * bytes of a table, while an overrun silently deletes whichever routes
     * happen to be registered last.
     */
    cfg.max_uri_handlers = 64;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    /*
     * Client slots. httpd needs 3 more sockets than this for its own use
     * (httpd_main.c enforces max_open_sockets + 3 <= LWIP_MAX_SOCKETS), and
     * accept() needs a FREE descriptor to hand the next connection - if the
     * table is exactly full it fails with ENFILE before lru_purge can
     * reclaim a slot. So the invariant below deliberately reserves spare
     * descriptors on top of httpd's three, for MQTT, DNS and that accept.
     */
    cfg.max_open_sockets = 7;
    _Static_assert(
        CONFIG_LWIP_MAX_SOCKETS >= 7 + 3 + 3,
        "LWIP_MAX_SOCKETS leaves no spare descriptors: accept() will fail "
        "with ENFILE once max_open_sockets clients connect. Raise "
        "CONFIG_LWIP_MAX_SOCKETS or lower max_open_sockets.");
    /*
     * The default 4096 is too tight for h_ota: it puts a 1KB receive buffer on
     * this stack and then calls down through esp_ota_write() into the SPI flash
     * driver. An overflow there looks exactly like a failed upload from the
     * browser's side, with nothing useful in the log.
     */
    cfg.stack_size = 8192;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }

    reg("/", HTTP_GET, h_root);
    reg("/events", HTTP_GET, h_events);
    reg("/api/state", HTTP_GET, h_state);
    reg("/api/control", HTTP_POST, h_control);
    reg("/api/recipes", HTTP_GET, h_recipes);
    reg("/api/recipes/save", HTTP_POST, h_recipe_save);
    reg("/api/recipes/delete", HTTP_POST, h_recipe_delete);
    reg("/api/recipes/send", HTTP_POST, h_recipe_send);
    reg("/api/recipes/apply", HTTP_POST, h_recipe_apply);
    reg("/api/time", HTTP_POST, h_time);
    reg("/api/batches", HTTP_GET, h_batches);
    reg("/api/batches.csv", HTTP_GET, h_batches_csv);
    reg("/api/batches/clear", HTTP_POST, h_batches_clear);
    reg("/api/storage/format", HTTP_POST, h_storage_format);
    reg("/api/pin", HTTP_GET, h_pin_state);
    reg("/api/pin", HTTP_POST, h_pin_set);
    reg("/api/pin/verify", HTTP_POST, h_pin_verify);
    reg("/api/control/enable", HTTP_POST, h_control_enable);
    reg("/api/history", HTTP_GET, h_history);
    reg("/api/verbs", HTTP_GET, h_verbs);
    reg("/api/capture", HTTP_GET, h_capture);
    reg("/api/capture/info", HTTP_GET, h_capture_info);
    reg("/api/capture/clear", HTTP_POST, h_capture_clear);
    reg("/api/usb/reattach", HTTP_POST, h_usb_reattach);
    reg("/api/trend", HTTP_GET, h_trend);
    reg("/api/enc", HTTP_GET, h_enc);
    reg("/api/scan", HTTP_GET, h_scan);
    reg("/api/wifi", HTTP_POST, h_wifi_post);
    reg("/api/forget", HTTP_POST, h_forget);
    reg("/api/cmd", HTTP_POST, h_cmd);
#if HR_ENABLE_PROBE
    reg("/api/probe", HTTP_POST, h_probe);
    ESP_LOGW(TAG, "PROBE ENDPOINT ENABLED - bench build, do not ship");
#endif
    reg("/api/ota", HTTP_POST, h_ota);
    reg("/api/mqtt", HTTP_GET, h_mqtt_get);
    reg("/api/mqtt", HTTP_POST, h_mqtt_post);
    reg("/api/log", HTTP_GET, h_log);
    reg("/api/log", HTTP_POST, h_log);
    reg("/api/wififlags", HTTP_POST, h_wififlags);
    reg("/api/compat", HTTP_GET, h_compat_get);
    reg("/api/compat", HTTP_POST, h_compat_post);
    reg("/api/dryer/reboot", HTTP_POST, h_dryer_reboot);
    reg("/img/*", HTTP_GET, h_img);
    /* Captive-portal probes (Android/Apple/Windows). */
    reg("/generate_204", HTTP_GET, h_redirect);
    reg("/hotspot-detect.html", HTTP_GET, h_redirect);
    reg("/connecttest.txt", HTTP_GET, h_redirect);

    ESP_LOGI(TAG, "web UI started");
}

void hr_http_notify(uint32_t seq)
{
    /* The SSE handler polls history, so an explicit signal isn't required.
     * Kept for future use (e.g. task notification to reduce latency). */
    (void)seq;
}
