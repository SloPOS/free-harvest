/*
 * No-IP watchdog: the decision logic for a station that is associated with
 * the access point but has no usable IPv4 address.
 *
 * Why this exists. A DHCP lease does not last forever. When lwIP fails to
 * renew one (the router's DHCP server was restarted, the association went
 * stale after a group-key rotation, the AP was replaced by one on the same
 * SSID...), the lease expires, lwIP clears the address to 0.0.0.0 and starts
 * broadcasting DISCOVER again. The Wi-Fi driver, meanwhile, is still happily
 * associated: beacons arrive, RSSI reads fine, and WIFI_EVENT_STA_DISCONNECTED
 * never fires. Everything above the driver that keys "connected" off that
 * event - the status line, the dryer's WIFIINFO, the display - keeps saying
 * the link is up while the adapter is unreachable, for as long as the lease
 * stays lost. Seen on real hardware after several hours: IP 0.0.0.0, a
 * healthy signal bar, no alert.
 *
 * What it decides, given "associated?" and "have an IP?" at each tick:
 *
 *   no IP for grace_ms          -> report NO_IP (status/alerts flip)
 *   no IP for dhcp_restart_ms   -> restart the DHCP client once
 *   no IP for reconnect_ms      -> leave and rejoin the network, so the
 *                                  association and DHCP both start fresh;
 *                                  repeated with doubling back-off while the
 *                                  address stays missing
 *
 * Pure and host-testable: no ESP-IDF, no timers of its own. The caller feeds
 * it the clock and the two facts and performs the action it returns. Time is
 * a uint32_t millisecond counter; wrap-around is handled by subtraction.
 */
#ifndef HR_NETWATCH_H
#define HR_NETWATCH_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HR_NETWATCH_NONE = 0,     /* nothing to do */
    HR_NETWATCH_DHCP_RESTART, /* stop + start the DHCP client */
    HR_NETWATCH_RECONNECT,    /* disconnect + connect the station */
} hr_netwatch_action_t;

typedef struct {
    uint32_t grace_ms;        /* no IP tolerated before it counts as NO_IP */
    uint32_t dhcp_restart_ms; /* no IP for this long: restart the client */
    uint32_t reconnect_ms;    /* no IP for this long: rejoin the network */
    uint32_t reconnect_max_ms; /* cap for the doubling reconnect back-off */
} hr_netwatch_cfg_t;

typedef struct {
    hr_netwatch_cfg_t cfg;
    bool associated;
    bool have_ip;
    bool in_episode;          /* associated with no address right now */
    bool reported;            /* this episode has outlived the grace */
    uint32_t noip_since_ms;   /* when the current no-IP episode began */
    uint32_t last_action_ms;  /* when the last remedy was issued */
    uint8_t stage;            /* 0 waiting, 1 DHCP restarted, 2 reconnected */
    uint32_t backoff_n;       /* consecutive reconnects without an IP */
    /* Since boot, for /api/state. */
    uint32_t episodes;        /* no-IP episodes that outlived the grace */
    uint32_t dhcp_restarts;
    uint32_t reconnects;
} hr_netwatch_t;

/* Sensible defaults: 5 s grace, restart DHCP at 15 s, rejoin at 45 s,
 * back-off capped at 6 minutes. */
void hr_netwatch_init(hr_netwatch_t *w, const hr_netwatch_cfg_t *cfg);

/* Station associated / left the access point. */
void hr_netwatch_on_assoc(hr_netwatch_t *w, uint32_t now_ms);
void hr_netwatch_on_disassoc(hr_netwatch_t *w, uint32_t now_ms);

/*
 * Feed the current facts and get the remedy due right now, if any. Call it
 * periodically (a couple of seconds) and from the GOT_IP / LOST_IP events.
 * An action is returned at most once per stage per episode; the caller
 * performs it. Returns HR_NETWATCH_NONE when not associated or when the
 * address is present.
 */
hr_netwatch_action_t hr_netwatch_tick(hr_netwatch_t *w, bool have_ip,
                                      uint32_t now_ms);

/* True while associated with no address for longer than the grace. */
bool hr_netwatch_no_ip(const hr_netwatch_t *w, uint32_t now_ms);

/* Milliseconds of the current no-IP episode, 0 when there is none. */
uint32_t hr_netwatch_noip_for_ms(const hr_netwatch_t *w, uint32_t now_ms);

/* The reconnect delay in force for the current episode (back-off applied). */
uint32_t hr_netwatch_reconnect_delay_ms(const hr_netwatch_t *w);

#endif /* HR_NETWATCH_H */
