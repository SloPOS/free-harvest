#include "hr_capture.h"
#include "hr_http.h"
#include "hr_log.h"
#include "hr_mqtt.h"
#include "hr_telemetry.h"
#include "hr_trend.h"
#include "hr_usb.h"
#include "hr_wifi.h"

#include "esp_app_desc.h"
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

void hr_http_set_telemetry(const hr_telemetry_t *t)
{
    if (t != NULL && t->valid) {
        s_tel = *t;
        s_tel_valid = true;
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

static esp_err_t h_state(httpd_req_t *req)
{
    char ip[16], ssid[33], serial[64], uid[128];
    hr_wifi_ip(ip, sizeof(ip));
    hr_wifi_current_ssid(ssid, sizeof(ssid));

    LOCK();
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

    char body[900];
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
                     hr_usb_mount_events(), hr_usb_rx_bytes());
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
        if (h != NULL) {
            static char buf[1024];
            int n;
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
 * (read-only queries + BEEP/CLICK). The allow-list is enforced in the tested
 * session layer; anything else is rejected here with 403.
 */
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

    char verb[HR_MAX_VERB] = {0};
    httpd_query_key_value(buf, "verb", verb, sizeof(verb));
    hr_url_decode(verb);

    if (hr_cmd_classify(verb) != HR_CMD_SAFE) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"not allowed\"}");
    }

    bool ok = hr_session_send_safe(s_session, verb);
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
    cfg.max_uri_handlers = 20;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    /* Extra workers so a long-lived /events stream doesn't block the UI. */
    cfg.max_open_sockets = 7;
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
