#include "hr_usb.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

#include <string.h>

static const char *TAG = "hr_usb";

#define CDC_ITF TINYUSB_CDC_ACM_0
#define RX_CHUNK 256

static hr_session_t *s_session;
static volatile bool s_host_present;

/* USB-level diagnostic counters - see hr_usb.h for why these exist. */
static volatile bool s_mounted;
static volatile bool s_suspended;
static volatile unsigned s_mount_events;
static volatile unsigned long s_rx_bytes;

static unsigned long now_ms(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000);
}

static void on_rx(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buf[RX_CHUNK];
    size_t got = 0;

    /* Drain everything TinyUSB has buffered for us. */
    while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &got) == ESP_OK &&
           got > 0) {
        s_rx_bytes += got;
        hr_session_rx(s_session, buf, got, now_ms());
        got = 0;
    }
}

/*
 * TinyUSB bus-event hooks (weak symbols in the TinyUSB core; the MSC driver
 * would also define them but it is not compiled here). These are the only
 * signals that tell us whether the dryer enumerated us at all, which the CDC
 * line-state callback does NOT - it fires from a class request, so its absence
 * and a total enumeration failure look the same in the log.
 */
void tud_mount_cb(void)
{
    s_mounted = true;
    s_suspended = false;
    s_mount_events++;
    ESP_LOGI(TAG, "USB mounted: host enumerated us (mount #%u)",
             (unsigned)s_mount_events);
}

void tud_umount_cb(void)
{
    s_mounted = false;
    ESP_LOGW(TAG, "USB unmounted: host dropped us");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    s_suspended = true;
    ESP_LOGW(TAG, "USB suspended (remote wakeup %s)",
             remote_wakeup_en ? "enabled" : "disabled");
}

void tud_resume_cb(void)
{
    s_suspended = false;
    ESP_LOGI(TAG, "USB resumed");
}

/*
 * SET_LINE_CODING from the host. Logged purely as a host fingerprint.
 *
 * The dryer's own firmware issues CDC line-coding requests (its strings include
 * "USB_CDC_GET_LINE_CODING %d %ld"), whereas a PC sends these only when an
 * application actually opens the port. Seeing this therefore identifies WHICH
 * host is on the wire - the one ambiguity that "mounted=1, rx_bytes=0" cannot
 * resolve by itself.
 *
 * Registered via tinyusb_config_cdcacm_t; esp_tinyusb owns the real
 * tud_cdc_line_coding_cb symbol and dispatches to us, so do NOT define that
 * weak override here - it collides at link time.
 */
static void on_line_coding(int itf, cdcacm_event_t *event)
{
    (void)itf;
    const cdc_line_coding_t *c = event->line_coding_changed_data.p_line_coding;
    if (c == NULL) {
        return;
    }
    ESP_LOGI(TAG, "line coding: %lu baud, %u data bits, %u stop, parity %u",
             (unsigned long)c->bit_rate, (unsigned)c->data_bits,
             (unsigned)c->stop_bits, (unsigned)c->parity);
}

/*
 * Force a USB detach/re-attach without rebooting.
 *
 * Why this exists: the dryer's CDC thread gives up if nothing answers its probe
 * ("CDC timed out, not connected, resumed Main thread") and nothing re-runs
 * that probe on its own - its only recovery path is gated on an already-pending
 * CDC message. But an adapter reboot HAS historically been enough to bring the
 * link back, and from the dryer's point of view a reboot is just a detach
 * followed by an attach ~1.1s later. This reproduces exactly that, so a lost
 * link can be retried mid-batch instead of waiting to power-cycle the machine.
 *
 * Runs on its own short-lived task so the HTTP response goes out first and the
 * single httpd worker is not blocked for the gap.
 */
static void reattach_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "forcing USB detach");
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(1500));
    tud_connect();
    ESP_LOGW(TAG, "USB re-attached; waiting for the dryer to re-enumerate "
                  "(watch the mount count)");
    vTaskDelete(NULL);
}

bool hr_usb_bus_reattach(void)
{
    return xTaskCreate(reattach_task, "usb_reattach", 3072, NULL, 5, NULL) ==
           pdPASS;
}

bool hr_usb_mounted(void) { return s_mounted; }
bool hr_usb_suspended(void) { return s_suspended; }
unsigned long hr_usb_rx_bytes(void) { return s_rx_bytes; }
unsigned hr_usb_mount_events(void) { return s_mount_events; }

static void on_line_state(int itf, cdcacm_event_t *event)
{
    (void)itf;
    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    s_host_present = dtr;
    ESP_LOGI(TAG, "line state: dtr=%d rts=%d", (int)dtr, (int)rts);
}

void hr_usb_tx(const char *data, size_t len, void *user)
{
    (void)user;
    size_t queued = tinyusb_cdcacm_write_queue(CDC_ITF, (const uint8_t *)data,
                                               len);
    if (queued != len) {
        ESP_LOGW(TAG, "short write: queued %u of %u", (unsigned)queued,
                 (unsigned)len);
    }
    /* Flush promptly - the dryer's parser is CR-driven and latency-sensitive. */
    esp_err_t err = tinyusb_cdcacm_write_flush(CDC_ITF, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(err));
    }
}

bool hr_usb_host_present(void)
{
    return s_host_present;
}

void hr_usb_init(hr_session_t *session)
{
    s_session = session;

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL, /* default descriptor; see README re VID/PID */
        .string_descriptor = NULL,
        .external_phy = false,
        .configuration_descriptor = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    const tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = CDC_ITF,
        .rx_unread_buf_sz = 1024,
        .callback_rx = &on_rx,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = &on_line_state,
        .callback_line_coding_changed = &on_line_coding,
    };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));

    /* Heap at USB bring-up. hr_capture_init() formats/mounts a 3MB SPIFFS
     * immediately before this, so if that ever starves TinyUSB's DMA buffers
     * the evidence needs to be in the log rather than inferred. */
    ESP_LOGI(TAG, "USB CDC-ACM device ready (free heap %u, DMA-capable %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
}
