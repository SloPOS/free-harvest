/*
 * MQTT client with Home Assistant auto-discovery.
 *
 * Connects to an external broker (e.g. the Mosquitto add-on in Home Assistant)
 * and:
 *   - publishes HA MQTT-discovery configs so the dryer appears automatically
 *     as a device with sensors (temp, pressure, state, elapsed, prep) and
 *     controls (refresh, beep, batch-name, recipe push);
 *   - publishes telemetry to a state topic on every STAT frame;
 *   - subscribes to a command topic and routes inbound commands through the
 *     tested allow-list (read-only + benign + config; hardware verbs refused).
 *
 * Broker settings are stored in NVS (set via the web setup page), so no
 * re-flashing to point at a different broker.
 *
 * Security model: the device is expected to live on a trusted LAN behind a
 * VPN; MQTT itself is not additionally authenticated by this firmware beyond
 * the broker's own credentials.
 */
#ifndef HR_MQTT_H
#define HR_MQTT_H

#include "hr_session.h"
#include "hr_telemetry.h"

#include <stdbool.h>
#include <stddef.h>

/* Start MQTT if a broker is configured in NVS. Safe to call always. */
void hr_mqtt_start(hr_session_t *session);

/* Publish telemetry (called from the frame observer on each STAT). */
void hr_mqtt_publish_telemetry(const hr_telemetry_t *t);

/* Publish a raw inbound frame line to a debug topic (optional visibility). */
void hr_mqtt_publish_frame(const char *verb, const char *body);

/* True once connected to the broker. */
bool hr_mqtt_connected(void);

/* Store broker settings and (re)connect. host required; user/pass optional. */
bool hr_mqtt_set_broker(const char *host, int port, const char *user,
                        const char *pass);

/* Fill JSON status for the web UI: {"configured":..,"connected":..,"host":..}*/
size_t hr_mqtt_status_json(char *out, size_t cap);

#endif /* HR_MQTT_H */
