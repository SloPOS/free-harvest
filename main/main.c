/*
 * HarvestRight freeze dryer - replacement WiFi adapter firmware (ESP32-S3).
 *
 * Role: present a USB CDC-ACM device to the dryer (which is the USB host),
 * speak its CR-terminated ASCII frame protocol, record every frame, and serve
 * a live decoding web UI over the user's home WiFi.
 *
 * Status: the portable protocol + history core is unit-tested on host (see
 * test/). The exact contents the dryer expects inside a GOTIT ack are NOT yet
 * confirmed - see README and decoded/PROTOCOL_NOTES.md.
 */
#include "hr_capture.h"
#include "hr_http.h"
#include "hr_history.h"
#include "hr_log.h"
#include "hr_mqtt.h"
#include "hr_session.h"
#include "hr_telemetry.h"
#include "hr_trend.h"
#include "hr_usb.h"
#include "hr_wifi.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "hr_main";

static hr_session_t s_session;
static hr_history_t s_history;
static SemaphoreHandle_t s_hist_lock;
/* Tracks whether the batch-elapsed counter is actually advancing, so an idle
 * dryer isn't reported as "running" using last batch's leftover elapsed. */
static hr_phase_tracker_t s_tracker;
/*
 * 30s temperature/pressure series for the graph. Guarded by s_hist_lock, the
 * same mutex as history, because it is written from the USB RX task and read by
 * the HTTP task.
 *
 * RAM only for now: a reboot loses the current run's graph. Persisting it needs
 * a flash-write queue so no flash I/O lands on the USB RX path - that is the
 * next commit, not this one.
 */
static hr_trend_t s_trend;
/* Last batch-elapsed seen, to notice a new batch and start a fresh series. */
static long s_last_batch_elapsed = -1;

static unsigned long now_ms(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000);
}

/*
 * How long a freshly OTA'd image gets to prove it is reachable before the
 * rollback fires. Generous: a slow AP, a DHCP retry and one failed join
 * attempt all have to fit inside it.
 */
#define HR_OTA_CONFIRM_TIMEOUT_MS 120000UL

/*
 * True when this boot is running a just-installed image that the bootloader
 * has NOT yet been told to keep. See sdkconfig.defaults for the mechanism.
 */
static bool ota_awaiting_confirm(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (run == NULL || esp_ota_get_state_partition(run, &state) != ESP_OK) {
        return false;
    }
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

/*
 * "Healthy" deliberately means REACHABLE, not "fully working".
 *
 * If the web server is up and the user can get to it - either on their LAN or
 * through the setup AP - then a bad build is recoverable by uploading another
 * one, so we keep it. An image that cannot get onto the network at all is NOT
 * recoverable that way, and rolling back is strictly better than leaving the
 * adapter needing a USB cable. Note that failing to decode dryer frames is
 * explicitly NOT a rollback trigger: the dryer may simply be unplugged.
 */
static bool adapter_reachable(void)
{
    hr_wifi_status_t w = hr_wifi_status();
    return w == HR_WIFI_CONNECTED || w == HR_WIFI_AP_SETUP;
}

/*
 * Session observer: fires for every inbound frame (from the USB RX task).
 * Record it in history for the web UI, and optionally echo to the console.
 * The same mutex guards history reads in hr_http.c.
 */
static void on_inbound(const hr_frame_t *f, void *user)
{
    (void)user;

    xSemaphoreTake(s_hist_lock, portMAX_DELAY);
    uint32_t seq = hr_history_add(&s_history, f, (uint32_t)now_ms());
    xSemaphoreGive(s_hist_lock);

    hr_http_notify(seq);

    /* Persist every frame so a full cycle can be recovered later - the RAM
     * ring only holds a few minutes. */
    {
        char line[HR_MAX_FRAME];
        if (hr_frame_tostring(f, line, sizeof(line)) > 0) {
            hr_capture_append((uint32_t)now_ms(), line);
        }
    }

    /* Decode STAT frames once, then share with both the web UI and MQTT. */
    hr_telemetry_t tel;
    if (hr_telemetry_from_stat(f, &tel)) {
        hr_phase_tracker_update(&s_tracker, &tel, (unsigned long)now_ms());
        hr_http_set_telemetry(&tel);
        hr_http_set_tracker(&s_tracker);
        hr_mqtt_publish_telemetry(&tel);

        /*
         * Feed the graph series. The dryer's batch-elapsed counter only ever
         * counts up within a run, so a DECREASE means a new batch started and
         * the old curve must not be fitted across into the new one.
         */
        xSemaphoreTake(s_hist_lock, portMAX_DELAY);
        if (s_last_batch_elapsed >= 0 &&
            tel.batch_elapsed_s < s_last_batch_elapsed) {
            hr_trend_reset(&s_trend);
        }
        s_last_batch_elapsed = tel.batch_elapsed_s;
        hr_trend_add(&s_trend, now_ms(), (int)tel.temperature_f,
                     (uint32_t)tel.pressure_microns, tel.pressure_valid);
        xSemaphoreGive(s_hist_lock);
    }

#if CONFIG_HR_HTTP_LOG_TO_UART
    char line[HR_MAX_FRAME];
    if (hr_frame_tostring(f, line, sizeof(line)) > 0) {
        ESP_LOGI(TAG, "RX <- %s", line);
    }
#endif
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Capture logs into the in-app ring buffer (viewable at /api/log) as early
     * as possible so boot and MQTT connection errors are visible in the UI. */
    hr_log_init();

    s_hist_lock = xSemaphoreCreateMutex();
    hr_history_init(&s_history);
    hr_phase_tracker_init(&s_tracker);
    hr_trend_init(&s_trend);

    hr_session_init(&s_session, hr_usb_tx, NULL);
    hr_session_set_observer(&s_session, on_inbound, NULL);
    hr_session_set_ack_payload(&s_session, CONFIG_HR_ACK_PAYLOAD);

    hr_usb_init(&s_session);

    /*
     * MUST come after hr_usb_init(). Mounting (and on first boot formatting)
     * the 3MB SPIFFS consumes DMA-capable heap, and TinyUSB allocates its
     * endpoint buffers from the same pool. With capture first, the device still
     * enumerates - EP0 control transfers need almost nothing - but the bulk
     * endpoints never deliver a byte, which reads as "the dryer stopped talking"
     * rather than as an out-of-memory condition anywhere in the log.
     */
    hr_capture_init();

    hr_wifi_start();

    /* Share one mutex: main.c writes history (on_inbound), hr_http.c reads it.
     * Must be set before hr_http_start(). */
    hr_http_use_lock(s_hist_lock);
    hr_http_set_trend(&s_trend);
    hr_http_start(&s_session, &s_history);

    /* MQTT connects only if a broker is configured (via the web setup page);
     * otherwise it stays idle. Safe to call regardless of WiFi state. */
    hr_mqtt_start(&s_session);

    ESP_LOGI(TAG, "adapter running; waiting for dryer traffic");

    bool ota_pending = ota_awaiting_confirm();
    if (ota_pending) {
        ESP_LOGW(TAG, "running a newly installed image on trial; it will be "
                      "kept once the adapter is reachable, or rolled back in "
                      "%lus", HR_OTA_CONFIRM_TIMEOUT_MS / 1000);
    }

    hr_link_state_t last_link = HR_LINK_DOWN;
    unsigned long last_beat = 0;
    unsigned long boot_ms = now_ms();
    for (;;) {
        unsigned long t = now_ms();

        /* Decide the fate of a trial image before anything else can crash. */
        if (ota_pending) {
            if (adapter_reachable()) {
                esp_err_t cerr = esp_ota_mark_app_valid_cancel_rollback();
                ota_pending = false;
                ESP_LOGI(TAG, "update confirmed and kept (%s)",
                         esp_err_to_name(cerr));
            } else if (t - boot_ms >= HR_OTA_CONFIRM_TIMEOUT_MS) {
                ESP_LOGE(TAG, "update never became reachable; rolling back to "
                              "the previous firmware");
                /* Does not return on success. */
                esp_ota_mark_app_invalid_rollback_and_reboot();
                ota_pending = false; /* rollback unavailable; keep running */
            }
        }
        hr_session_tick(&s_session, t);
        /* Let a stale run expire even if frames stop arriving entirely. */
        hr_phase_tracker_tick(&s_tracker, t);
        /* Close elapsed graph buckets even while frames are absent, so a gap
         * shows as a gap instead of compressing the time axis. */
        xSemaphoreTake(s_hist_lock, portMAX_DELAY);
        hr_trend_tick(&s_trend, t);
        xSemaphoreGive(s_hist_lock);
        hr_http_set_tracker(&s_tracker);
        if (s_session.link != last_link) {
            last_link = s_session.link;
            ESP_LOGI(TAG, "link %s", last_link == HR_LINK_UP ? "UP" : "DOWN");
        }

        /*
         * Periodic USB/link heartbeat.
         *
         * Without this, the loop is silent unless the link state flips, so a
         * console log captured for a few seconds after boot looks identical to
         * a broken adapter - the dryer only emits idle STAT frames every ~15s,
         * so "no RX yet" is the expected state for the whole first interval.
         * Printing every 10s means any log long enough to matter is
         * self-diagnosing.
         */
        if (t - last_beat >= 10000UL) {
            last_beat = t;
            ESP_LOGI(TAG,
                     "usb mounted=%d suspended=%d mounts=%u rx_bytes=%lu | "
                     "frames_in=%lu link=%s | tusb ready=%d cdc_conn=%d "
                     "avail=%u",
                     (int)hr_usb_mounted(), (int)hr_usb_suspended(),
                     hr_usb_mount_events(), hr_usb_rx_bytes(),
                     s_session.frames_in,
                     s_session.link == HR_LINK_UP ? "UP" : "DOWN",
                     (int)hr_usb_tusb_ready(), (int)hr_usb_cdc_connected(),
                     hr_usb_cdc_available());
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
