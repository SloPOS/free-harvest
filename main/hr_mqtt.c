#include "hr_mqtt.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "hr_mqtt";
#define NVS_NS "hrmqtt"

/*
 * Topic scheme (unique per device via MAC-derived id):
 *   hrdryer/<id>/state        <- telemetry JSON (retained)
 *   hrdryer/<id>/avail        <- "online"/"offline" (LWT, retained)
 *   hrdryer/<id>/frame        <- raw frame debug (not retained)
 *   hrdryer/<id>/cmd          -> inbound: verb-only safe command (e.g. REQSTAT)
 *   hrdryer/<id>/config/set   -> inbound: "VERB,args" config write
 * HA discovery configs are published under homeassistant/.../config (retained).
 */
static esp_mqtt_client_handle_t s_client;
static hr_session_t *s_session;
static bool s_connected;
static char s_id[16];         /* e.g. "a1b2c3" from MAC */
static char s_host[64];
static int s_port;
static char s_base[40];       /* "hrdryer/<id>" */

/* ---- NVS broker config ---------------------------------------------------*/
static bool load_broker(void)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READONLY, &nh) != ESP_OK) {
        return false;
    }
    size_t hl = sizeof(s_host);
    int32_t port = 1883;
    bool ok = nvs_get_str(nh, "host", s_host, &hl) == ESP_OK && s_host[0];
    nvs_get_i32(nh, "port", &port);
    s_port = port;
    nvs_close(nh);
    return ok;
}

static bool save_broker(const char *host, int port, const char *user,
                        const char *pass)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_str(nh, "host", host) == ESP_OK &&
              nvs_set_i32(nh, "port", port) == ESP_OK &&
              nvs_set_str(nh, "user", user ? user : "") == ESP_OK &&
              nvs_set_str(nh, "pass", pass ? pass : "") == ESP_OK &&
              nvs_commit(nh) == ESP_OK;
    nvs_close(nh);
    return ok;
}

static void get_creds(char *user, size_t ul, char *pass, size_t pl)
{
    user[0] = '\0';
    pass[0] = '\0';
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        size_t a = ul, b = pl;
        nvs_get_str(nh, "user", user, &a);
        nvs_get_str(nh, "pass", pass, &b);
        nvs_close(nh);
    }
}

/* ---- publish helpers -----------------------------------------------------*/
static void pub(const char *topic, const char *payload, int retain)
{
    if (s_client && s_connected) {
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, retain);
    }
}

/*
 * One HA discovery sensor. `object_id` is unique within the device; `tmpl` is
 * the value_template that extracts the field from the state JSON.
 */
static void discover_sensor(const char *object_id, const char *name,
                            const char *unit, const char *device_class,
                            const char *tmpl)
{
    char topic[160];
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s_%s/config", s_id, object_id);

    char unit_field[48] = "";
    if (unit && unit[0]) {
        snprintf(unit_field, sizeof(unit_field),
                 "\"unit_of_measurement\":\"%s\",", unit);
    }
    char dc_field[64] = "";
    if (device_class && device_class[0]) {
        snprintf(dc_field, sizeof(dc_field),
                 "\"device_class\":\"%s\",", device_class);
    }

    char payload[520];
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"%s\","
             "\"uniq_id\":\"%s_%s\","
             "\"stat_t\":\"%s/state\","
             "\"avty_t\":\"%s/avail\","
             "%s%s"
             "\"val_tpl\":\"%s\","
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"HarvestRight Freeze Dryer\","
             "\"mf\":\"HarvestRight\",\"mdl\":\"Freeze Dryer (ESP32 adapter)\"}"
             "}",
             name, s_id, object_id, s_base, s_base, unit_field, dc_field,
             tmpl, s_id);
    pub(topic, payload, 1);
}

/* An HA button that publishes a fixed command to our cmd topic. */
static void discover_button(const char *object_id, const char *name,
                            const char *cmd_topic, const char *press_payload)
{
    char topic[160];
    snprintf(topic, sizeof(topic),
             "homeassistant/button/%s_%s/config", s_id, object_id);
    char payload[420];
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"%s\","
             "\"uniq_id\":\"%s_%s\","
             "\"cmd_t\":\"%s\","
             "\"payload_press\":\"%s\","
             "\"avty_t\":\"%s/avail\","
             "\"dev\":{\"ids\":[\"%s\"]}"
             "}",
             name, s_id, object_id, cmd_topic, press_payload, s_base, s_id);
    pub(topic, payload, 1);
}

/* An HA text entity that pushes a config command with the typed value. */
static void discover_text(const char *object_id, const char *name,
                          const char *cmd_topic)
{
    char topic[160];
    snprintf(topic, sizeof(topic),
             "homeassistant/text/%s_%s/config", s_id, object_id);
    char payload[420];
    snprintf(payload, sizeof(payload),
             "{"
             "\"name\":\"%s\","
             "\"uniq_id\":\"%s_%s\","
             "\"cmd_t\":\"%s\","
             "\"avty_t\":\"%s/avail\","
             "\"dev\":{\"ids\":[\"%s\"]}"
             "}",
             name, s_id, object_id, cmd_topic, s_base, s_id);
    pub(topic, payload, 1);
}

static void publish_discovery(void)
{
    char cmd[80], cfg[80], bname[96];
    snprintf(cmd, sizeof(cmd), "%s/cmd", s_base);
    snprintf(cfg, sizeof(cfg), "%s/config/set", s_base);

    /* Sensors (value_templates read the state JSON from hr_telemetry_to_json) */
    discover_sensor("temp", "Temperature", "\\u00b0F", "temperature",
                    "{{ value_json.temp_f }}");
    discover_sensor("pressure", "Pressure (raw)", "", "",
                    "{{ value_json.pressure }}");
    discover_sensor("state_type", "State Code", "", "",
                    "{{ value_json.type }}");
    discover_sensor("mode", "Mode", "", "", "{{ value_json.mode }}");
    discover_sensor("elapsed", "Batch Elapsed", "s", "duration",
                    "{{ value_json.elapsed_s }}");
    discover_sensor("prep", "Prep Remaining", "s", "duration",
                    "{{ value_json.prep_s }}");

    /* Buttons (safe commands) */
    discover_button("refresh", "Refresh Status", cmd, "REQSTAT");
    discover_button("beep", "Beep", cmd, "BEEP");

    /* Config write: batch name via a text entity. The value the user types is
     * published to config/set; hr_mqtt routes it as "SETBNAME,<value>". */
    (void)bname;
    discover_text("batch_name", "Batch Name", cfg);

    ESP_LOGI(TAG, "published HA discovery configs");
}

/* ---- inbound command handling -------------------------------------------*/
/*
 * cmd topic: payload is a bare verb (safe command) OR "VERB,args".
 * config/set topic (from the batch-name text entity): payload is the batch
 * name; we turn it into "SETBNAME,<name>".
 * Everything routes through the tested allow-list, so hardware/unknown verbs
 * are refused here regardless of payload.
 */
static void handle_cmd(const char *topic, const char *data, int len)
{
    char buf[256];
    if (len <= 0 || len >= (int)sizeof(buf)) {
        return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';

    bool is_config_set = strstr(topic, "/config/set") != NULL;

    if (is_config_set) {
        /* Batch-name text entity -> SETBNAME,<payload> (buf is already the
         * NUL-terminated payload). */
        bool ok = hr_session_send_config(s_session, "SETBNAME", buf);
        ESP_LOGI(TAG, "MQTT SETBNAME '%s' -> %s", buf, ok ? "sent" : "refused");
        return;
    }

    /* cmd topic: split verb,args */
    char *comma = strchr(buf, ',');
    const char *verb = buf;
    const char *args = NULL;
    if (comma) {
        *comma = '\0';
        args = comma + 1;
    }
    bool ok = hr_session_send_config(s_session, verb, args);
    ESP_LOGI(TAG, "MQTT cmd '%s' args '%s' -> %s", verb, args ? args : "",
             ok ? "sent" : "refused");
}

/* ---- MQTT event loop -----------------------------------------------------*/
static void on_mqtt(void *handler_args, esp_event_base_t base, int32_t id,
                    void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;
    char t[96];

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "broker connected");
        /* availability online (retained) */
        snprintf(t, sizeof(t), "%s/avail", s_base);
        pub(t, "online", 1);
        /* subscribe to command topics */
        snprintf(t, sizeof(t), "%s/cmd", s_base);
        esp_mqtt_client_subscribe(s_client, t, 1);
        snprintf(t, sizeof(t), "%s/config/set", s_base);
        esp_mqtt_client_subscribe(s_client, t, 1);
        publish_discovery();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "broker disconnected (will retry)");
        break;
    case MQTT_EVENT_DATA:
        handle_cmd(e->topic, e->data, e->data_len);
        break;
    case MQTT_EVENT_ERROR:
        /*
         * This is the diagnostic that matters when "nothing happens": report
         * exactly why the connect failed so it shows in the in-app log.
         */
        if (e->error_handle) {
            if (e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "TCP error: sock_errno=%d tls=%d (check broker "
                              "IP/port reachable, and that the broker allows "
                              "this client)",
                         e->error_handle->esp_transport_sock_errno,
                         e->error_handle->esp_tls_last_esp_err);
            } else if (e->error_handle->error_type ==
                       MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                ESP_LOGE(TAG, "broker REFUSED connection: return_code=%d "
                              "(1=bad protocol 2=bad client id 3=unavailable "
                              "4=bad user/pass 5=not authorized)",
                         e->error_handle->connect_return_code);
            } else {
                ESP_LOGE(TAG, "mqtt error type=%d",
                         (int)e->error_handle->error_type);
            }
        } else {
            ESP_LOGE(TAG, "mqtt error (no detail)");
        }
        break;
    default:
        break;
    }
}

/* ---- public API ----------------------------------------------------------*/
static void derive_id(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof(s_id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_base, sizeof(s_base), "hrdryer/%s", s_id);
}

static void connect_now(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    char user[64], pass[96];
    get_creds(user, sizeof(user), pass, sizeof(pass));
    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", s_host, s_port);

    char avail_topic[96];
    snprintf(avail_topic, sizeof(avail_topic), "%s/avail", s_base);

    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = uri;
    cfg.credentials.username = user[0] ? user : NULL;
    cfg.credentials.authentication.password = pass[0] ? pass : NULL;
    /* Last will: mark offline if we drop. */
    cfg.session.last_will.topic = avail_topic;
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    esp_err_t serr = esp_mqtt_client_start(s_client);
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "client_start failed: %s", esp_err_to_name(serr));
        return;
    }
    ESP_LOGI(TAG, "connecting to broker %s (user=%s)", uri,
             user[0] ? user : "<none>");
}

void hr_mqtt_start(hr_session_t *session)
{
    s_session = session;
    derive_id();
    if (!load_broker()) {
        ESP_LOGI(TAG, "no broker configured; MQTT idle");
        return;
    }
    connect_now();
}

void hr_mqtt_publish_telemetry(const hr_telemetry_t *t)
{
    if (!s_connected || !t || !t->valid) {
        return;
    }
    char json[256];
    if (hr_telemetry_to_json(t, json, sizeof(json)) == 0) {
        return;
    }
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/state", s_base);
    pub(topic, json, 1); /* retained so HA shows last value after restart */
}

void hr_mqtt_publish_frame(const char *verb, const char *body)
{
    if (!s_connected) {
        return;
    }
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/frame", s_base);
    char payload[HR_MAX_FRAME + 8];
    snprintf(payload, sizeof(payload), "%s", body ? body : verb);
    pub(topic, payload, 0);
}

bool hr_mqtt_connected(void)
{
    return s_connected;
}

bool hr_mqtt_set_broker(const char *host, int port, const char *user,
                        const char *pass)
{
    if (!host || !host[0] || port <= 0 || port > 65535) {
        return false;
    }
    if (!save_broker(host, port, user, pass)) {
        return false;
    }
    snprintf(s_host, sizeof(s_host), "%s", host);
    s_port = port;
    connect_now();
    return true;
}

size_t hr_mqtt_status_json(char *out, size_t cap)
{
    return (size_t)snprintf(out, cap,
                            "{\"configured\":%s,\"connected\":%s,"
                            "\"host\":\"%s\",\"port\":%d}",
                            s_host[0] ? "true" : "false",
                            s_connected ? "true" : "false", s_host, s_port);
}
