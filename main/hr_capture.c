#include "hr_capture.h"

#include "esp_timer.h"

#include "esp_log.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "hr_capture";
/* Longest frame line we will queue. Matches HR_MAX_FRAME plus the timestamp
   prefix; anything longer is truncated rather than dropped. */
#define HR_CAPTURE_LINE_MAX 544
#define MOUNT "/capture"
/* Graph series, separate from the frame log: compact, fixed-width records so a
   restore is a straight read rather than a re-parse of the text log. */
#define TRENDFILE MOUNT "/trend.bin"
/*
 * ROTATING SEGMENTS.
 *
 * This was one file, "/frames.log", that STOPPED appending when the partition
 * neared full. On a dryer left running that is a log which quietly becomes
 * useless: the oldest hours are preserved and everything since is discarded,
 * which is the wrong half to keep. Defrost and end-of-cycle states live at the
 * END of a run, so a stopped log loses exactly what we are trying to capture.
 *
 * Four segments, written in turn. On rotation the NEXT segment is deleted
 * before it becomes active, so the oldest quarter is what goes. Recording
 * never stops.
 *
 * Four rather than the batch logbook's two: a rotation costs 25% of history
 * instead of 50%, so the retained window never drops below three quarters of
 * capacity - about 2MB, or roughly two full cycles.
 */
#define CAP_SEGMENTS 4

static const char *const k_seg[CAP_SEGMENTS] = {
    MOUNT "/frames.0.log",
    MOUNT "/frames.1.log",
    MOUNT "/frames.2.log",
    MOUNT "/frames.3.log",
};

/*
 * Held back from the segments so the filesystem is never driven to genuinely
 * full: SPIFFS becomes awkward to read or clear there, and the trend file
 * (~43KB) shares this partition and must always have room to be rewritten.
 */
#define RESERVE_BYTES (128 * 1024)

/*
 * When the log already on flash will not fit the budget, the per-segment
 * ceiling gives way before recorded frames do - but not below this floor,
 * or rotation would thrash.
 */
#define SEG_MIN_BYTES  (64 * 1024)
#define SEG_STEP_BYTES (4 * 1024)

static bool s_ready;
static SemaphoreHandle_t s_lock;
static size_t s_total;      /* partition capacity */
static uint8_t s_seg;       /* active segment index */
static size_t s_seg_used;   /* bytes in the active segment */
static size_t s_seg_max;    /* per-segment cap, computed at mount */
static unsigned long s_rotations;

/*
 * RUN COLLAPSING.
 *
 * An idle dryer repeats itself. REQINFO arrives every 10 seconds doing
 * nothing, and every 2 seconds while a panel sits on the WiFi screen - where
 * it was measured at 80% of all inbound frames. Writing each one costs flash
 * and buys nothing: consecutive identical frames carry no information beyond
 * "still the same, still going".
 *
 * Identical consecutive bodies are collapsed into a single summary line:
 *
 *     45231  REQINFO,
 *     105240 ~repeat REQINFO, x30 45231..105240
 *
 * The count and the time span are kept, so the CADENCE survives - which
 * matters, because the 10s-vs-2s REQINFO rate is how we identify a machine
 * parked on its WiFi screen. The summary is strictly denser than the frames
 * it replaces, not lossier.
 *
 * A run is flushed when a different frame arrives, when the log is read or
 * cleared, and every RUN_MAX_MS regardless - otherwise a dryer left idle for
 * hours would leave the whole stretch invisible until something changed.
 */
#define RUN_MAX_MS 60000UL

static char     s_run_body[HR_CAPTURE_LINE_MAX];
static uint32_t s_run_count;      /* repeats SUPPRESSED, not including the first */
static uint32_t s_run_first_ms;
static uint32_t s_run_last_ms;
static unsigned long s_suppressed;
static size_t s_used;       /* bytes written to the log */
static bool s_full_warned;

/* ------------------------------------------------------------------ */
/* Write queue                                                         */
/* ------------------------------------------------------------------ */
/*
 * All flash writes happen on ONE worker task fed by a queue.
 *
 * hr_capture_append() is called from the USB CDC RX callback - i.e. on the
 * TinyUSB task. Doing fopen/fprintf/fclose there overflowed its stack and
 * panicked the chip on the first frame the dryer sent, which is the bug behind
 * the v0.3-v0.3.3 withdrawal. Beyond the stack, blocking that task on flash
 * also stalls USB reception, so frames can be missed during a long run.
 *
 * The queue is deliberately small: it exists to decouple tasks, not to buffer
 * minutes of data. If it fills we drop the line and count it, because blocking
 * the USB task is the one thing we must never do.
 */
#define WRITE_Q_LEN 12

/* One 12-byte record per graph point. Fixed width so hr_capture_trend_load()
   can size the series from the file length alone. */
typedef struct __attribute__((packed)) {
    uint32_t elapsed_s;   /* the DRYER's batch clock, not our uptime */
    int16_t temp_raw_f;
    int16_t temp_smooth_cf;
    uint32_t pressure_raw;
} trend_rec_t;

typedef struct {
    uint32_t t_ms;
    char body[HR_CAPTURE_LINE_MAX];
} write_msg_t;

static QueueHandle_t s_q;
static volatile unsigned long s_dropped;
/* Count write failures separately from queue drops: a full queue and a failed
   fopen/lock look identical from outside, and they have different fixes. */
static volatile unsigned long s_trend_writes, s_trend_fails;
unsigned long hr_capture_trend_writes(void) { return s_trend_writes; }
unsigned long hr_capture_trend_fails(void) { return s_trend_fails; }

static size_t seg_size(unsigned i)
{
    struct stat st;
    return (stat(k_seg[i], &st) == 0) ? (size_t)st.st_size : 0;
}

/* What the whole set would occupy once every segment has reached the ceiling. */
static size_t projected_set(const size_t *sz, size_t seg_max)
{
    size_t p = 0;
    for (unsigned i = 0; i < CAP_SEGMENTS; i++) {
        p += (sz[i] > seg_max) ? sz[i] : seg_max;
    }
    return p;
}

/*
 * Segments in age order: index 0 is the oldest surviving, CAP_SEGMENTS-1 is
 * the active one. The segment immediately after the active is the oldest,
 * because it is the one deleted at the next rotation.
 */
static unsigned seg_at(unsigned age_order)
{
    return (unsigned)((s_seg + 1u + age_order) % CAP_SEGMENTS);
}

static size_t seg_total_used(void)
{
    size_t n = 0;
    for (unsigned i = 0; i < CAP_SEGMENTS; i++) {
        n += seg_size(i);
    }
    return n;
}

/* Remember which segment is active so a reboot does not append to the oldest
 * one and invert the age order - the same reason hr_batchstore persists it. */
static void seg_save(void)
{
    nvs_handle_t nh;
    if (nvs_open("hrcap", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u8(nh, "seg", s_seg);
        nvs_commit(nh);
        nvs_close(nh);
    }
}

static void seg_load(void)
{
    nvs_handle_t nh;
    s_seg = 0;
    if (nvs_open("hrcap", NVS_READONLY, &nh) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(nh, "seg", &v) == ESP_OK && v < CAP_SEGMENTS) {
            s_seg = v;
        }
        nvs_close(nh);
    }
}

/*
 * Write one line to the active segment. Caller holds the lock and has already
 * rotated if needed.
 */
static void emit(uint32_t t_ms, const char *body)
{
    FILE *f = fopen(k_seg[s_seg], "a");
    if (f == NULL) {
        return;
    }
    int n = fprintf(f, "%" PRIu32 "\t%s\n", t_ms, body);
    fclose(f);
    if (n > 0) {
        s_used += (size_t)n;
        s_seg_used += (size_t)n;
    }
}

/* Emit the pending run summary, if any. Caller holds the lock. */
static void flush_run(void)
{
    if (s_run_count == 0) {
        return;
    }
    char line[HR_CAPTURE_LINE_MAX + 64];
    snprintf(line, sizeof(line), "~repeat %s x%" PRIu32 " %" PRIu32 "..%" PRIu32,
             s_run_body, s_run_count, s_run_first_ms, s_run_last_ms);
    emit(s_run_last_ms, line);
    s_run_count = 0;
}

/* Runs on the worker task: the only place that touches the filesystem. */
static void write_line_now(const write_msg_t *m)
{
    if (!s_ready) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }

    /*
     * Rotate BEFORE writing so a line is never split across two segments.
     * The estimate is deliberately generous - an over-estimate rotates one
     * line early, an under-estimate would truncate a frame.
     */
    size_t need = strlen(m->body) + 16;
    /* A pending summary belongs with the frames it describes. */
    if (s_seg_max && s_seg_used + need > s_seg_max) {
        unsigned next = (unsigned)((s_seg + 1u) % CAP_SEGMENTS);
        size_t dropped = seg_size(next);
        remove(k_seg[next]);
        s_seg = (uint8_t)next;
        s_seg_used = 0;
        s_rotations++;
        seg_save();
        s_used = seg_total_used();
        ESP_LOGW(TAG, "capture rotated to seg%u; dropped %u bytes of the "
                      "oldest frames (rotation %lu)",
                 next, (unsigned)dropped, s_rotations);
    }

    /*
     * Same frame as last time? Extend the run instead of writing it. The run
     * is still flushed every RUN_MAX_MS so a long idle stretch is never
     * entirely absent from the log.
     */
    if (s_run_count > 0 && strcmp(m->body, s_run_body) == 0) {
        s_run_count++;
        s_run_last_ms = m->t_ms;
        s_suppressed++;
        if (m->t_ms - s_run_first_ms >= RUN_MAX_MS) {
            flush_run();
            snprintf(s_run_body, sizeof(s_run_body), "%s", m->body);
            s_run_first_ms = m->t_ms;
            s_run_last_ms = m->t_ms;
        }
        xSemaphoreGive(s_lock);
        return;
    }
    if (s_run_count == 0 && s_run_body[0] != '\0' &&
        strcmp(m->body, s_run_body) == 0) {
        /* second sighting - start a run rather than writing the duplicate */
        s_run_count = 1;
        s_run_first_ms = s_run_last_ms = m->t_ms;
        s_suppressed++;
        xSemaphoreGive(s_lock);
        return;
    }

    flush_run();
    emit(m->t_ms, m->body);
    snprintf(s_run_body, sizeof(s_run_body), "%s", m->body);
    xSemaphoreGive(s_lock);
}

static void writer_task(void *arg)
{
    (void)arg;
    write_msg_t m;
    for (;;) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) == pdTRUE) {
            write_line_now(&m);
        }
    }
}

void hr_capture_append(uint32_t t_ms, const char *body)
{
    if (s_q == NULL || body == NULL || body[0] == '\0') {
        return;
    }
    write_msg_t m;
    m.t_ms = t_ms;
    snprintf(m.body, sizeof(m.body), "%s", body);
    /* Zero timeout: never block the caller, which may be the USB RX task. */
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        s_dropped++;
    }
}

unsigned long hr_capture_dropped(void) { return s_dropped; }

size_t hr_capture_trend_load(hr_trend_t *tr, uint32_t *last_elapsed)
{
    if (!s_ready || tr == NULL) {
        return 0;
    }
    FILE *f = fopen(TRENDFILE, "rb");
    if (f == NULL) {
        return 0;
    }
    size_t n = 0;
    trend_rec_t r;
    while (fread(&r, sizeof(r), 1, f) == 1) {
        hr_trend_point_t p;
        p.temp_raw_f = r.temp_raw_f;
        p.temp_smooth_cf = r.temp_smooth_cf;
        p.pressure_raw = r.pressure_raw;
        if (!hr_trend_restore_point(tr, &p)) {
            break;   /* ring full - keep the earliest data, drop the rest */
        }
        if (last_elapsed != NULL) {
            *last_elapsed = r.elapsed_s;
        }
        n++;
    }
    fclose(f);
    return n;
}

/*
 * Persist the whole series in one truncating write.
 *
 * The first design appended one record per bucket, which is cheaper - but
 * SPIFFS refused append mode on this file with EIO every single time
 * (writes=0, fails climbing, confirmed on hardware). "wb" is the mode SPIFFS
 * handles reliably, and rewriting removes a subtler problem too: an appended
 * file could disagree with the in-RAM series after a restore, because restored
 * gap buckets were never themselves written. A rewrite is self-consistent by
 * construction.
 *
 * Called from the MAIN LOOP, not the USB RX callback. That path still must
 * never touch flash - see hr_capture_append().
 */
bool hr_capture_trend_save(const hr_trend_t *tr, uint32_t batch_elapsed_s)
{
    if (!s_ready || tr == NULL) {
        return false;
    }
    size_t n = hr_trend_count(tr);
    if (n == 0) {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        s_trend_fails++;
        return false;
    }
    bool ok = false;
    FILE *f = fopen(TRENDFILE, "wb");
    if (f != NULL) {
        ok = true;
        for (size_t i = 0; i < n && ok; i++) {
            hr_trend_point_t p;
            if (!hr_trend_get(tr, i, &p)) {
                ok = false;
                break;
            }
            trend_rec_t r;
            r.elapsed_s = batch_elapsed_s;
            r.temp_raw_f = p.temp_raw_f;
            r.temp_smooth_cf = p.temp_smooth_cf;
            r.pressure_raw = p.pressure_raw;
            ok = (fwrite(&r, sizeof(r), 1, f) == 1);
        }
        if (fclose(f) != 0) {
            ok = false;
        }
    } else {
        /* Rate-limited rather than once-only: the first failure had one cause
           and the next had another, and "once" hid the second. */
        static unsigned n_warn;
        if ((n_warn++ % 32) == 0) {
            ESP_LOGE(TAG, "trend fopen(%s) failed: %s (errno %d)", TRENDFILE,
                     strerror(errno), errno);
        }
    }
    if (ok) {
        s_trend_writes++;
    } else {
        s_trend_fails++;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

size_t hr_capture_trend_bytes(void)
{
    if (!s_ready) {
        return 0;
    }
    struct stat st;
    return (stat(TRENDFILE, &st) == 0) ? (size_t)st.st_size : 0;
}

void hr_capture_trend_reset(void)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    remove(TRENDFILE);
    xSemaphoreGive(s_lock);
}

/*
 * Mount worker.
 *
 * Mounting is deferred off the boot path so the ~190ms of SPI flash activity
 * (each erase/write suspends the instruction cache) does not overlap USB
 * enumeration. This is robustness, NOT the fix for the USB regression - that
 * was a stack overflow, see CONFIG_TINYUSB_TASK_STACK_SIZE in
 * sdkconfig.defaults. Deferring the mount alone did not help, because what
 * actually broke USB was hr_capture_append() running on the TinyUSB task.
 *
 * The capture log simply becomes available a few seconds into the boot;
 * nothing else depends on it being ready immediately.
 */
static void capture_mount_task(void *arg)
{
    (void)arg;
    /* Let enumeration and the first control transfers complete first. */
    vTaskDelay(pdMS_TO_TICKS(HR_CAPTURE_MOUNT_DELAY_MS));

    int64_t t0 = esp_timer_get_time();
    hr_capture_mount_now();
    ESP_LOGI(TAG, "capture mount took %lld ms",
             (long long)((esp_timer_get_time() - t0) / 1000));
    vTaskDelete(NULL);
}

void hr_capture_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    /* Queue first: hr_capture_append() must be safe to call the moment the USB
       stack starts delivering frames, even before the mount completes. Queued
       lines are simply discarded by the worker until s_ready. */
    if (s_q == NULL) {
        s_q = xQueueCreate(WRITE_Q_LEN, sizeof(write_msg_t));
    }
    if (s_q == NULL) {
        ESP_LOGE(TAG, "could not create write queue; capture log disabled");
        return;
    }
    /* 4096 is enough here and only here: this task exists precisely so that
       nothing else has to carry the VFS/newlib file-I/O stack depth. */
    if (xTaskCreate(writer_task, "cap_write", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start writer task; capture log disabled");
        return;
    }
    if (xTaskCreate(capture_mount_task, "cap_mount", 4096, NULL, 2, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "could not start mount task; capture log disabled");
    }
}

void hr_capture_mount_now(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MOUNT,
        .partition_label = "capture",
        /*
         * Descriptors, and why this keeps growing.
         *
         * 2 starved the frame writer. 6 was enough until the logbook arrived,
         * which added an append path AND a reader that holds a descriptor open
         * for the whole streamed response - so two browsers each fetching
         * /api/batches, plus the frame writer and a trend save, exceeded it and
         * the trend save started failing with EIO.
         *
         * The failure mode is quiet: writes fail, reads still work, and nothing
         * is obviously broken until a graph is missing. Budget generously.
         */
        .max_files = 12,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (capture log disabled)",
                 esp_err_to_name(err));
        return;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info("capture", &total, &used) == ESP_OK) {
        s_total = total;
    }
    seg_load();
    s_seg_used = seg_size(s_seg);
    s_used = seg_total_used();
    s_seg_max = (s_total > RESERVE_BYTES)
                    ? (s_total - RESERVE_BYTES) / CAP_SEGMENTS
                    : 0;

    /*
     * Migrate a pre-rotation single file into segment 0 rather than orphaning
     * it - it holds real captured frames somebody may still want.
     */
    struct stat st;
    if (stat(MOUNT "/frames.log", &st) == 0) {
        if (seg_size(0) == 0 && rename(MOUNT "/frames.log", k_seg[0]) == 0) {
            ESP_LOGW(TAG, "migrated legacy frames.log (%u bytes) into seg0",
                     (unsigned)st.st_size);
        } else {
            remove(MOUNT "/frames.log");
        }
        s_seg_used = seg_size(s_seg);
        s_used = seg_total_used();
    }

    /*
     * ENFORCE THE TOTAL BUDGET.
     *
     * Four segments at s_seg_max fit the partition by construction. A segment
     * can still arrive OVERSIZED - a migrated pre-rotation frames.log is
     * bigger than any single segment - and one of those breaks the invariant:
     * on this partition an oversized segment plus three full ones comes to
     * 3,045,533 bytes against a 2,884,241 capacity.
     *
     * That overflow is silent. SPIFFS starts refusing writes and emit() has no
     * way to report it, so frames would simply stop appearing - the exact
     * failure this rotation work exists to remove.
     *
     * Checking only the ACTIVE segment is not enough: a rotation moves off an
     * oversized segment while leaving it in the set, so it survives as history
     * and the overflow arrives later, on a boot that looks healthy.
     *
     * So: project the set at its EVENTUAL largest. Every segment reaches
     * s_seg_max before the rotation comes round to delete it, and an oversized
     * one stays oversized - so each contributes max(current, s_seg_max), not
     * its size today. Projecting only the active segment's growth is what
     * makes an overflow look safe right up until it happens: with one 980KB
     * segment and three empty ones the set looks fine, and only becomes an
     * overflow once the empty ones fill.
     *
     * While that projection exceeds the budget, drop the biggest non-active
     * segment. Only acts when there is a real risk, and says what it dropped.
     */
    size_t sz[CAP_SEGMENTS];
    for (unsigned i = 0; i < CAP_SEGMENTS; i++) {
        sz[i] = seg_size(i);
    }
    const size_t budget = (s_total > RESERVE_BYTES) ? s_total - RESERVE_BYTES
                                                    : 0;

    /*
     * Lowering the ceiling costs future headroom. Deleting a segment costs
     * frames already recorded - which may be the run the user is about to
     * download. So the ceiling gives way first, all the way to its floor.
     */
    if (s_seg_max && projected_set(sz, s_seg_max) > budget) {
        size_t was = s_seg_max;
        while (s_seg_max > SEG_MIN_BYTES &&
               projected_set(sz, s_seg_max) > budget) {
            s_seg_max -= SEG_STEP_BYTES;
        }
        ESP_LOGW(TAG, "per-segment ceiling lowered %u -> %u to fit the log "
                      "already on flash; nothing dropped",
                 (unsigned)was, (unsigned)s_seg_max);
    }

    /*
     * Only when the files themselves overflow the budget - which no ceiling
     * can fix - does recorded data have to go, oldest first.
     */
    while (s_seg_max && projected_set(sz, s_seg_max) > budget) {
        int    biggest = -1;
        size_t biggest_sz = 0;
        for (unsigned i = 0; i < CAP_SEGMENTS; i++) {
            if (i != s_seg && sz[i] > biggest_sz) {
                biggest_sz = sz[i];
                biggest = (int)i;
            }
        }
        if (biggest < 0 || biggest_sz == 0) {
            break;
        }
        remove(k_seg[biggest]);
        sz[biggest] = 0;
        ESP_LOGW(TAG, "seg%d dropped (%u bytes): even at the smallest ceiling "
                      "the log on flash overflows the %u budget",
                 biggest, (unsigned)biggest_sz, (unsigned)budget);
        s_used = seg_total_used();
    }

    s_ready = true;
    ESP_LOGI(TAG, "capture log ready: %u bytes used of %u, seg%u active, "
                  "%u per segment",
             (unsigned)s_used, (unsigned)s_total, (unsigned)s_seg,
             (unsigned)s_seg_max);
}

bool hr_capture_ready(void)
{
    return s_ready;
}


size_t hr_capture_size(void)
{
    return s_used;
}

size_t hr_capture_capacity(void)
{
    return s_total;
}

bool hr_capture_clear(void)
{
    if (!s_ready) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_run_count = 0;
    s_run_body[0] = '\0';
    for (unsigned i = 0; i < CAP_SEGMENTS; i++) {
        remove(k_seg[i]);
    }
    remove(MOUNT "/frames.log");   /* legacy, if migration ever failed */
    s_seg = 0;
    s_seg_used = 0;
    s_used = 0;
    s_full_warned = false;
    seg_save();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "capture log cleared");
    return true;
}

/*
 * Reading the log uses POSIX open/read rather than stdio.
 *
 * fopen(LOGFILE, "r") SUCCEEDED and the very first fread() then returned 0, on
 * a file SPIFFS reported as 270KB - reproducibly, and only after a reboot. The
 * download had been silently returning an empty body ever since. Note the trend
 * file in this same module is read with "rb" and has never had the problem,
 * which points at newlib's buffered text-mode path over SPIFFS rather than at
 * the file or the filesystem.
 *
 * open/read has no buffering layer to get this wrong, so instead of working out
 * which stdio subtlety bites here, the layer is removed. The handle is the file
 * descriptor biased by +1, because a valid fd of 0 is indistinguishable from
 * the NULL this API uses for failure.
 */
/*
 * Flush and unmount the capture filesystem.
 *
 * Called before the deliberate reboot after an OTA. Without it SPIFFS is never
 * cleanly unmounted, and the symptom is not a lost write - it is a file whose
 * metadata and data disagree: stat() reported 26,359 bytes on a log whose very
 * first read() returned nothing. Since the mount is configured with
 * format_if_mount_failed, a bad enough inconsistency then wipes the partition
 * silently, which is how a 270KB capture became 26KB between two reboots.
 *
 * Takes the writer lock so an in-flight line finishes first, and never releases
 * it: nothing should write after this point, and blocking a late writer is
 * better than letting it reopen the filesystem we just closed.
 */
void hr_capture_shutdown(void)
{
    if (!s_ready) {
        return;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000));
    }
    s_ready = false;
    esp_vfs_spiffs_unregister("capture");
    ESP_LOGI(TAG, "capture filesystem unmounted cleanly");
}

/*
 * Reformat the capture partition.
 *
 * SPIFFS can reach a state where it reports plenty of free space and still
 * refuses every write with EIO - reads keep working, so nothing looks broken
 * until you notice the trend graph and the logbook have both quietly stopped
 * recording. This partition has already been corrupted once by an unclean
 * reboot, so a way back is not hypothetical.
 *
 * Destructive: the capture log, the trend and the logbook all go. That is the
 * point - the alternative is a device that silently never records again.
 */
bool hr_capture_format(void)
{
    ESP_LOGW(TAG, "reformatting the capture partition; stored data is lost");
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000));
    }
    s_ready = false;
    esp_vfs_spiffs_unregister("capture");
    esp_err_t err = esp_spiffs_format("capture");
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
        return false;
    }
    /*
     * Re-mount directly rather than via hr_capture_init(), which SPAWNS the
     * writer and mount tasks - calling it twice would leave duplicates running
     * against the same queue.
     */
    hr_capture_mount_now();
    return true;
}

/*
 * A download spans every segment, oldest first, so it reads as one continuous
 * log exactly as it did when there was a single file.
 */
/*
 * Flash wears out and SPIFFS does not repair itself. A segment can develop a
 * spot that returns EIO for good, and a log is exactly the kind of file where
 * the bytes AFTER the damage are the ones worth having. So a read error steps
 * over the bad region rather than ending the download.
 */
#define SKIP_BYTES 4096
#define SKIP_MAX   64            /* at most 256 KB of damage stepped over */

typedef struct {
    int      fd;      /* -1 when no segment is currently open */
    unsigned age;     /* next age-order slot to open, 0 = oldest */
    unsigned cur;     /* segment currently open, for diagnostics */
    size_t   got;     /* bytes read from it so far */
    unsigned skips;   /* damaged regions stepped over in this segment */
} cap_reader_t;

void *hr_capture_open(void)
{
    if (!s_ready) {
        return NULL;
    }
    /* Make any pending run visible before the caller reads the log. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        flush_run();
        xSemaphoreGive(s_lock);
    }

    cap_reader_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->fd = -1;
    r->age = 0;
    return r;
}

int hr_capture_read(void *handle, char *buf, size_t cap)
{
    cap_reader_t *r = (cap_reader_t *)handle;
    if (r == NULL || buf == NULL || cap == 0) {
        return 0;
    }
    for (;;) {
        if (r->fd < 0) {
            if (r->age >= CAP_SEGMENTS) {
                return 0;           /* every segment consumed */
            }
            unsigned i = seg_at(r->age++);
            r->fd = open(k_seg[i], O_RDONLY);
            if (r->fd < 0) {
                /*
                 * Absent is normal before the first wrap. A segment that
                 * stat()s non-empty and still will not open is not - that is
                 * the fault that served an empty download while the info
                 * endpoint reported megabytes.
                 */
                size_t sz = seg_size(i);
                if (sz > 0) {
                    ESP_LOGE(TAG, "capture: seg%u holds %u bytes but will not "
                                  "open (errno %d)", i, (unsigned)sz, errno);
                }
                continue;
            }
            r->cur = i;
            r->got = 0;
            r->skips = 0;
        }
        int n = (int)read(r->fd, buf, cap);
        if (n > 0) {
            r->got += (size_t)n;
            return n;
        }
        if (n < 0) {
            /*
             * Step over the damage to the next page boundary and keep reading.
             * Abandoning the segment here is what cost the end of a real run:
             * one bad spot 238 KB into a 248 KB segment made everything after
             * it - the last hours of the batch - unreachable, while the parts
             * before it downloaded perfectly.
             */
            if (r->skips < SKIP_MAX) {
                off_t here = lseek(r->fd, 0, SEEK_CUR);
                if (here < 0) {
                    here = (off_t)r->got;
                }
                off_t next = (here / SKIP_BYTES + 1) * SKIP_BYTES;
                r->skips++;
                if (lseek(r->fd, next, SEEK_SET) >= 0) {
                    ESP_LOGW(TAG, "capture: seg%u unreadable at %u (errno %d);"
                                  " skipping to %u",
                             r->cur, (unsigned)here, errno, (unsigned)next);
                    r->got = (size_t)next;
                    continue;
                }
            }
            ESP_LOGE(TAG, "capture: seg%u read failed after %u bytes "
                          "(errno %d)", r->cur, (unsigned)r->got, errno);
        } else if (r->got == 0 && seg_size(r->cur) > 0) {
            ESP_LOGE(TAG, "capture: seg%u stats %u bytes but the first read "
                          "returned 0", r->cur, (unsigned)seg_size(r->cur));
        }
        close(r->fd);               /* end of this segment: move to the next */
        r->fd = -1;
    }
}

void hr_capture_close(void *handle)
{
    cap_reader_t *r = (cap_reader_t *)handle;
    if (r != NULL) {
        if (r->fd >= 0) {
            close(r->fd);
        }
        free(r);
    }
}

unsigned hr_capture_seg_count(void)
{
    return CAP_SEGMENTS;
}

unsigned hr_capture_seg_active(void)
{
    return s_seg;
}

size_t hr_capture_seg_bytes(unsigned i)
{
    return (i < CAP_SEGMENTS) ? seg_size(i) : 0;
}

unsigned long hr_capture_rotations(void)
{
    return s_rotations;
}

unsigned long hr_capture_suppressed(void)
{
    return s_suppressed;
}
