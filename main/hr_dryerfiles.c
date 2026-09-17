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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hr_files";

#define NVS_NS    "hrfiles"
#define NVS_KEY   "on"
#define NVS_DEPTH "depth"    /* u8 */
#define NVS_GAP   "gap"      /* u16, ms */

#ifndef CONFIG_HR_FILES_PIPELINE_DEPTH
#define CONFIG_HR_FILES_PIPELINE_DEPTH 1
#endif
#ifndef CONFIG_HR_FILES_REQ_GAP_MS
#define CONFIG_HR_FILES_REQ_GAP_MS 0
#endif

static hr_session_t *s_session;
static SemaphoreHandle_t s_lock;
static hr_files_t s_fs;
static bool s_enabled;
static int s_depth = CONFIG_HR_FILES_PIPELINE_DEPTH;
static unsigned long s_gap_ms = CONFIG_HR_FILES_REQ_GAP_MS;

/* The stream's side buffer for whole FDFILEBLOCK frames (hr_big_cb). */
static char s_big[HR_FILES_BIGBUF];

/*
 * Blocks accumulate here (USB RX task, the sink) until the HTTP handler
 * takes them (hr_dryerfiles_take_chunk). A ring in effect: the handler
 * takes whatever has arrived once HR_DF_CHUNK_MIN_BLOCKS are in (or the
 * transfer ended) while the machine keeps asking; the machine is told to
 * WAIT only when fewer than `depth` blocks of room remain - the replies
 * already in flight still fit - and resumes as soon as a take makes room.
 * Allocated for the transfer, freed when the last chunk has been taken.
 */
#define CHUNK_CAP (HR_DF_CHUNK_BLOCKS * HR_FDBLOCK_SIZE)
static struct {
    char *buf;
    size_t len;
    bool full;              /* the sink answered WAIT; take_chunk resumes */
    bool ended;             /* machine finished (done or error) */
    hr_files_err_t err;
} s_chunk;

/* Last list from the dryer. */
static hr_df_entry_t s_list[HR_DF_LIST_MAX];
static unsigned s_nlist;
static bool s_list_valid;
static unsigned long s_list_done_ms;

static bool s_link_up;
static bool s_running;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static unsigned long now_ms(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000);
}

/*
 * One line in the capture for the things this client does, with the "~"
 * prefix the log uses for lines that are not frames ("~boot", "~repeat"):
 * FDFILEBLOCK frames never pass through the frame observer, so without this
 * a transfer would leave no trace next to the STAT frames it ran between.
 * Queued like every other capture line; never touches flash from here.
 */
static void cap_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void cap_note(const char *fmt, ...)
{
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(line, sizeof(line), "~files ");
    if (n > 0 && (size_t)n < sizeof(line)) {
        vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    }
    va_end(ap);
    hr_capture_append((uint32_t)now_ms(), line);
}

/*
 * A name we are willing to send. The wire form is one space-delimited
 * argument and the reply header is comma-delimited; anything that could
 * be read as a second argument, a field separator or a path is refused.
 */
static bool name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    size_t n = strlen(name);
    if (n >= HR_FILES_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c <= 0x20 || c >= 0x7f || c == ',' || c == '"' || c == '/' ||
            c == '\\' || c == '\'' || c == '*' || c == '?') {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* hr_files callbacks                                                  */
/* ------------------------------------------------------------------ */
static bool cb_send(const char *verb, const char *a1, const char *a2, void *u)
{
    (void)u;
    hr_builder_t b;
    hr_build_begin(&b, verb);
    hr_build_str(&b, a1);
    if (a2 != NULL) {
        hr_build_str(&b, a2);
    }
    /* Plaintext on every firmware: 6.0.644170 encodes only what it SENDS
     * (PROTOCOL_NOTES.md, "6.0.644170"), so FDFILES / FILEREAD go out
     * exactly like FDNAME. */
    return hr_session_send(s_session, &b);
}

/* USB RX task, under s_lock (the caller took it). Copy, never flash. */
static int cb_sink(const hr_fdblock_t *b, void *u)
{
    (void)u;
    /* A one-line note per block in the capture, since the data itself never
     * goes through the frame path. */
    cap_note("block %s %ld %ld %ld sum=%02X ok", b->name, b->nbytes, b->block,
             b->size, b->sum);

    if (s_chunk.buf == NULL || s_chunk.len + (size_t)b->nbytes > CHUNK_CAP) {
        return HR_FILES_SINK_FAIL; /* cannot happen: the machine waits */
    }
    memcpy(s_chunk.buf + s_chunk.len, b->data, (size_t)b->nbytes);
    hr_fdblock_unbel(s_chunk.buf + s_chunk.len, (size_t)b->nbytes);
    s_chunk.len += (size_t)b->nbytes;

    /* Room for the replies still in flight (depth - 1) plus one more
     * request? If not, ask for nothing new until the handler takes some. */
    size_t need = (size_t)s_fs.depth * HR_FDBLOCK_SIZE;
    if (CHUNK_CAP - s_chunk.len < need) {
        s_chunk.full = true;
        return HR_FILES_SINK_WAIT;
    }
    return HR_FILES_SINK_OK;
}

static void cb_list(const hr_fdlist_entry_t *e, void *u)
{
    (void)u;
    if (e->end) {
        s_list_valid = true;
        s_list_done_ms = now_ms();
        return;
    }
    if (s_nlist < HR_DF_LIST_MAX) {
        snprintf(s_list[s_nlist].name, sizeof(s_list[s_nlist].name), "%s",
                 e->name);
        s_list[s_nlist].size = e->size;
        s_nlist++;
    }
}

static void cb_done(hr_files_state_t st, hr_files_err_t err, void *u)
{
    (void)u;
    if (s_chunk.buf != NULL) {
        s_chunk.ended = true;
        s_chunk.err = (st == HR_FILES_DONE) ? HR_FILES_ERR_NONE : err;
    }
    const bool listing = (s_fs.name[0] == '\0');
    ESP_LOGI(TAG, "%s %s%s%s: %s%ld in %lu ms (%lu requests, %lu blocks, "
                  "%lu bad, %lu timeouts; first req %lu ms, dryer %lu ms, "
                  "paused for consumer %lu ms, depth %d)",
             listing ? "listing" : "transfer", hr_files_state_str(st),
             err ? ": " : "", hr_files_err_str(err),
             listing ? "entries " : "bytes ",
             listing ? (long)s_fs.entries : s_fs.received,
             now_ms() - s_fs.started_ms, s_fs.requests, s_fs.blocks_ok,
             s_fs.blocks_bad, s_fs.timeouts, s_fs.t_first_ms, s_fs.t_dryer_ms,
             s_fs.t_paused_ms, s_fs.depth);
    cap_note("%s %s %s %ld %s %lu ms first=%lu dryer=%lu paused=%lu depth=%d",
             hr_files_state_str(st), hr_files_err_str(err),
             listing ? s_fs.pattern : s_fs.name,
             listing ? (long)s_fs.entries : s_fs.received,
             listing ? "entries" : "B", now_ms() - s_fs.started_ms,
             s_fs.t_first_ms, s_fs.t_dryer_ms, s_fs.t_paused_ms, s_fs.depth);
}

/* ------------------------------------------------------------------ */
/* Session hook: whole blocks from the stream's side buffer            */
/* ------------------------------------------------------------------ */
static void on_block(const char *frame, size_t len, void *user)
{
    (void)user;
    LOCK();
    hr_files_on_block(&s_fs, frame, len, now_ms());
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
    /* Ask for the next block from the reply itself (USB RX task, like the
     * WIFIINFO answer to REQINFO): measured 70-100 ms a block against the
     * 250 ms of a main-loop tick. */
    s_fs.send_inline = true;
    hr_files_set_sink(&s_fs, cb_sink, NULL);
    hr_files_set_list_cb(&s_fs, cb_list, NULL);
    hr_files_set_done_cb(&s_fs, cb_done, NULL);

#ifdef CONFIG_HR_BATCH_HISTORY_DEFAULT_ON
    bool on = true;
#else
    bool on = false;
#endif
    const char *src = "kconfig", *psrc = "kconfig";
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        uint8_t v = 0;
        uint16_t g = 0;
        if (nvs_get_u8(nh, NVS_KEY, &v) == ESP_OK) {
            on = (v != 0);
            src = "nvs";
        }
        if (nvs_get_u8(nh, NVS_DEPTH, &v) == ESP_OK && v >= 1 &&
            v <= HR_FILES_DEPTH_MAX) {
            s_depth = v;
            psrc = "nvs";
        }
        if (nvs_get_u16(nh, NVS_GAP, &g) == ESP_OK && g <= 5000) {
            s_gap_ms = g;
            psrc = "nvs";
        }
        nvs_close(nh);
    }
    s_enabled = on;
    hr_files_set_pacing(&s_fs, s_depth, s_gap_ms);
    s_depth = s_fs.depth;

    /* The side buffer is lent regardless of the switch: with it the stream
     * collects blocks whole; without it, it swallows them. Either way no
     * block data can ever be mistaken for frames. Requests are what the
     * switch gates. */
    hr_session_set_block_sink(s, s_big, sizeof(s_big), on_block, NULL);
    ESP_LOGI(TAG, "batch history (dryer files): %s (%s); side buffer %u B; "
                  "pipeline depth %d, gap %lu ms (%s)",
             on ? "ON" : "off", src, (unsigned)sizeof(s_big), s_depth,
             s_gap_ms, psrc);
}

void hr_dryerfiles_pacing(int *depth, unsigned long *gap_ms)
{
    if (depth) *depth = s_depth;
    if (gap_ms) *gap_ms = s_gap_ms;
}

bool hr_dryerfiles_set_pacing(int depth, unsigned long gap_ms)
{
    if (s_lock == NULL || depth < 1 || depth > HR_FILES_DEPTH_MAX ||
        gap_ms > 5000) {
        return false;
    }
    LOCK();
    hr_files_set_pacing(&s_fs, depth, gap_ms);
    s_depth = s_fs.depth;
    s_gap_ms = gap_ms;
    UNLOCK();
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed; pacing not persisted");
        return false;
    }
    bool ok = nvs_set_u8(nh, NVS_DEPTH, (uint8_t)depth) == ESP_OK &&
              nvs_set_u16(nh, NVS_GAP, (uint16_t)gap_ms) == ESP_OK &&
              nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    ESP_LOGW(TAG, "pacing set: depth %d, gap %lu ms%s", depth, gap_ms,
             ok ? "" : " (NOT persisted)");
    return ok;
}

bool hr_dryerfiles_enabled(void)
{
    return s_enabled;
}

bool hr_dryerfiles_set_enabled(bool on)
{
    LOCK();
    s_enabled = on;
    if (!on && hr_files_busy(&s_fs)) {
        hr_files_cancel(&s_fs, HR_FILES_ERR_CANCEL, now_ms());
    }
    UNLOCK();
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed; setting not persisted");
        return false;
    }
    bool ok = nvs_set_u8(nh, NVS_KEY, on ? 1 : 0) == ESP_OK &&
              nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    ESP_LOGW(TAG, "batch history set %s%s", on ? "ON" : "off",
             ok ? "" : " (NOT persisted)");
    return ok;
}

void hr_dryerfiles_on_frame(const hr_frame_t *f)
{
    if (f == NULL || strcmp(f->verb, "FDFILELIST") != 0 || s_lock == NULL) {
        return;
    }
    LOCK();
    hr_files_on_frame(&s_fs, f, now_ms());
    UNLOCK();
}

void hr_dryerfiles_tick(unsigned long t, bool link_up, bool dryer_running)
{
    if (s_lock == NULL) {
        return;
    }
    (void)t; /* the loop's clock can be seconds old after a flash write;
                the machine compares against sends stamped with now_ms() */
    LOCK();
    s_link_up = link_up;
    s_running = dryer_running;
    hr_files_tick(&s_fs, now_ms(), link_up);
    UNLOCK();
}

static hr_df_result_t precheck(bool force)
{
    if (!s_enabled) {
        return HR_DF_DISABLED;
    }
    if (hr_files_busy(&s_fs) || s_chunk.buf != NULL) {
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

hr_df_result_t hr_dryerfiles_refresh(const char *pattern, bool force)
{
    if (s_lock == NULL) {
        return HR_DF_DISABLED;
    }
    if (pattern == NULL || pattern[0] == '\0') {
        pattern = ".csv";
    }
    LOCK();
    hr_df_result_t r = precheck(force);
    if (r == HR_DF_OK) {
        s_nlist = 0;
        s_list_valid = false;
        if (!hr_files_start_list(&s_fs, pattern, now_ms())) {
            r = HR_DF_BADNAME;
        } else {
            hr_files_kick(&s_fs, now_ms()); /* first request now, not next tick */
        }
    }
    UNLOCK();
    if (r == HR_DF_OK) {
        ESP_LOGW(TAG, "asking the dryer for its file list (%s)%s", pattern,
                 force ? " [forced]" : "");
        cap_note("list %s%s", pattern, force ? " forced" : "");
    }
    return r;
}

hr_df_result_t hr_dryerfiles_fetch(const char *name, bool force)
{
    if (s_lock == NULL) {
        return HR_DF_DISABLED;
    }
    if (!name_ok(name)) {
        return HR_DF_BADNAME;
    }
    char *buf = malloc(CHUNK_CAP);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no heap for the %u-block chunk buffer",
                 HR_DF_CHUNK_BLOCKS);
        return HR_DF_STORAGE;
    }
    long expected = -1;
    LOCK();
    hr_df_result_t r = precheck(force);
    for (unsigned i = 0; r == HR_DF_OK && i < s_nlist; i++) {
        if (strcmp(s_list[i].name, name) == 0) {
            expected = s_list[i].size;
        }
    }
    if (r == HR_DF_OK) {
        s_chunk.buf = buf;
        buf = NULL;
        s_chunk.len = 0;
        s_chunk.full = false;
        s_chunk.ended = false;
        s_chunk.err = HR_FILES_ERR_NONE;
        if (!hr_files_start_read(&s_fs, name, expected, now_ms())) {
            r = (s_fs.err == HR_FILES_ERR_TOO_BIG) ? HR_DF_TOOBIG
                                                    : HR_DF_BADNAME;
            free(s_chunk.buf);
            s_chunk.buf = NULL;
        } else {
            /* The first FILEREAD goes out from here (the httpd task; the
             * USB transport is mutex-protected) rather than from the main
             * loop's next tick, which measured up to 5 s away after a
             * flash write - most of a 6 KB file's total time. */
            hr_files_kick(&s_fs, now_ms());
        }
    }
    UNLOCK();
    free(buf);
    if (r == HR_DF_OK) {
        ESP_LOGW(TAG, "reading %s from the dryer (%ld bytes expected)%s", name,
                 expected, force ? " [forced]" : "");
        cap_note("read %s %ld%s", name, expected, force ? " forced" : "");
    }
    return r;
}

size_t hr_dryerfiles_take_chunk(char *out, size_t cap, bool *done,
                                hr_files_err_t *err)
{
    size_t n = 0;
    bool fin = false;
    hr_files_err_t e = HR_FILES_ERR_NONE;
    if (s_lock == NULL || out == NULL) {
        if (done) *done = true;
        if (err) *err = HR_FILES_ERR_CANCEL;
        return 0;
    }
    LOCK();
    if (s_chunk.buf == NULL) {
        fin = true;
        e = s_fs.err;
    } else if (s_chunk.full || s_chunk.ended ||
               s_chunk.len >= HR_DF_CHUNK_MIN_BLOCKS * HR_FDBLOCK_SIZE) {
        /* enough to be worth a TCP write, a full buffer, or the tail after
         * the machine finished - hand it out while the dryer keeps going */
        n = s_chunk.len < cap ? s_chunk.len : cap;
        memcpy(out, s_chunk.buf, n);
        if (n < s_chunk.len) {
            memmove(s_chunk.buf, s_chunk.buf + n, s_chunk.len - n);
        }
        s_chunk.len -= n;
        if (s_chunk.len == 0 && s_chunk.ended) {
            fin = true;
            e = s_chunk.err;
            free(s_chunk.buf);
            s_chunk.buf = NULL;
        } else if (s_chunk.full &&
                   CHUNK_CAP - s_chunk.len >=
                       (size_t)(s_fs.depth + 1) * HR_FDBLOCK_SIZE) {
            /* room again for the window plus one: let the machine ask */
            s_chunk.full = false;
            hr_files_resume(&s_fs, now_ms());
        }
    }
    UNLOCK();
    if (done) *done = fin;
    if (err) *err = e;
    return n;
}

void hr_dryerfiles_cancel(void)
{
    if (s_lock == NULL) {
        return;
    }
    LOCK();
    hr_files_cancel(&s_fs, HR_FILES_ERR_CANCEL, now_ms());
    free(s_chunk.buf);
    s_chunk.buf = NULL;
    s_chunk.len = 0;
    s_chunk.full = false;
    s_chunk.ended = false;
    UNLOCK();
}

void hr_dryerfiles_snapshot(hr_df_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    unsigned long t = now_ms();
    LOCK();
    out->enabled = s_enabled;
    out->link_up = s_link_up;
    out->dryer_running = s_running;
    out->state = s_fs.state;
    out->err = s_fs.err;
    snprintf(out->name, sizeof(out->name), "%s", s_fs.name);
    out->block = s_fs.block;
    out->received = s_fs.received;
    out->size = s_fs.size;
    out->pct = hr_files_progress_pct(&s_fs);
    bool busy = hr_files_busy(&s_fs);
    out->age_ms = busy ? 0 : t - s_fs.finished_ms;
    out->elapsed_ms = s_fs.transfers
                          ? (busy ? t : s_fs.finished_ms) - s_fs.started_ms
                          : 0;
    out->list_valid = s_list_valid;
    out->list_age_ms = s_list_valid ? t - s_list_done_ms : 0;
    out->nlist = s_nlist;
    memcpy(out->list, s_list, sizeof(hr_df_entry_t) * s_nlist);
    out->requests = s_fs.requests;
    out->timeouts = s_fs.timeouts;
    out->blocks_ok = s_fs.blocks_ok;
    out->blocks_bad = s_fs.blocks_bad;
    out->transfers = s_fs.transfers;
    out->blocks_in = s_session ? s_session->blocks_in : 0;
    out->big_dropped = s_session ? s_session->stream.big_dropped : 0;
    out->depth = s_fs.depth;
    out->gap_ms = s_fs.gap_ms;
    out->t_first_ms = s_fs.t_first_ms;
    out->t_dryer_ms = s_fs.t_dryer_ms;
    out->t_paused_ms = s_fs.t_paused_ms;
    UNLOCK();
}

int hr_dryerfiles_state_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    if (s_lock == NULL) {
        return snprintf(out, cap, "\"files_state\":\"off\",");
    }
    LOCK();
    hr_files_state_t st = s_fs.state;
    hr_files_err_t err = s_fs.err;
    unsigned long req = s_fs.requests, to = s_fs.timeouts,
                  ok = s_fs.blocks_ok, bad = s_fs.blocks_bad,
                  tr = s_fs.transfers;
    int depth = s_fs.depth;
    bool en = s_enabled;
    UNLOCK();
    int n = snprintf(out, cap,
                     "\"files_enabled\":%s,\"files_state\":\"%s\","
                     "\"files_error\":\"%s\",\"files_requests\":%lu,"
                     "\"files_timeouts\":%lu,\"files_blocks_ok\":%lu,"
                     "\"files_blocks_bad\":%lu,\"files_transfers\":%lu,"
                     "\"files_depth\":%d,",
                     en ? "true" : "false", hr_files_state_str(st),
                     hr_files_err_str(err), req, to, ok, bad, tr, depth);
    return (n < 0 || (size_t)n >= cap) ? 0 : n;
}

const char *hr_df_result_str(hr_df_result_t r)
{
    switch (r) {
    case HR_DF_OK:       return "ok";
    case HR_DF_DISABLED: return "batch history is switched off in settings";
    case HR_DF_BUSY:     return "a transfer is already running";
    case HR_DF_LINK:     return "no link to the dryer";
    case HR_DF_RUNNING:  return "the dryer is running a batch (force=1 to override)";
    case HR_DF_BADNAME:  return "not a file name the dryer would have";
    case HR_DF_STORAGE:  return "no memory for the transfer";
    case HR_DF_TOOBIG:   return "file too large to fetch";
    default:             return "?";
    }
}
