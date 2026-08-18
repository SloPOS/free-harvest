#include "hr_capture.h"

#include "esp_timer.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
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
#define LOGFILE MOUNT "/frames.log"
/*
 * Stop appending a little short of full so the filesystem always has room to
 * be read and cleared. Without this a full SPIFFS can be awkward to recover.
 */
#define RESERVE_BYTES (64 * 1024)

static bool s_ready;
static SemaphoreHandle_t s_lock;
static size_t s_total;      /* partition capacity */
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

/* Runs on the worker task: the only place that touches the filesystem. */
static void write_line_now(const write_msg_t *m)
{
    if (!s_ready) {
        return;
    }
    if (s_total && s_used + RESERVE_BYTES >= s_total) {
        if (!s_full_warned) {
            s_full_warned = true;
            ESP_LOGW(TAG, "capture log full (%u bytes) - download and clear it "
                          "to keep recording", (unsigned)s_used);
        }
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    FILE *f = fopen(LOGFILE, "a");
    if (f != NULL) {
        int n = fprintf(f, "%" PRIu32 "\t%s\n", m->t_ms, m->body);
        fclose(f);
        if (n > 0) {
            s_used += (size_t)n;
        }
    }
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
        /* The frame writer, the trend save, stat() and an HTTP download can
           all want a descriptor at once; 2 was too few and starved writes. */
        .max_files = 6,
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
    struct stat st;
    s_used = (stat(LOGFILE, &st) == 0) ? (size_t)st.st_size : 0;
    s_ready = true;
    ESP_LOGI(TAG, "capture log ready: %u bytes used of %u", (unsigned)s_used,
             (unsigned)s_total);
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
    remove(LOGFILE);
    s_used = 0;
    s_full_warned = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "capture log cleared");
    return true;
}

void *hr_capture_open(void)
{
    if (!s_ready) {
        return NULL;
    }
    return (void *)fopen(LOGFILE, "r");
}

int hr_capture_read(void *handle, char *buf, size_t cap)
{
    if (handle == NULL || buf == NULL || cap == 0) {
        return 0;
    }
    return (int)fread(buf, 1, cap, (FILE *)handle);
}

void hr_capture_close(void *handle)
{
    if (handle != NULL) {
        fclose((FILE *)handle);
    }
}
