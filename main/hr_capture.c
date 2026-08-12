#include "hr_capture.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "hr_capture";
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

void hr_capture_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }

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

void hr_capture_append(uint32_t t_ms, const char *body)
{
    if (!s_ready || body == NULL || body[0] == '\0') {
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
        int n = fprintf(f, "%" PRIu32 "\t%s\n", t_ms, body);
        fclose(f);
        if (n > 0) {
            s_used += (size_t)n;
        }
    }
    xSemaphoreGive(s_lock);
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
