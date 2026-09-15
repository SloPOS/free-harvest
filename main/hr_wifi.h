/*
 * WiFi provisioning + connection management.
 *
 * Behaviour:
 *   - Credentials are stored in NVS (namespace "hrwifi"), NOT compiled in,
 *     so the network can be changed without re-flashing.
 *   - On boot: if credentials exist, join that network as a station.
 *   - If none exist, or the join fails/drops for too long, start a setup
 *     access point (CONFIG_HR_AP_SSID) plus a captive-portal DNS so any
 *     browser request is redirected to the setup page.
 *   - The HTTP layer (hr_http) calls hr_wifi_set_credentials() when the user
 *     submits the setup form; this stores them and reconnects.
 *   - "Connected" means associated AND holding an IPv4 address. A station
 *     whose DHCP lease lapses stays associated (the driver never reports a
 *     disconnect), so a 2 s watchdog checks the address: after a few
 *     seconds without one the status is HR_WIFI_NO_IP, DHCP is restarted,
 *     and if that does not help the network is left and rejoined, with
 *     back-off (hr_netwatch.h, CONFIG_HR_WIFI_NOIP_*).
 */
#ifndef HR_WIFI_H
#define HR_WIFI_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    HR_WIFI_BOOTING = 0,
    HR_WIFI_AP_SETUP,   /* setup AP is up, waiting for credentials */
    HR_WIFI_CONNECTING, /* trying stored credentials */
    HR_WIFI_CONNECTED,  /* station connected, has IP */
    /*
     * Station associated, but no usable IPv4 address for longer than a few
     * seconds: the DHCP lease was lost and not renewed, or a join never got
     * one. The radio and RSSI look fine; nothing is reachable. hr_wifi is
     * already restarting DHCP / rejoining (see hr_netwatch.h); callers must
     * treat this as NOT connected.
     */
    HR_WIFI_NO_IP,
} hr_wifi_status_t;

void hr_wifi_start(void);

hr_wifi_status_t hr_wifi_status(void);

/* Current station IP as a string ("0.0.0.0" if not connected). */
void hr_wifi_ip(char *out, size_t cap);

/*
 * The no-IP watchdog, for /api/state. `associated` is the driver's view of
 * the link; `noip_s` is how long the station has been associated without an
 * address (0 when it has one or is not associated); the counters are since
 * boot: episodes that outlived the grace period, DHCP client restarts and
 * forced rejoins issued to get an address back.
 */
typedef struct {
    bool associated;
    unsigned long noip_s;
    unsigned episodes;
    unsigned dhcp_restarts;
    unsigned reconnects;
} hr_wifi_noip_stats_t;

void hr_wifi_noip_stats(hr_wifi_noip_stats_t *out);

/* The SSID we are connected to or configured for ("" if none). */
void hr_wifi_current_ssid(char *out, size_t cap);

/* Signal strength as a 0-100 percentage, 0 when not associated.
 * Reported to the dryer in WIFIINFO, which is what its own panel
 * shows. */
int hr_wifi_rssi_pct(void);

/* Store new credentials, then reconnect. Returns false on bad input. */
bool hr_wifi_set_credentials(const char *ssid, const char *password);

/* Forget stored credentials and fall back to the setup AP. */
void hr_wifi_forget(void);

/*
 * A deliberate restart is about to happen (hr_reboot_request()). From here
 * the event handler ignores the disconnect that esp_restart()'s Wi-Fi
 * shutdown handler produces - instead of re-opening the setup AP and
 * reconnecting into a stack that is being torn down - and the retry timers
 * are stopped. Nothing else changes; the driver is stopped by esp_restart().
 */
void hr_wifi_prepare_restart(void);

/* Trigger an async scan; results retrieved with hr_wifi_scan_result_json(). */
void hr_wifi_scan_start(void);

/*
 * Write a JSON array of the most recent scan results into `out`:
 *   [{"ssid":"..","rssi":-60,"secure":true}, ...]
 * Returns bytes written.
 */
size_t hr_wifi_scan_result_json(char *out, size_t cap);

#endif /* HR_WIFI_H */
