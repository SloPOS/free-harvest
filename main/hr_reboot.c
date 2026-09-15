#include "hr_reboot.h"
#include "hr_capture.h"
#include "hr_wifi.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdbool.h>

static const char *TAG = "hr_reboot";

/*
 * Stack for the restart sequence.
 *
 * The OTA path used to do this on a 3072-byte task. Everything below runs on
 * whichever task calls it: the SPIFFS unmount (VFS + newlib depth, the same
 * reason the capture writer has 4096), log formatting, and then
 * esp_restart() itself, which runs every registered shutdown handler in the
 * caller's context - esp_wifi_stop() among them - before it touches the
 * reset registers. None of that has any business being tight on stack.
 */
#define REBOOT_TASK_STACK 6144

/* Exactly one request ever proceeds, so its parameters can live here. */
static atomic_bool s_pending;
static const char *s_why;
static uint32_t    s_delay_ms;

/*
 * The shutdown order, and why it is this order:
 *
 *  1. Wi-Fi is told a restart is coming. esp_restart() calls esp_wifi_stop()
 *     via a shutdown handler, and a connected station being stopped raises
 *     WIFI_EVENT_STA_DISCONNECTED - which hr_wifi's handler would otherwise
 *     answer by re-enabling the setup AP and reconnecting, mid-teardown.
 *  2. The capture filesystem is quiesced and unmounted - bounded wait, and
 *     left mounted if it is still busy; see hr_capture_shutdown(). A capture
 *     that never mounted is a no-op there.
 *  3. esp_restart().
 */
static void reboot_now(void)
{
    if (s_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(s_delay_ms));
    }
    ESP_LOGW(TAG, "restarting: %s", s_why ? s_why : "requested");
    hr_wifi_prepare_restart();
    hr_capture_shutdown();
    esp_restart();
}

static void reboot_task(void *arg)
{
    (void)arg;
    reboot_now();
    vTaskDelete(NULL); /* not reached: esp_restart() does not return */
}

void hr_reboot_request(const char *why, uint32_t delay_ms)
{
    if (atomic_exchange(&s_pending, true)) {
        ESP_LOGW(TAG, "restart already pending; ignoring \"%s\"",
                 why ? why : "");
        return;
    }
    /* `why` is only ever read from the log line above; callers pass string
     * literals, so no copy is needed. */
    s_why = why;
    s_delay_ms = delay_ms;

    /*
     * The delay runs on the helper task rather than here, so the caller - an
     * HTTP handler that still has a reply to finish - returns immediately.
     */
    if (xTaskCreate(reboot_task, "hr_reboot", REBOOT_TASK_STACK, NULL, 5,
                    NULL) != pdPASS) {
        /* Out of memory for a task: restart anyway, from here, so a failed
         * allocation can never leave the adapter running the wrong image. */
        ESP_LOGE(TAG, "could not start the restart task; restarting inline");
        reboot_now();
    }
}
