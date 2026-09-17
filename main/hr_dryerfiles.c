/*
 * hr_dryerfiles - see hr_dryerfiles.h.
 */
#include "hr_dryerfiles.h"

#include "hr_capture.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "hr_files";

#define NVS_NS  "hrfiles"
#define NVS_KEY "on"

static hr_session_t *s_session;
static SemaphoreHandle_t s_lock;
static hr_files_t s_fs;
static bool s_enabled;
static bool s_link_up;
static bool s_running;

/* The stream's side buffer, where a whole block frame is assembled. */
static char s_side[HR_FILE_LONGBUF];

/*
 * The ring between the USB RX task, which fills it a block at a time, and the
 * web server, which empties it. Allocated for a transfer and freed once the
 * last byte has been taken - there is no reason to hold 8 KB of a small heap
 * while nothing is being read.
 */
static struct {
    char *buf;
    size_t head;      /* next byte to give out */
    size_t len;       /* bytes in hand */
    bool ended;       /* the machine finished, one way or the other */
    unsigned long last_take_ms;
} s_ring;

static hr_df_entry_t s_list[HR_DF_LIST_MAX];
static unsigned s_nlist;
static bool s_list_valid;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static unsigned long now_ms(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000);
}

const char *hr_df_result_str(hr_df_result_t r)
{
    switch (r) {
    case HR_DF_OK:       return "ok";
    case HR_DF_DISABLED: return "switched off";
    case HR_DF_BUSY:     return "busy";
    case HR_DF_LINK:     return "no dryer";
    case HR_DF_RUNNING:  return "batch running";
    case HR_DF_BADNAME:  return "name refused";
    case HR_DF_TOOBIG:   return "file too big";
    case HR_DF_NOMEM:    return "out of memory";
    default:             return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Callbacks from hr_files - all of them under the lock already         */
/* ------------------------------------------------------------------ */
static bool cb_send(const char *verb, const char *arg, const char *index,
                    void *user)
{
    (void)user;
    hr_builder_t b;
    hr_build_begin(&b, verb);
    hr_build_str(&b, arg);
    hr_build_str(&b, index);
    /* Plaintext on either firmware: a 6.0.644170 machine encodes what it
     * SENDS, not what it accepts. Same path as FDNAME and REQCFG. */
    return hr_session_send(s_session, &b);
}

static void cb_entry(const hr_file_entry_t *e, void *user)
{
    (void)user;
    if (e->last) {
        s_list_valid = true;
        return;
    }
    if (s_nlist < HR_DF_LIST_MAX) {
        snprintf(s_list[s_nlist].name, sizeof(s_list[s_nlist].name), "%s",
                 e->name);
        s_list[s_nlist].size = e->size;
        s_nlist++;
    }
}

static bool cb_data(const char *data, size_t n, void *user)
{
    (void)user;
    if (s_ring.buf == NULL || n > HR_DF_RING_CAP - s_ring.len) {
        return false;   /* nowhere to put it: the transfer pauses */
    }
    /* Two copies at most, since the ring wraps. */
    size_t at = (s_ring.head + s_ring.len) % HR_DF_RING_CAP;
    size_t first = HR_DF_RING_CAP - at;
    if (first > n) {
        first = n;
    }
    memcpy(s_ring.buf + at, data, first);
    if (n > first) {
        memcpy(s_ring.buf, data + first, n - first);
    }
    s_ring.len += n;
    return true;
}

static void cb_done(hr_files_state_t st, hr_files_err_t err, void *user)
{
    (void)user;
    s_ring.ended = true;
    ESP_LOGW(TAG, "\"%s\" %s %s: %ld bytes, %ld entries, %lu ms "
                  "(%lu requests, %lu blocks, %lu bad, %lu timeouts)",
             s_fs.arg, hr_files_state_str(st), hr_files_err_str(err),
             s_fs.received, s_fs.entries, now_ms() - s_fs.started_ms,
             s_fs.requests, s_fs.blocks_ok, s_fs.blocks_bad, s_fs.timeouts);
    /* One line in the capture beside the STAT frames it ran between; block
     * frames never pass through the frame observer. Queued, never flash. */
    char line[128];
    snprintf(line, sizeof(line), "~files %s %s %ld B %lu ms", s_fs.arg,
             hr_files_state_str(st), s_fs.received,
             now_ms() - s_fs.started_ms);
    hr_capture_append((uint32_t)now_ms(), line);
}

/* A whole block frame out of the session's side buffer, on the RX task. */
static void on_block(char *frame, size_t len, void *user)
{
    (void)user;
    if (s_lock == NULL) {
        return;
    }
    LOCK();
    hr_files_on_block(&s_fs, frame, len, now_ms());
    /*
     * Ask for the next block from here rather than waiting for the main
     * loop's next pass: the dryer answers in about 170 ms, and a 250 ms tick
     * between requests would add half again to every transfer. This is the
     * same path the WIFIINFO answer to REQINFO already takes.
     */
    hr_files_tick(&s_fs, now_ms(), s_link_up);
    UNLOCK();
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */
void hr_dryerfiles_init(hr_session_t *s)
{
    s_session = s;
    s_lock = xSemaphoreCreateMutex();
    hr_files_init(&s_fs, cb_send, NULL);
    hr_files_set_entry_cb(&s_fs, cb_entry, NULL);
    hr_files_set_data_cb(&s_fs, cb_data, NULL);
    hr_files_set_done_cb(&s_fs, cb_done, NULL);

#ifdef CONFIG_HR_BATCH_HISTORY_DEFAULT_ON
    bool on = true;
#else
    bool on = false;
#endif
    const char *src = "kconfig";
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(nh, NVS_KEY, &v) == ESP_OK) {
            on = (v != 0);
            src = "nvs";
        }
        nvs_close(nh);
    }
    s_enabled = on;

    /*
     * The side buffer is lent whatever the switch says. With it, a block
     * frame is collected whole; without it the framer would have to swallow
     * one, and either way it can never be mistaken for frames. What the
     * switch gates is asking for files in the first place.
     */
    hr_session_set_file_sink(s, s_side, sizeof(s_side), on_block, NULL);
    ESP_LOGI(TAG, "batch history: %s (%s); side buffer %u B",
             on ? "on" : "off", src, (unsigned)sizeof(s_side));
}

bool hr_dryerfiles_enabled(void)
{
    return s_enabled;
}

bool hr_dryerfiles_set_enabled(bool on)
{
    if (s_lock == NULL) {
        return false;
    }
    LOCK();
    s_enabled = on;
    if (!on) {
        hr_files_cancel(&s_fs, now_ms());
    }
    UNLOCK();
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed; the switch is not persisted");
        return false;
    }
    const bool ok = nvs_set_u8(nh, NVS_KEY, on ? 1 : 0) == ESP_OK &&
                    nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    ESP_LOGW(TAG, "batch history switched %s%s", on ? "on" : "off",
             ok ? "" : " (NOT persisted)");
    return ok;
}

/* Common gate for both kinds of request. Call with the lock held. */
static hr_df_result_t precheck(bool force)
{
    if (!s_enabled) {
        return HR_DF_DISABLED;
    }
    if (hr_files_busy(&s_fs) || s_ring.buf != NULL) {
        return HR_DF_BUSY;
    }
    if (!s_link_up) {
        return HR_DF_LINK;
    }
    if (s_running && !force) {
        return HR_DF_RUNNING;
    }
    return HR_DF_OK;
}

hr_df_result_t hr_dryerfiles_list(const char *pattern, bool force)
{
    if (s_lock == NULL) {
        return HR_DF_DISABLED;
    }
    if (pattern == NULL || pattern[0] == '\0') {
        pattern = ".csv";
    }
    if (!hr_files_arg_ok(pattern)) {
        return HR_DF_BADNAME;
    }
    LOCK();
    hr_df_result_t r = precheck(force);
    if (r == HR_DF_OK) {
        s_nlist = 0;
        s_list_valid = false;
        if (!hr_files_list(&s_fs, pattern, now_ms())) {
            r = HR_DF_BADNAME;
        } else {
            hr_files_tick(&s_fs, now_ms(), s_link_up);  /* ask now */
        }
    }
    UNLOCK();
    if (r == HR_DF_OK) {
        ESP_LOGW(TAG, "asking the dryer what it has matching \"%s\"%s",
                 pattern, force ? " (forced)" : "");
    }
    return r;
}

hr_df_result_t hr_dryerfiles_read(const char *name, bool force)
{
    if (s_lock == NULL) {
        return HR_DF_DISABLED;
    }
    if (!hr_files_arg_ok(name) || name[0] == '.') {
        return HR_DF_BADNAME;
    }
    char *ring = malloc(HR_DF_RING_CAP);
    if (ring == NULL) {
        ESP_LOGE(TAG, "no heap for the %u B transfer ring", HR_DF_RING_CAP);
        return HR_DF_NOMEM;
    }
    LOCK();
    hr_df_result_t r = precheck(force);
    long size = -1;
    for (unsigned i = 0; r == HR_DF_OK && i < s_nlist; i++) {
        if (strcmp(s_list[i].name, name) == 0) {
            size = s_list[i].size;
        }
    }
    if (r == HR_DF_OK) {
        s_ring.buf = ring;
        s_ring.head = 0;
        s_ring.len = 0;
        s_ring.ended = false;
        s_ring.last_take_ms = now_ms();
        ring = NULL;
        if (!hr_files_read(&s_fs, name, size, now_ms())) {
            free(s_ring.buf);
            s_ring.buf = NULL;
            r = (s_fs.err == HR_FILES_ERR_TOO_BIG) ? HR_DF_TOOBIG
                                                   : HR_DF_BADNAME;
        } else {
            hr_files_tick(&s_fs, now_ms(), s_link_up);
        }
    }
    UNLOCK();
    free(ring);   /* NULL if it was taken */
    if (r == HR_DF_OK) {
        ESP_LOGW(TAG, "reading \"%s\" off the dryer%s", name,
                 force ? " (forced)" : "");
    }
    return r;
}

void hr_dryerfiles_cancel(void)
{
    if (s_lock == NULL) {
        return;
    }
    LOCK();
    hr_files_cancel(&s_fs, now_ms());
    s_ring.ended = true;
    UNLOCK();
}

size_t hr_dryerfiles_take(char *out, size_t cap, bool *more)
{
    size_t n = 0;
    if (s_lock == NULL || out == NULL) {
        if (more != NULL) {
            *more = false;
        }
        return 0;
    }
    LOCK();
    s_ring.last_take_ms = now_ms();
    if (s_ring.buf != NULL && s_ring.len > 0) {
        n = s_ring.len < cap ? s_ring.len : cap;
        const size_t first = HR_DF_RING_CAP - s_ring.head;
        const size_t part = first < n ? first : n;
        memcpy(out, s_ring.buf + s_ring.head, part);
        if (n > part) {
            memcpy(out + part, s_ring.buf, n - part);
        }
        s_ring.head = (s_ring.head + n) % HR_DF_RING_CAP;
        s_ring.len -= n;
    }
    /* Room again: let a paused transfer carry on. */
    if (hr_files_busy(&s_fs) && s_fs.paused &&
        HR_DF_RING_CAP - s_ring.len >= HR_FILE_BLOCK) {
        hr_files_resume(&s_fs, now_ms());
        hr_files_tick(&s_fs, now_ms(), s_link_up);
    }
    const bool running = hr_files_busy(&s_fs);
    if (more != NULL) {
        *more = running || s_ring.len > 0;
    }
    /* Everything taken and nothing more coming: give the heap back. */
    if (!running && s_ring.len == 0 && s_ring.buf != NULL) {
        free(s_ring.buf);
        s_ring.buf = NULL;
    }
    UNLOCK();
    return n;
}

void hr_dryerfiles_on_frame(const hr_frame_t *f)
{
    if (f == NULL || s_lock == NULL || strcmp(f->verb, "FDFILELIST") != 0) {
        return;
    }
    LOCK();
    hr_files_on_frame(&s_fs, f, now_ms());
    hr_files_tick(&s_fs, now_ms(), s_link_up);   /* the next index, at once */
    UNLOCK();
}

void hr_dryerfiles_tick(bool link_up, bool dryer_running)
{
    if (s_lock == NULL) {
        return;
    }
    LOCK();
    s_link_up = link_up;
    s_running = dryer_running;
    /*
     * A browser that closed the tab mid-transfer leaves the dryer answering
     * into a ring nobody empties. Give it up rather than tie up the link.
     */
    if (s_ring.buf != NULL && hr_files_busy(&s_fs) &&
        now_ms() - s_ring.last_take_ms > HR_DF_IDLE_MS) {
        ESP_LOGW(TAG, "nobody has taken a byte for %lus; ending the transfer",
                 HR_DF_IDLE_MS / 1000);
        hr_files_cancel(&s_fs, now_ms());
        s_ring.ended = true;
    }
    hr_files_tick(&s_fs, now_ms(), link_up);
    /* A transfer that ended with nobody left to read it frees its ring. */
    if (s_ring.buf != NULL && !hr_files_busy(&s_fs) && s_ring.ended &&
        now_ms() - s_ring.last_take_ms > HR_DF_IDLE_MS) {
        free(s_ring.buf);
        s_ring.buf = NULL;
        s_ring.len = 0;
    }
    UNLOCK();
}

void hr_dryerfiles_snapshot(hr_df_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        out->size = -1;
        out->pct = -1;
        return;
    }
    LOCK();
    out->enabled = s_enabled;
    out->link_up = s_link_up;
    out->dryer_running = s_running;
    out->state = s_fs.state;
    out->err = s_fs.err;
    out->busy = hr_files_busy(&s_fs);
    snprintf(out->file, sizeof(out->file), "%s", s_fs.arg);
    out->received = s_fs.received;
    out->size = s_fs.size;
    out->pct = (s_fs.size > 0)
                   ? (int)((s_fs.received * 100) / s_fs.size)
                   : -1;
    out->elapsed_ms = out->busy ? now_ms() - s_fs.started_ms : 0;
    out->list_valid = s_list_valid;
    out->nlist = s_nlist;
    memcpy(out->list, s_list, sizeof(out->list));
    out->requests = s_fs.requests;
    out->timeouts = s_fs.timeouts;
    out->blocks_ok = s_fs.blocks_ok;
    out->blocks_bad = s_fs.blocks_bad;
    out->transfers = s_fs.transfers;
    out->blocks_in = s_session != NULL ? s_session->blocks_in : 0;
    out->blocks_dropped = s_session != NULL ? s_session->stream.long_dropped
                                            : 0;
    UNLOCK();
}

int hr_dryerfiles_state_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    if (s_lock == NULL) {
        const int n = snprintf(out, cap, "\"files_state\":\"off\",");
        return (n < 0 || (size_t)n >= cap) ? 0 : n;
    }
    LOCK();
    const hr_files_state_t st = s_fs.state;
    const hr_files_err_t err = s_fs.err;
    const bool on = s_enabled;
    const unsigned long req = s_fs.requests, to = s_fs.timeouts;
    const unsigned long ok = s_fs.blocks_ok, bad = s_fs.blocks_bad;
    const unsigned long dropped = s_session != NULL
                                      ? s_session->stream.long_dropped : 0;
    UNLOCK();
    const int n = snprintf(out, cap,
                           "\"files_enabled\":%s,\"files_state\":\"%s\","
                           "\"files_error\":\"%s\",\"files_requests\":%lu,"
                           "\"files_timeouts\":%lu,\"files_blocks_ok\":%lu,"
                           "\"files_blocks_bad\":%lu,"
                           "\"files_blocks_dropped\":%lu,",
                           on ? "true" : "false", hr_files_state_str(st),
                           hr_files_err_str(err), req, to, ok, bad, dropped);
    /* Refuse rather than truncate: a half-written pair would break the JSON
     * the whole dashboard reads. */
    return (n < 0 || (size_t)n >= cap) ? 0 : n;
}
