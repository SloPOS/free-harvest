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
#include <sys/stat.h>

static const char *TAG = "hr_capture";
/* Longest frame line we will queue. Matches HR_MAX_FRAME plus the timestamp
   prefix; anything longer is truncated rather than dropped. */
#define HR_CAPTURE_LINE_MAX 544
#define MOUNT "/capture"
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

typedef struct {
    uint32_t t_ms;
    char body[HR_CAPTURE_LINE_MAX];
} write_msg_t;

static QueueHandle_t s_q;
static volatile unsigned long s_dropped;

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
        .max_files = 2,
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
