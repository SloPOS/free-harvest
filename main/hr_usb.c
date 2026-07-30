#include "hr_usb.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

#include <string.h>

static const char *TAG = "hr_usb";

#define CDC_ITF TINYUSB_CDC_ACM_0
#define RX_CHUNK 256

static hr_session_t *s_session;
static volatile bool s_host_present;

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
        hr_session_rx(s_session, buf, got, now_ms());
        got = 0;
    }
}

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
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));

    ESP_LOGI(TAG, "USB CDC-ACM device ready");
}
