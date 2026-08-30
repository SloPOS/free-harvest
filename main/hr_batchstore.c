#include "hr_batchstore.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "hr_batch";

#define SEG_A      "/capture/batches.0.csv"
#define SEG_B      "/capture/batches.1.csv"
#define NVS_NS     "hrbatch"

/*
 * 128 KB per segment, two segments.
 *
 * The capture log shares this 3 MB partition and will happily grow to fill all
 * of it, so the logbook needs a reserve or it silently stops recording after a
 * few months - a failure nobody would notice until they went looking for
 * history that was never there.
 *
 * Two rotating segments rather than trimming old lines out of one file:
 * trimming means rewriting, and a rewrite torn by a power cut loses everything
 * rather than one record. That is the same reasoning that makes the append
 * itself append-only. The cost is that a rollover drops up to half the
 * history, which is the right trade against losing all of it.
 */
#define SEG_MAX    (128 * 1024)

static bool     s_ready;
static uint8_t  s_active;          /* 0 = SEG_A, 1 = SEG_B */

/* Clock: an epoch anchor plus the uptime at which it was taken. */
static uint32_t s_epoch_base;
static uint64_t s_epoch_set_us;

static const char *seg_path(uint8_t which)
{
    return which ? SEG_B : SEG_A;
}

static size_t file_size(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) ? (size_t)st.st_size : 0;
}

static void load_active(void)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(nh, "seg", &v) == ESP_OK) {
            s_active = v ? 1 : 0;
        }
        nvs_close(nh);
    }
}

static void save_active(void)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u8(nh, "seg", s_active);
        nvs_commit(nh);
        nvs_close(nh);
    }
}

/*
 * Must not be called until the capture partition is mounted.
 *
 * That mount is deliberately deferred ~8 seconds off the boot path, to keep
 * flash work away from USB enumeration. Calling this at startup instead - as
 * the first version did - leaves the store permanently unready, silently, and
 * the logbook never records anything. main.c now calls it once
 * hr_capture_ready() turns true.
 */
void hr_batchstore_init(void)
{
    load_active();
    /* A write to the active segment proves the filesystem is up. Doing it here
     * rather than trusting a flag means a failed mount shows as an unready
     * store instead of as append failures later. */
    FILE *f = fopen(seg_path(s_active), "a");
    if (f == NULL) {
        ESP_LOGW(TAG, "logbook unavailable: %s (%s)", seg_path(s_active),
                 strerror(errno));
        s_ready = false;
        return;
    }
    fclose(f);
    s_ready = true;
    ESP_LOGI(TAG, "logbook ready: seg%u active, %u + %u bytes stored",
             (unsigned)s_active, (unsigned)file_size(SEG_A),
             (unsigned)file_size(SEG_B));
}

bool hr_batchstore_ready(void) { return s_ready; }

size_t hr_batchstore_bytes(void)
{
    return s_ready ? file_size(SEG_A) + file_size(SEG_B) : 0;
}

bool hr_batchstore_append(const hr_batch_t *b)
{
    if (!s_ready || b == NULL) {
        return false;
    }
    char line[HR_BATCH_LINE_MAX];
    size_t n = hr_batch_encode(b, line, sizeof(line));
    if (n == 0) {
        ESP_LOGW(TAG, "record would not encode; not written");
        return false;
    }

    /* Rotate BEFORE writing, so a record is never split across segments. */
    if (file_size(seg_path(s_active)) + n + 1 > SEG_MAX) {
        uint8_t next = s_active ? 0 : 1;
        remove(seg_path(next));
        s_active = next;
        save_active();
        ESP_LOGW(TAG, "logbook rotated to seg%u; the older half is gone",
                 (unsigned)s_active);
    }

    FILE *f = fopen(seg_path(s_active), "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "append failed: %s", strerror(errno));
        return false;
    }
    int w = fprintf(f, "%s\n", line);
    fclose(f);
    if (w <= 0) {
        return false;
    }
    ESP_LOGI(TAG, "batch recorded: %s, %us, %s", b->name,
             (unsigned)b->duration_s, hr_outcome_str(b->outcome));
    return true;
}

/*
 * Reader across both segments, oldest first.
 *
 * The inactive segment is the older one - it was filled before the rotation
 * that made the other active - so it is read first and may not exist at all on
 * a store that has never rotated.
 */
typedef struct {
    int     fd;
    uint8_t stage;     /* 0 = older segment, 1 = active, 2 = done */
} reader_t;

static int open_stage(reader_t *r)
{
    while (r->stage < 2) {
        const char *p = (r->stage == 0) ? seg_path(s_active ? 0 : 1)
                                        : seg_path(s_active);
        r->fd = open(p, O_RDONLY);
        if (r->fd >= 0) {
            return 0;
        }
        /*
         * Absent is normal - the older segment does not exist until the first
         * rotation. A segment that stat()s non-empty and still will not open
         * is damage, and silently returning an empty logbook for it reads
         * exactly like "no batches have ever run".
         */
        if (file_size(p) > 0) {
            ESP_LOGE(TAG, "logbook %s holds %u bytes but will not open "
                          "(errno %d)", p, (unsigned)file_size(p), errno);
        }
        r->stage++;
    }
    return -1;
}

void *hr_batchstore_open(void)
{
    if (!s_ready) {
        return NULL;
    }
    reader_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->fd = -1;
    r->stage = 0;
    if (open_stage(r) < 0) {
        free(r);
        return NULL;
    }
    return r;
}

int hr_batchstore_read(void *handle, char *buf, size_t cap)
{
    reader_t *r = (reader_t *)handle;
    if (r == NULL || buf == NULL || cap == 0) {
        return 0;
    }
    for (;;) {
        if (r->fd >= 0) {
            int n = (int)read(r->fd, buf, cap);
            if (n > 0) {
                return n;
            }
            close(r->fd);
            r->fd = -1;
        }
        r->stage++;
        if (r->stage >= 2 || open_stage(r) < 0) {
            return 0;
        }
    }
}

void hr_batchstore_close(void *handle)
{
    reader_t *r = (reader_t *)handle;
    if (r != NULL) {
        if (r->fd >= 0) {
            close(r->fd);
        }
        free(r);
    }
}

bool hr_batchstore_clear(void)
{
    if (!s_ready) {
        return false;
    }
    remove(SEG_A);
    remove(SEG_B);
    s_active = 0;
    save_active();
    ESP_LOGW(TAG, "logbook cleared");
    return true;
}

/* ---- in-progress batch --------------------------------------------------- */

bool hr_batchstore_save_open(const hr_batch_t *b)
{
    nvs_handle_t nh;
    if (b == NULL || nvs_open(NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_blob(nh, "open", b, sizeof(*b)) == ESP_OK &&
              nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    return ok;
}

bool hr_batchstore_load_open(hr_batch_t *out)
{
    nvs_handle_t nh;
    if (out == NULL || nvs_open(NVS_NS, NVS_READONLY, &nh) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(*out);
    bool ok = nvs_get_blob(nh, "open", out, &len) == ESP_OK &&
              len == sizeof(*out);
    nvs_close(nh);
    return ok && out->outcome == HR_OUTCOME_RUNNING;
}

void hr_batchstore_clear_open(void)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
        nvs_erase_key(nh, "open");
        nvs_commit(nh);
        nvs_close(nh);
    }
}

/* ---- clock --------------------------------------------------------------- */

bool hr_time_set(uint32_t epoch)
{
    /*
     * Sanity floor: anything before 2020 is a browser with a broken clock or a
     * malformed request, and recording it would produce dates worse than no
     * dates. 1577836800 is 2020-01-01.
     */
    if (epoch < 1577836800u) {
        ESP_LOGW(TAG, "ignoring implausible clock value %lu",
                 (unsigned long)epoch);
        return false;
    }
    bool first = (s_epoch_base == 0);
    s_epoch_base = epoch;
    s_epoch_set_us = (uint64_t)esp_timer_get_time();
    if (first) {
        ESP_LOGI(TAG, "clock set from a browser: %lu", (unsigned long)epoch);
    }
    return true;
}

uint32_t hr_time_now(void)
{
    if (s_epoch_base == 0) {
        return 0;                    /* unknown, and says so downstream */
    }
    uint64_t elapsed_us = (uint64_t)esp_timer_get_time() - s_epoch_set_us;
    return s_epoch_base + (uint32_t)(elapsed_us / 1000000ULL);
}

bool hr_time_known(void) { return s_epoch_base != 0; }
