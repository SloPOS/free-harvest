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
#include "hr_http.h"
#include "hr_history.h"
#include "hr_log.h"
#include "hr_mqtt.h"
#include "hr_session.h"
#include "hr_telemetry.h"
#include "hr_usb.h"
#include "hr_wifi.h"

#include "esp_log.h"
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

static unsigned long now_ms(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000);
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

    /* Decode STAT frames once, then share with both the web UI and MQTT. */
    hr_telemetry_t tel;
    if (hr_telemetry_from_stat(f, &tel)) {
        hr_phase_tracker_update(&s_tracker, &tel, (unsigned long)now_ms());
        hr_http_set_telemetry(&tel);
        hr_http_set_tracker(&s_tracker);
        hr_mqtt_publish_telemetry(&tel);
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

    hr_session_init(&s_session, hr_usb_tx, NULL);
    hr_session_set_observer(&s_session, on_inbound, NULL);
    hr_session_set_ack_payload(&s_session, CONFIG_HR_ACK_PAYLOAD);

    hr_usb_init(&s_session);
    hr_wifi_start();

    /* Share one mutex: main.c writes history (on_inbound), hr_http.c reads it.
     * Must be set before hr_http_start(). */
    hr_http_use_lock(s_hist_lock);
    hr_http_start(&s_session, &s_history);

    /* MQTT connects only if a broker is configured (via the web setup page);
     * otherwise it stays idle. Safe to call regardless of WiFi state. */
    hr_mqtt_start(&s_session);

    ESP_LOGI(TAG, "adapter running; waiting for dryer traffic");

    hr_link_state_t last_link = HR_LINK_DOWN;
    for (;;) {
        hr_session_tick(&s_session, now_ms());
        /* Let a stale run expire even if frames stop arriving entirely. */
        hr_phase_tracker_tick(&s_tracker, (unsigned long)now_ms());
        hr_http_set_tracker(&s_tracker);
        if (s_session.link != last_link) {
            last_link = s_session.link;
            ESP_LOGI(TAG, "link %s", last_link == HR_LINK_UP ? "UP" : "DOWN");
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
