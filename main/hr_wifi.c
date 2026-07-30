#include "hr_wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "hr_wifi";
#define NVS_NS "hrwifi"
#define MAX_SCAN 16
#define STA_RETRY_LIMIT 8 /* consecutive failures before falling back to AP */
/*
 * Security: the setup AP is only open for this long. If no successful home-
 * network connection happens in the window, the AP is shut down and a reboot
 * is required to re-open it. This stops an internet/router outage from leaving
 * the device broadcasting an open setup network indefinitely for attackers.
 */
#define AP_OPEN_WINDOW_US (5 * 60 * 1000000ULL)

static hr_wifi_status_t s_status;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static int s_sta_retries;
static char s_ssid[33];
static esp_timer_handle_t s_ap_timeout_timer;
static bool s_ap_window_expired; /* true once the 5-min window has closed */

static wifi_ap_record_t s_scan[MAX_SCAN];
static uint16_t s_scan_count;
static volatile bool s_scanning;

/* -------------------------------------------------------------------- */
/* NVS credential storage                                                */
/* -------------------------------------------------------------------- */
static bool load_credentials(char *ssid, size_t ssid_cap, char *pw,
                             size_t pw_cap)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READONLY, &nh) != ESP_OK) {
        return false;
    }
    size_t sl = ssid_cap, pl = pw_cap;
    bool ok = nvs_get_str(nh, "ssid", ssid, &sl) == ESP_OK &&
              nvs_get_str(nh, "pw", pw, &pl) == ESP_OK && ssid[0] != '\0';
    nvs_close(nh);
    return ok;
}

static bool store_credentials(const char *ssid, const char *pw)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_str(nh, "ssid", ssid) == ESP_OK &&
              nvs_set_str(nh, "pw", pw) == ESP_OK && nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    return ok;
}

/* -------------------------------------------------------------------- */
/* Mode switching                                                        */
/* -------------------------------------------------------------------- */

/*
 * AP-timeout: fires once, AP_OPEN_WINDOW_US after the setup AP opens. If we
 * have not connected to the home network by then, shut the AP down (STA-only)
 * so the device stops broadcasting. Re-opening requires a reboot.
 * Runs in the esp_timer task - safe to call esp_wifi_* here.
 */
static void ap_timeout_cb(void *arg)
{
    (void)arg;
    if (s_status == HR_WIFI_CONNECTED) {
        return; /* already connected; AP was closed on GOT_IP */
    }
    ESP_LOGW(TAG, "setup-AP window (5 min) elapsed with no connection; "
                  "disabling AP. Reboot to re-open setup.");
    s_ap_window_expired = true;
    /* STA-only kills the AP but keeps trying stored creds (if any). */
    esp_wifi_set_mode(WIFI_MODE_STA);
}

static void arm_ap_timeout(void)
{
    if (s_ap_window_expired) {
        return; /* window already used this boot; do not re-open */
    }
    if (s_ap_timeout_timer == NULL) {
        const esp_timer_create_args_t a = {.callback = ap_timeout_cb,
                                           .name = "ap_timeout"};
        if (esp_timer_create(&a, &s_ap_timeout_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(s_ap_timeout_timer); /* restart the window cleanly */
    esp_timer_start_once(s_ap_timeout_timer, AP_OPEN_WINDOW_US);
}

static void cancel_ap_timeout(void)
{
    if (s_ap_timeout_timer != NULL) {
        esp_timer_stop(s_ap_timeout_timer);
    }
}

/*
 * Configure the setup AP. Does NOT start the WiFi driver - the driver is
 * started exactly once in hr_wifi_start(). Callable any time to (re)assert
 * the AP config (e.g. hr_wifi_forget()).
 */
static void configure_ap(void)
{
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", CONFIG_HR_AP_SSID);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;

    const char *pw = CONFIG_HR_AP_PASSWORD;
    if (strlen(pw) >= 8) {
        snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", pw);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
}

static void start_ap_mode(void)
{
    if (s_ap_window_expired) {
        /* The one-time setup window is over; refuse to re-broadcast the AP.
         * Stay STA-only and keep retrying stored credentials. */
        ESP_LOGW(TAG, "setup AP not re-opened (window used); STA-only. "
                      "Reboot to run setup again.");
        s_status = HR_WIFI_CONNECTING;
        esp_wifi_set_mode(WIFI_MODE_STA);
        return;
    }
    ESP_LOGI(TAG, "setup AP \"%s\" active (open for 5 min)", CONFIG_HR_AP_SSID);
    s_status = HR_WIFI_AP_SETUP;
    configure_ap();
    arm_ap_timeout();
    /* WiFi is already started by hr_wifi_start(); nothing else to do. */
}

/*
 * Configure the station and connect. WiFi is already started in APSTA mode by
 * hr_wifi_start(), so we MUST NOT call esp_wifi_start() again here - we only
 * set the STA config and initiate the connect.
 *
 * NOTE (single-radio APSTA): once the STA associates, the softAP is forced to
 * the STA's channel (esp_wifi.h). A browser on the setup AP will briefly drop
 * when that happens, then can reconnect to the AP (now on the new channel).
 * The AP is deliberately kept up so the status page remains reachable and can
 * report the new station IP.
 */
static void start_sta_connect(const char *ssid, const char *pw)
{
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    s_status = HR_WIFI_CONNECTING;
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);

    wifi_config_t sta = {0};
    snprintf((char *)sta.sta.ssid, sizeof(sta.sta.ssid), "%s", ssid);
    snprintf((char *)sta.sta.password, sizeof(sta.sta.password), "%s", pw);
    /* Accept any auth mode the AP offers (open, WPA2, WPA3-transition). */
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

/* -------------------------------------------------------------------- */
/* Event handling                                                        */
/* -------------------------------------------------------------------- */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /*
         * Runs in the WiFi event task - MUST NOT block. Reconnect immediately
         * (no vTaskDelay here; that would stall the stack and the setup AP's
         * beacons). We keep the AP up throughout so the user is never locked
         * out. After many failures we stop hammering but stay in APSTA.
         */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (s_status == HR_WIFI_CONNECTED) {
            /*
             * We had a working link and lost it (e.g. router/internet outage).
             * Reconnect, but DO NOT re-broadcast the setup AP if the one-time
             * setup window already closed - an outage must not reopen an attack
             * surface. Only restore the AP if the window is still valid.
             */
            if (!s_ap_window_expired) {
                ESP_LOGW(TAG, "link dropped (reason %d), restoring setup AP + "
                              "reconnecting", d->reason);
                esp_wifi_set_mode(WIFI_MODE_APSTA);
                configure_ap();
                arm_ap_timeout();
            } else {
                ESP_LOGW(TAG, "link dropped (reason %d), reconnecting STA-only "
                              "(setup AP window used)", d->reason);
            }
            s_status = HR_WIFI_CONNECTING;
            esp_wifi_connect();
        } else if (++s_sta_retries < STA_RETRY_LIMIT) {
            ESP_LOGW(TAG, "join attempt %d failed (reason %d), retrying",
                     s_sta_retries, d->reason);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "join failed %d times (reason %d); staying in setup "
                          "AP - re-submit credentials from the page to retry",
                     s_sta_retries, d->reason);
            s_status = HR_WIFI_AP_SETUP;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_status = HR_WIFI_CONNECTED;
        s_sta_retries = 0;
        cancel_ap_timeout(); /* connected in time; no need to force-close AP */
        /*
         * Security: once we're on the home network, shut the setup AP down
         * entirely (STA-only) so nobody can join HR-Adapter-Setup and try to
         * reconfigure the device while it's in use. The AP is restored
         * automatically if the STA link later drops (see disconnect handler).
         */
        esp_err_t merr = esp_wifi_set_mode(WIFI_MODE_STA);
        if (merr == ESP_OK) {
            ESP_LOGI(TAG, "setup AP disabled (STA-only) for security");
        } else {
            ESP_LOGW(TAG, "could not disable setup AP: %s",
                     esp_err_to_name(merr));
        }
    }
    /* Scan results are collected synchronously in hr_wifi_scan_start(); no
     * SCAN_DONE handling needed here. */
}

/* -------------------------------------------------------------------- */
/* Captive-portal DNS: answer every A query with our AP address.         */
/* -------------------------------------------------------------------- */
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* AP gateway address the portal lives on (default 192.168.4.1). */
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src,
                         &slen);
        if (n < (int)sizeof(uint16_t) * 6) {
            continue;
        }
        /* Only answer standard queries with exactly one question. */
        if ((buf[2] & 0x80) != 0) {
            continue;
        }
        /* Build a minimal response: flip QR bit, one answer A record. */
        buf[2] |= 0x80; /* response */
        buf[3] |= 0x80; /* recursion available */
        buf[6] = 0x00;
        buf[7] = 0x01; /* ANCOUNT = 1 */

        /* Append answer pointing back at the question name (0xC00C). */
        if (n + 16 > (int)sizeof(buf)) {
            continue;
        }
        uint8_t *p = buf + n;
        *p++ = 0xC0;
        *p++ = 0x0C; /* name pointer to offset 12 */
        *p++ = 0x00;
        *p++ = 0x01; /* type A */
        *p++ = 0x00;
        *p++ = 0x01; /* class IN */
        *p++ = 0x00;
        *p++ = 0x00;
        *p++ = 0x00;
        *p++ = 0x3C; /* TTL 60s */
        *p++ = 0x00;
        *p++ = 0x04; /* RDLENGTH 4 */
        memcpy(p, &ip.ip.addr, 4);
        p += 4;

        sendto(sock, buf, p - buf, 0, (struct sockaddr *)&src, slen);
    }
}

/* -------------------------------------------------------------------- */
/* Public API                                                            */
/* -------------------------------------------------------------------- */
void hr_wifi_start(void)
{
    s_status = HR_WIFI_BOOTING;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL, NULL));

    /*
     * Bring the driver up ONCE, in APSTA, with the setup AP always configured.
     * The AP stays up permanently so the user is never locked out; the STA is
     * connected on demand. esp_wifi_start() must happen before any
     * esp_wifi_connect() call (else ESP_ERR_WIFI_NOT_STARTED).
     */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    configure_ap();
    ESP_ERROR_CHECK(esp_wifi_start());

    char ssid[33] = {0}, pw[65] = {0};
    if (load_credentials(ssid, sizeof(ssid), pw, sizeof(pw))) {
        start_sta_connect(ssid, pw); /* safe now: driver is started */
    } else {
        start_ap_mode();
    }

    /* Captive-portal DNS is harmless in STA mode too, so always run it. */
    xTaskCreate(dns_task, "hr_dns", 3072, NULL, 4, NULL);
}

hr_wifi_status_t hr_wifi_status(void)
{
    return s_status;
}

void hr_wifi_ip(char *out, size_t cap)
{
    esp_netif_ip_info_t ip = {0};
    if (s_status == HR_WIFI_CONNECTED) {
        esp_netif_get_ip_info(s_sta_netif, &ip);
    } else {
        esp_netif_get_ip_info(s_ap_netif, &ip);
    }
    snprintf(out, cap, IPSTR, IP2STR(&ip.ip));
}

void hr_wifi_current_ssid(char *out, size_t cap)
{
    snprintf(out, cap, "%s", s_ssid);
}

bool hr_wifi_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) {
        return false;
    }
    if (password == NULL) {
        password = "";
    }
    if (!store_credentials(ssid, password)) {
        return false;
    }
    s_sta_retries = 0;
    esp_wifi_disconnect();
    start_sta_connect(ssid, password);
    return true;
}

void hr_wifi_forget(void)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
        nvs_erase_all(nh);
        nvs_commit(nh);
        nvs_close(nh);
    }
    start_ap_mode();
}

void hr_wifi_scan_start(void)
{
    /*
     * Blocking scan so the HTTP handler returns fresh results in one request.
     * The earlier async design returned the PREVIOUS scan (empty on first
     * call), so the UI never showed anything. A blocking scan takes ~1-2s;
     * we guard against overlapping calls and against scanning while a STA
     * connect is in flight (which would return ESP_ERR_WIFI_STATE).
     */
    if (s_scanning) {
        return;
    }
    if (s_status == HR_WIFI_CONNECTING) {
        return; /* can't scan mid-connect; UI keeps last results */
    }
    s_scanning = true;

    wifi_scan_config_t cfg = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&cfg, true); /* block until done */
    if (err == ESP_OK) {
        uint16_t n = MAX_SCAN;
        esp_wifi_scan_get_ap_records(&n, s_scan);
        s_scan_count = n;
        ESP_LOGI(TAG, "scan found %u networks", n);
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }
    s_scanning = false;
}

size_t hr_wifi_scan_result_json(char *out, size_t cap)
{
    size_t o = 0;
    o += snprintf(out + o, cap - o, "[");
    for (uint16_t i = 0; i < s_scan_count && o < cap - 96; i++) {
        char ssid_esc[64];
        /* SSIDs can contain quotes/backslashes; escape for JSON. */
        size_t e = 0;
        for (size_t k = 0; s_scan[i].ssid[k] && e < sizeof(ssid_esc) - 2; k++) {
            char c = (char)s_scan[i].ssid[k];
            if (c == '"' || c == '\\') {
                ssid_esc[e++] = '\\';
            }
            ssid_esc[e++] = c;
        }
        ssid_esc[e] = '\0';
        bool secure = s_scan[i].authmode != WIFI_AUTH_OPEN;
        o += snprintf(out + o, cap - o,
                      "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                      i ? "," : "", ssid_esc, s_scan[i].rssi,
                      secure ? "true" : "false");
    }
    o += snprintf(out + o, cap - o, "]");
    return o;
}
