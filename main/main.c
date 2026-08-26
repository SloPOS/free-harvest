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
#include "hr_batchstore.h"
#include "hr_http.h"
#include "hr_history.h"
#include "hr_log.h"
#include "hr_mqtt.h"
#include "hr_session.h"
#include "hr_telemetry.h"
#include "hr_trend.h"
#include "hr_usb.h"
#include "hr_wifi.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
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
 * Persisted to flash via the capture worker, so a power cut or a reflash no
 * longer loses the run - see the resume block in the main loop.
 */
static hr_trend_t s_trend;
/* Last batch-elapsed seen, to notice a new batch and start a fresh series. */
static long s_last_batch_elapsed = -1;

/*
 * Batch logbook.
 *
 * The tracker is fed from the USB RX callback because that is where telemetry
 * arrives, but it only ever computes - no flash, no NVS. A finished record is
 * parked here and written by the main loop instead, for the same reason
 * hr_capture_append() queues rather than writing: this callback runs on the
 * TinyUSB task, and flash work on that stack is what panicked the chip once
 * already.
 */
static hr_batch_tracker_t s_batch;
static hr_batch_t         s_batch_done;
static volatile bool      s_batch_done_pending;
static bool               s_batch_boot_checked;
static uint32_t           s_batch_saved_ms;
static bool               s_batch_store_inited;

/*
 * Introducing ourselves once per link, and a STATE heartbeat after that.
 *
 * The handshake is PACED - one frame per main-loop pass, 250ms apart - rather
 * than emitted as a burst. The captured genuine adapter waits for the dryer's
 * UID before sending its last frame, and a 6.0.644170 machine answered only
 * the second frame of our 15ms burst, ignoring the two behind it.
 *
 * It also restarts on a USB RE-ENUMERATION, not only when the protocol link
 * drops. Those are different events: after a detach/attach the dryer has a
 * fresh CDC session, but REQINFO keeps arriving throughout, so s_session.link
 * never goes down and the old code never re-introduced itself. The comment
 * here used to claim it did.
 */
static uint8_t  s_hello_step;      /* frames sent so far, 0..HR_HELLO_STEPS */
static uint32_t s_hello_ms;        /* when the last one went out */
static unsigned s_hello_mounts;    /* USB mount count the handshake belongs to */
static uint32_t s_heartbeat_ms;
/* Graph points already written to flash, so the loop only persists new ones. */
static size_t s_trend_persisted;
/* The resume decision runs once, after the capture log mounts and the dryer
   has told us where its batch clock stands. */
static bool s_resume_done;
/* Backoff clock for the series write, so a failure cannot spin. */
static unsigned long s_trend_last_try;

/*
 * The identifier we present to the dryer in WIFIINFO field 4.
 *
 * Every genuine capture carries "HR_" followed by the adapter's MAC with no
 * separators - HR_aabbccddeeff - and the same string appears a second time in
 * esp/version.txt on the stock adapter's mass-storage volume, which the dryer
 * also reads. So a real adapter presents that identifier to the machine twice,
 * in that format.
 *
 * We were sending CONFIG_HR_AP_SSID, "HR-Adapter-Setup", which matches neither
 * the format nor anything the dryer has seen before. Whether any firmware
 * actually checks it is UNKNOWN - this is a divergence from ground truth being
 * closed, not a demonstrated fix.
 *
 * This changes only the WIFIINFO field. The setup hotspot keeps its
 * CONFIG_HR_AP_SSID name, because that one is for humans to find in a phone's
 * Wi-Fi list and "HR_3c8427e1b4f0" is not.
 */
static const char *adapter_id(void)
{
    static char id[20];
    if (id[0] == '\0') {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(id, sizeof(id), "HR_%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return id;
}

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
            s_trend_persisted = 0;
            hr_capture_trend_reset();
        }
        s_last_batch_elapsed = tel.batch_elapsed_s;
        hr_trend_add(&s_trend, now_ms(), (int)tel.temperature_f,
                     (uint32_t)tel.pressure_microns, tel.pressure_valid);
        xSemaphoreGive(s_hist_lock);

        /*
         * Watch for batch boundaries. Pure computation - a finished record is
         * handed to the main loop to write, never written from here.
         */
        hr_batch_t finished;
        if (hr_batch_observe(&s_batch, (int)tel.type,
                             (int32_t)tel.batch_elapsed_s,
                             (int32_t)tel.temperature_f,
                             tel.pressure_valid
                                 ? (int32_t)tel.pressure_microns : 0,
                             tel.mode, hr_time_now(),
                             &finished) == HR_BATCH_FINISHED) {
            if (!s_batch_done_pending) {
                finished.extra_dry_s = hr_http_extra_dry_s();
                s_batch_done = finished;
                s_batch_done_pending = true;
            }
        }
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
    /* hr_batchstore_init() is NOT called here: the capture partition mounts on
     * a deferred task and is not available yet. The main loop initialises the
     * store once hr_capture_ready() reports the filesystem is up. */
    hr_batch_tracker_reset(&s_batch);

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

        /*
         * Power-loss recovery, decided once per boot.
         *
         * Needs two things to be true: the log is mounted, and the dryer has
         * told us where its batch clock stands. Its counter runs while we are
         * unpowered, so the difference against the last persisted point IS the
         * outage - no clock of our own, and nothing inferred.
         *
         * Any buckets recorded in the seconds before this runs are discarded:
         * restored history has to be contiguous and in order, and a handful of
         * post-boot samples are worth less than a correct timeline.
         */
        if (!s_resume_done && hr_capture_ready() && s_last_batch_elapsed >= 0) {
            s_resume_done = true;
            uint32_t last = 0;
            uint32_t now_elapsed = (uint32_t)s_last_batch_elapsed;
            xSemaphoreTake(s_hist_lock, portMAX_DELAY);
            hr_trend_reset(&s_trend);
            size_t n = hr_capture_trend_load(&s_trend, &last);
            if (n > 0 && now_elapsed >= last) {
                uint32_t gap_s = now_elapsed - last;
                size_t gap_buckets = gap_s / (HR_TREND_BUCKET_MS / 1000);
                hr_trend_restore_gap(&s_trend, gap_buckets);
                hr_trend_resume(&s_trend, t);
                s_trend_persisted = hr_trend_count(&s_trend);
                ESP_LOGI(TAG,
                         "resumed batch: %u points restored, %us gap "
                         "(%u missing buckets)",
                         (unsigned)n, (unsigned)gap_s, (unsigned)gap_buckets);
            } else {
                /* Either nothing stored, or the dryer's clock went backwards -
                 * a new batch began while we were down, so the stored run is
                 * finished and must not be drawn as part of this one. */
                hr_trend_reset(&s_trend);
                s_trend_persisted = 0;
                hr_capture_trend_reset();
                if (n > 0) {
                    ESP_LOGI(TAG, "stored graph belongs to a finished batch "
                                  "(elapsed %u < %u); discarded",
                             (unsigned)now_elapsed, (unsigned)last);
                }
            }
            xSemaphoreGive(s_hist_lock);
        }

        /*
         * Say hello once the link is up, then heartbeat.
         *
         * The genuine adapter opens with STATE / UNIQUE / FDNAME / REQCFG and
         * then emits STATE every ~15s. This firmware used to initiate nothing
         * at all, which one dryer tolerated and another did not - the latter
         * sending REQINFO forever and never starting telemetry.
         */
        {
            bool up = (s_session.link == HR_LINK_UP);
            unsigned mounts = hr_usb_mount_events();
            if (mounts != s_hello_mounts) {
                /* Fresh CDC session: the dryer has forgotten us. */
                s_hello_mounts = mounts;
                s_hello_step = 0;
            }
            if (!up) {
                s_hello_step = 0;
            } else if (s_hello_step < HR_HELLO_STEPS) {
                if (s_hello_step == 0 ||
                    (uint32_t)now_ms() - s_hello_ms >= 250u) {
                    if (s_hello_step == 0) {
                        ESP_LOGI(TAG,
                                 "link up: introducing ourselves to the dryer");
                    }
                    hr_session_hello_step(&s_session, s_hello_step);
                    s_hello_step++;
                    s_hello_ms = (uint32_t)now_ms();
                    s_heartbeat_ms = s_hello_ms;
                }
            } else if ((uint32_t)now_ms() - s_heartbeat_ms > 15000u) {
                s_heartbeat_ms = (uint32_t)now_ms();
                hr_session_heartbeat(&s_session);
            }
        }

        /*
         * Keep the session's idea of the network current, so REQINFO can be
         * answered with a WIFIINFO that reflects reality.
         *
         * The dryer's own panel shows this, and at least one firmware version
         * refuses to send telemetry at all until it gets an answer it accepts.
         * Refreshed here rather than on every WiFi event because the frame is
         * only sent when asked, every couple of seconds at most.
         */
        {
            char ssid[33];
            hr_wifi_current_ssid(ssid, sizeof(ssid));
            bool up = (hr_wifi_status() == HR_WIFI_CONNECTED);
            hr_session_set_wifi(&s_session, up ? 5 : 1,
                                hr_wifi_rssi_pct(), ssid, adapter_id());
            /*
             * Tell the dryer the link is complete once we actually have a
             * network. Hardcoded false for the life of this project, which is
             * why the machine's own WiFi panel always showed the SSID but
             * never a connection.
             */
            hr_session_set_cloud_auto(&s_session, up);
        }

        /* ---- batch logbook. All flash work happens on THIS task. ------- */
        if (!s_batch_store_inited && hr_capture_ready()) {
            s_batch_store_inited = true;
            hr_batchstore_init();
        }
        if (hr_batchstore_ready()) {
            /*
             * Once, after mount: a record left open in NVS means the adapter
             * went down mid-batch. Close it as interrupted rather than let it
             * silently merge into whatever runs next.
             */
            if (!s_batch_boot_checked) {
                s_batch_boot_checked = true;
                hr_batch_t open;
                if (hr_batchstore_load_open(&open)) {
                    open.outcome = HR_OUTCOME_INTERRUPTED;
                    hr_batchstore_append(&open);
                    hr_batchstore_clear_open();
                    ESP_LOGW(TAG, "recovered an interrupted batch: %s, %us",
                             open.name, (unsigned)open.duration_s);
                }
            }

            if (s_batch_done_pending) {
                s_batch_done_pending = false;
                hr_batchstore_append(&s_batch_done);
                hr_batchstore_clear_open();
            } else if (s_batch.active &&
                       (uint32_t)now_ms() - s_batch_saved_ms > 60000u) {
                /*
                 * Checkpoint the open batch once a minute: often enough that a
                 * power cut costs at most a minute of a 24-hour run, rare
                 * enough not to wear out the NVS partition.
                 */
                s_batch_saved_ms = (uint32_t)now_ms();
                hr_batchstore_save_open(&s_batch.cur);
            }
        }

        /*
         * Persist newly committed buckets. Deliberately on THIS task: the USB
         * RX callback must never queue flash work in bulk, and one point per
         * 30s is far below the queue depth.
         */
        if (s_resume_done) {
            xSemaphoreTake(s_hist_lock, portMAX_DELAY);
            size_t have = hr_trend_count(&s_trend);
            bool due = (have > s_trend_persisted);
            xSemaphoreGive(s_hist_lock);
            /*
             * One rewrite per new bucket - at most every 30s - and BACKED OFF
             * on failure. Without the backoff a failing write retried every
             * loop tick (4/s), which hammered the filesystem and buried the
             * real error in noise.
             */
            if (due && t - s_trend_last_try >= 5000UL) {
                s_trend_last_try = t;
                if (hr_capture_trend_save(&s_trend,
                                          (uint32_t)s_last_batch_elapsed)) {
                    s_trend_persisted = have;
                }
            }
        }
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
                     "frames_in=%lu frames_out=%lu link=%s | heap=%u | trend pts=%u persisted=%u "
                     "bytes=%u writes=%lu fails=%lu drops=%lu",
                     (int)hr_usb_mounted(), (int)hr_usb_suspended(),
                     hr_usb_mount_events(), hr_usb_rx_bytes(),
                     s_session.frames_in, s_session.frames_out,
                     s_session.link == HR_LINK_UP ? "UP" : "DOWN",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)hr_trend_count(&s_trend),
                     (unsigned)s_trend_persisted,
                     (unsigned)hr_capture_trend_bytes(),
                     hr_capture_trend_writes(),
                     hr_capture_trend_fails(),
                     hr_capture_dropped());
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
