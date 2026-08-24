#include "hr_capture.h"
#include "hr_control.h"
#include "hr_recipe.h"
#include "hr_batchstore.h"
#include "hr_http.h"
#include "hr_log.h"
#include "hr_mqtt.h"
#include "hr_telemetry.h"
#include "hr_trend.h"
#include "hr_usb.h"
#include "hr_wifi.h"

#include "esp_app_desc.h"
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
/*
 * Running/idle is decided by whether the dryer's elapsed counter is actually
 * ADVANCING - it retains the previous batch's value when idle, so a non-zero
 * elapsed alone does not mean a batch is running.
 */
static hr_phase_tracker_t s_tracker;
static bool s_tracker_ready;
/* 30s graph series, owned by main.c and guarded by the shared s_lock. */
static hr_trend_t *s_trend;

/* Learns how often drying had to be extended - see hr_recipe.h. */
static hr_dry_tracker_t s_dry = {-1, 0};

int32_t hr_http_extra_dry_s(void)
{
    return hr_dry_extra_s(&s_dry);
}

void hr_http_set_telemetry(const hr_telemetry_t *t)
{
    if (t != NULL && t->valid) {
        s_tel = *t;
        s_tel_valid = true;
        /* Watch the SCREEN rather than our own commands: most
         * More Dry Time presses happen on the panel by hand. */
        hr_dry_observe(&s_dry, (int)t->type);
    }
}

void hr_http_set_trend(hr_trend_t *tr)
{
    s_trend = tr;
}

void hr_http_set_tracker(const hr_phase_tracker_t *tr)
{
    if (tr != NULL) {
        s_tracker = *tr;
        s_tracker_ready = true;
    }
}

/*
 * The frame history is written from the USB RX task and read from httpd
 * tasks, so guard it. The app supplies this mutex (it is the writer) via
 * hr_http_use_lock() so both sides serialise on one lock.
 */
static SemaphoreHandle_t s_lock;
#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* -------------------------------------------------------------------- */
/* Small helpers                                                         */
/* -------------------------------------------------------------------- */
static esp_err_t send_json(httpd_req_t *req, const char *json, size_t len)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, len);
}

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
static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_EXT:      return "external";
    case ESP_RST_SW:       return "sw";        /* esp_restart(), e.g. after OTA */
    case ESP_RST_PANIC:    return "panic";     /* crashed - look for a coredump */
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";  /* USB rail sagged */
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
    }
}

/* Defined with the rest of the control code further down; /api/state needs
 * it here to report whether control is switched on. */
static bool ctrl_enabled(void);
static bool pin_is_set(char *out, size_t cap);
/* Defined with the PIN code below; the control endpoints above need it.
 * Returns true when the request may proceed, and has already sent the
 * refusal when it returns false. */
static bool pin_guard(httpd_req_t *req, const char *body);

static esp_err_t h_state(httpd_req_t *req)
{
    char ip[16], ssid[33], serial[64], uid[128];
    hr_wifi_ip(ip, sizeof(ip));
    hr_wifi_current_ssid(ssid, sizeof(ssid));

    char laststat[HR_MAX_FRAME * 2];
    char pinbuf[16];
    LOCK();
    hr_json_escape(s_session->info.last_stat, laststat, sizeof(laststat));
    hr_json_escape(s_session->info.serial, serial, sizeof(serial));
    hr_json_escape(s_session->info.uid, uid, sizeof(uid));
    unsigned long fin = s_session->frames_in, fout = s_session->frames_out;
    unsigned long unk = s_session->unknown_verbs;
    unsigned long bad = s_session->stream.frames_bad;
    const char *link = s_session->link == HR_LINK_UP ? "up" : "down";
    uint32_t latest = hr_history_latest_seq(s_history);
    UNLOCK();

    /* Cycle phase + live readings (empty/idle values when nothing seen yet). */
    hr_phase_t ph = s_tel_valid
                        ? hr_phase_of_tracked(&s_tel,
                                              s_tracker_ready ? &s_tracker : NULL)
                        : HR_PHASE_UNKNOWN;
    char mode_esc[32];
    hr_json_escape(s_tel_valid ? s_tel.mode : "", mode_esc, sizeof(mode_esc));

    /*
     * The actions the machine is offering RIGHT NOW. The UI renders from this
     * rather than from a hardcoded list, so a screen we have never captured
     * shows no buttons at all instead of guessed ones.
     */
    char acts[384];
    size_t ai = 0;
    acts[ai++] = '[';
    const hr_action_t *av[8];
    size_t an = hr_control_for_screen(s_tel_valid ? (int)s_tel.type : -1, av, 8);
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

    char body[2048];
    int n = snprintf(body, sizeof(body),
                     "{\"link\":\"%s\",\"serial\":\"%s\",\"uid\":\"%s\","
                     "\"frames_in\":%lu,\"frames_out\":%lu,"
                     "\"unknown_verbs\":%lu,\"frames_bad\":%lu,"
                     "\"latest_seq\":%" PRIu32 ",\"wifi\":\"%s\",\"ip\":\"%s\","
                     "\"ssid\":\"%s\","
                     "\"phase\":%d,\"phase_label\":\"%s\",\"have_tel\":%s,"
                     "\"temp_f\":%ld,\"pressure\":%ld,\"elapsed_s\":%ld,"
                     "\"prep_s\":%ld,\"mode\":\"%s\",\"stat_type\":%d,"
                     "\"freeze_pct\":%ld,\"freeze_eta_s\":%ld,"
                     "\"phase_pct\":%ld,\"phase_s\":%ld,"
                     "\"vacuum_um\":%ld,\"vacuum_ok\":%s,"
                     /* USB-level diagnostics: reachable over WiFi while the
                      * adapter is plugged into the dryer, which the serial
                      * console is not. See hr_usb.h for how to read them. */
                     "\"usb_mounted\":%s,\"usb_suspended\":%s,"
                     "\"usb_mounts\":%u,\"usb_rx_bytes\":%lu,"
                     "\"uptime_s\":%lu,\"reset_reason\":\"%s\","
                     "\"control\":%s,\"actions\":%s,\"pin\":%s,"
                     /* Raw frame: the config screens carry the live
                      * recipe in fields we do not decode here, and the
                      * editor seeds itself from what is on the panel
                      * rather than from a remembered default. */
                     "\"last_stat\":\"%s\","
                     "\"version\":\"" FREEHARVEST_VERSION "\"}",
                     link, serial, uid, fin, fout, unk, bad, latest,
                     wifi_status_str(), ip, ssid,
                     (int)ph, hr_phase_label(ph), s_tel_valid ? "true" : "false",
                     s_tel_valid ? s_tel.temperature_f : 0,
                     s_tel_valid ? s_tel.pressure_raw : 0,
                     s_tel_valid ? s_tel.batch_elapsed_s : 0,
                     s_tel_valid ? s_tel.prep_remaining_s : 0,
                     mode_esc, s_tel_valid ? s_tel.type : 0,
                     s_tel_valid ? s_tel.freeze_pct : 0,
                     (s_tel_valid && s_tracker_ready)
                         ? hr_freeze_eta_s(&s_tracker, &s_tel)
                         : -1,
                     s_tel_valid ? s_tel.phase_pct : 0,
                     s_tel_valid ? s_tel.phase_elapsed_s : 0,
                     s_tel_valid ? s_tel.pressure_microns : 0,
                     (s_tel_valid && s_tel.pressure_valid) ? "true" : "false",
                     hr_usb_mounted() ? "true" : "false",
                     hr_usb_suspended() ? "true" : "false",
                     hr_usb_mount_events(), hr_usb_rx_bytes(),
                     (unsigned long)(esp_timer_get_time() / 1000000),
                     reset_reason_str(), ctrl_enabled() ? "true" : "false",
                     acts, pin_is_set(pinbuf, sizeof(pinbuf))
                         ? "true" : "false", laststat);
    return send_json(req, body, n);
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
    static hr_hist_entry_t out[HR_HIST_CAP]; /* static: too big for stack */
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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "[");

    LOCK();
    int nv = s_history->nverbs;
    for (int i = 0; i < nv; i++) {
        const hr_hist_verb_t *v = &s_history->verbs[i];
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
        httpd_resp_send_chunk(req, obj, n);
    }
    UNLOCK();

    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* -------------------------------------------------------------------- */
/* GET /api/capture  -> plain-text log download                          */
/* -------------------------------------------------------------------- */
static esp_err_t h_capture(httpd_req_t *req)
{
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
            static char buf[1024];
            int n;
            int first = hr_capture_read(h, buf, sizeof(buf));
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
                goto ram_fallback;
            }
            if (httpd_resp_send_chunk(req, buf, first) != ESP_OK) {
                hr_capture_close(h);
                return ESP_FAIL;
            }
            while ((n = hr_capture_read(h, buf, sizeof(buf))) > 0) {
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
    static hr_hist_entry_t out[HR_HIST_CAP];
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
    char body[160];
    int n = snprintf(body, sizeof(body),
                     "{\"ready\":%s,\"bytes\":%u,\"capacity\":%u}",
                     hr_capture_ready() ? "true" : "false",
                     (unsigned)hr_capture_size(),
                     (unsigned)hr_capture_capacity());
    return send_json(req, body, n);
}

/* POST /api/capture/clear -> erase the persistent log */
static esp_err_t h_capture_clear(httpd_req_t *req)
{
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

static esp_err_t h_wifi_post(httpd_req_t *req)
{
    char buf[160];
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

    char ssid[64] = {0}, pw[96] = {0};
    httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(buf, "password", pw, sizeof(pw));
    /* httpd_query_key_value does NOT percent-decode; do it ourselves so
     * passwords with %-escaped characters (!, @, #, &, =, spaces) work. */
    hr_url_decode(ssid);
    hr_url_decode(pw);

    if (!hr_wifi_set_credentials(ssid, pw)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    return send_json(req, "{\"ok\":true}", 11);
}

static esp_err_t h_forget(httpd_req_t *req)
{
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

    if (hr_cmd_classify(verb) != HR_CMD_SAFE) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"not allowed\"}");
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
 * Reboot is deferred slightly so the HTTP 200 reaches the browser first.
 */
static void ota_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    /* Unmount before restarting. An OTA reboot that leaves SPIFFS mounted has
     * been corrupting the capture log - see hr_capture_shutdown(). */
    hr_capture_shutdown();
    ESP_LOGW(TAG, "rebooting into new firmware");
    esp_restart();
}

static esp_err_t h_ota(httpd_req_t *req)
{
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
    xTaskCreate(ota_reboot_task, "ota_reboot", 3072, NULL, 5, NULL);
    return ESP_OK;
}

/* GET /api/log -> recent device log lines (debug view). */
static esp_err_t h_log(httpd_req_t *req)
{
    static char json[8192]; /* static: too large for the httpd task stack */
    size_t n = hr_log_json(json, sizeof(json));
    return send_json(req, json, n);
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
    char buf[320];
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

    char host[80] = {0}, ports[8] = {0}, user[64] = {0}, pass[96] = {0};
    httpd_query_key_value(buf, "host", host, sizeof(host));
    httpd_query_key_value(buf, "port", ports, sizeof(ports));
    httpd_query_key_value(buf, "user", user, sizeof(user));
    httpd_query_key_value(buf, "password", pass, sizeof(pass));
    hr_url_decode(host);
    hr_url_decode(user);
    hr_url_decode(pass);
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
static uint32_t ctrl_next_seq(void)
{
    nvs_handle_t nh;
    uint32_t seq = HR_SEQ_START;
    if (nvs_open(CTRL_NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
        if (nvs_get_u32(nh, "seq", &seq) != ESP_OK) {
            seq = HR_SEQ_START;
        }
        seq++;
        nvs_set_u32(nh, "seq", seq);
        nvs_commit(nh);
        nvs_close(nh);
    }
    return seq;
}

/* The screen currently on the panel, or -1 when we genuinely do not know. */
static int live_screen(void)
{
    return s_tel_valid ? (int)s_tel.type : -1;
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
#define RCP_SLOTS  8

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
static esp_err_t h_recipes(httpd_req_t *req)
{
    static char body[2048];
    size_t at = 0;
    at += (size_t)snprintf(body + at, sizeof(body) - at,
                           "{\"extra_dry_s\":%ld,\"slots\":[",
                           (long)hr_dry_extra_s(&s_dry));
    bool first = true;
    for (int i = 0; i < RCP_SLOTS; i++) {
        hr_recipe_t r;
        if (!rcp_load(i, &r)) {
            continue;
        }
        char nm[64], nt[400];
        hr_json_escape(r.name, nm, sizeof(nm));
        hr_json_escape(r.notes, nt, sizeof(nt));
        at += (size_t)snprintf(body + at, sizeof(body) - at,
                               "%s{\"slot\":%d,\"family\":%d,\"name\":\"%s\","
                               "\"notes\":\"%s\",\"runs\":%lu,\"nnum\":%u,"
                               "\"suggest_dry_s\":%ld,\"num\":[",
                               first ? "" : ",", i, (int)r.family, nm, nt,
                               (unsigned long)r.runs, (unsigned)r.nnum,
                               (long)hr_recipe_suggested_dry_s(&r, &s_dry));
        for (uint8_t k = 0; k < r.nnum && at < sizeof(body) - 16; k++) {
            at += (size_t)snprintf(body + at, sizeof(body) - at, "%s%ld",
                                   k ? "," : "", (long)r.num[k]);
        }
        at += (size_t)snprintf(body + at, sizeof(body) - at, "]}");
        first = false;
        if (at > sizeof(body) - 320) {
            break;
        }
    }
    at += (size_t)snprintf(body + at, sizeof(body) - at, "]}");
    return send_json(req, body, (int)at);
}

/* POST /api/recipes/save   slot,family,name,notes,num=csv */
static esp_err_t h_recipe_save(httpd_req_t *req)
{
    char buf[1024];
    if (read_body(req, buf, sizeof(buf)) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"bad body\"}");
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

    hr_recipe_t r;
    if (!rcp_load(atoi(slot_s), &r)) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"no recipe\"}");
    }

    char frame[320];
    uint32_t seq = ctrl_next_seq();
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
    ESP_LOGW(TAG, "recipe %s sent%s: %s", r.name, start ? " WITH START" : "",
             ok ? "ok" : "failed");

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
    if (hr_recipe_build(&r, start, ctrl_next_seq(), frame, sizeof(frame)) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"build failed\"}");
    }
    LOCK();
    bool ok = hr_session_send_raw(s_session, frame);
    UNLOCK();
    ESP_LOGW(TAG, "recipe applied %s%s: %s", r.name,
             start ? " WITH START" : "", ok ? "ok" : "failed");

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
    if (s_pin_locked_until != 0 && ms_now() < s_pin_locked_until) {
        return PIN_LOCKED;
    }
    if (supplied != NULL && supplied[0] && pin_equal(supplied, want)) {
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
 */
static bool pin_guard(httpd_req_t *req, const char *body)
{
    char supplied[PIN_MAX + 1] = {0};
    if (body != NULL) {
        httpd_query_key_value(body, "pin", supplied, sizeof(supplied));
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
    static char buf[512];
    int n;
    while ((n = hr_batchstore_read(h, buf, sizeof(buf))) > 0) {
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
 */
#define BATCH_SHOW 40

static esp_err_t h_batches(httpd_req_t *req)
{
    static hr_batch_t ring[BATCH_SHOW];
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
        char row[352];
        int rn = snprintf(row, sizeof(row),
                          "%s{\"start\":%lu,\"duration_s\":%lu,\"name\":\"%s\","
                          "\"family\":%u,\"extra_dry_s\":%ld,\"min_f\":%d,"
                          "\"max_f\":%d,\"vacuum_um\":%ld,\"pulldown_s\":%lu,"
                          "\"outcome\":\"%s\"}",
                          k ? "," : "",
                          (unsigned long)b->start_epoch,
                          (unsigned long)b->duration_s, nm,
                          (unsigned)b->family, (long)b->extra_dry_s,
                          (int)b->min_temp_f, (int)b->max_temp_f,
                          (long)b->best_vacuum_um,
                          (unsigned long)b->pulldown_s,
                          hr_outcome_str(b->outcome));
        if (rn > 0 && httpd_resp_send_chunk(req, row, rn) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    httpd_resp_send_chunk(req, "]}", 2);
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
    bool ok = hr_batchstore_clear();
    return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", ok ? 11 : 12);
}

/* -------------------------------------------------------------------- */
/* Registration                                                          */
/* -------------------------------------------------------------------- */
static void reg(const char *uri, httpd_method_t method,
                esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = {.uri = uri, .method = method, .handler = fn};
    httpd_register_uri_handler(s_httpd, &u);
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
    cfg.max_uri_handlers = 32;
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
    reg("/api/control/enable", HTTP_POST, h_control_enable);
    reg("/api/history", HTTP_GET, h_history);
    reg("/api/verbs", HTTP_GET, h_verbs);
    reg("/api/capture", HTTP_GET, h_capture);
    reg("/api/capture/info", HTTP_GET, h_capture_info);
    reg("/api/capture/clear", HTTP_POST, h_capture_clear);
    reg("/api/usb/reattach", HTTP_POST, h_usb_reattach);
    reg("/api/trend", HTTP_GET, h_trend);
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
