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
} hr_wifi_status_t;

void hr_wifi_start(void);

hr_wifi_status_t hr_wifi_status(void);

/* Current station IP as a string ("0.0.0.0" if not connected). */
void hr_wifi_ip(char *out, size_t cap);

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

/* Trigger an async scan; results retrieved with hr_wifi_scan_result_json(). */
void hr_wifi_scan_start(void);

/*
 * Write a JSON array of the most recent scan results into `out`:
 *   [{"ssid":"..","rssi":-60,"secure":true}, ...]
 * Returns bytes written.
 */
size_t hr_wifi_scan_result_json(char *out, size_t cap);

#endif /* HR_WIFI_H */
