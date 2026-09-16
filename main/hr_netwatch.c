/*
 * No-IP watchdog decision logic. See hr_netwatch.h for the why.
 */
#include "hr_netwatch.h"

#include <string.h>

#define DEFAULT_GRACE_MS         (5u * 1000u)
#define DEFAULT_DHCP_RESTART_MS  (15u * 1000u)
#define DEFAULT_RECONNECT_MS     (45u * 1000u)
#define DEFAULT_RECONNECT_MAX_MS (6u * 60u * 1000u)
#define BACKOFF_SHIFT_CAP        10u /* 2^10 x base is already past any cap */

void hr_netwatch_init(hr_netwatch_t *w, const hr_netwatch_cfg_t *cfg)
{
    memset(w, 0, sizeof(*w));
    if (cfg != NULL) {
        w->cfg = *cfg;
    }
    if (w->cfg.grace_ms == 0) {
        w->cfg.grace_ms = DEFAULT_GRACE_MS;
    }
    if (w->cfg.dhcp_restart_ms == 0) {
        w->cfg.dhcp_restart_ms = DEFAULT_DHCP_RESTART_MS;
    }
    if (w->cfg.reconnect_ms == 0) {
        w->cfg.reconnect_ms = DEFAULT_RECONNECT_MS;
    }
    if (w->cfg.reconnect_max_ms < w->cfg.reconnect_ms) {
        w->cfg.reconnect_max_ms = w->cfg.reconnect_ms > DEFAULT_RECONNECT_MAX_MS
                                      ? w->cfg.reconnect_ms
                                      : DEFAULT_RECONNECT_MAX_MS;
    }
}

static void begin_episode(hr_netwatch_t *w, uint32_t now_ms)
{
    w->in_episode = true;
    w->reported = false;
    w->noip_since_ms = now_ms;
    w->stage = 0;
}

static void end_episode(hr_netwatch_t *w)
{
    w->in_episode = false;
    w->reported = false;
    w->stage = 0;
}

void hr_netwatch_on_assoc(hr_netwatch_t *w, uint32_t now_ms)
{
    w->associated = true;
    w->have_ip = false;
    /* The DHCP clock starts at association: a join that never yields an
     * address is the same problem as a lease that was lost. The back-off
     * count is kept - a rejoin is what got us here. */
    begin_episode(w, now_ms);
}

void hr_netwatch_on_disassoc(hr_netwatch_t *w, uint32_t now_ms)
{
    (void)now_ms;
    w->associated = false;
    w->have_ip = false;
    end_episode(w);
}

uint32_t hr_netwatch_reconnect_delay_ms(const hr_netwatch_t *w)
{
    uint32_t n = w->backoff_n < BACKOFF_SHIFT_CAP ? w->backoff_n
                                                  : BACKOFF_SHIFT_CAP;
    uint64_t d = (uint64_t)w->cfg.reconnect_ms << n;
    if (d > w->cfg.reconnect_max_ms) {
        d = w->cfg.reconnect_max_ms;
    }
    return (uint32_t)d;
}

hr_netwatch_action_t hr_netwatch_tick(hr_netwatch_t *w, bool have_ip,
                                      uint32_t now_ms)
{
    if (!w->associated) {
        return HR_NETWATCH_NONE;
    }
    if (have_ip) {
        w->have_ip = true;
        w->backoff_n = 0; /* the network answered; start clean next time */
        end_episode(w);
        return HR_NETWATCH_NONE;
    }
    w->have_ip = false;
    if (!w->in_episode) {
        begin_episode(w, now_ms);
    }
    uint32_t elapsed = now_ms - w->noip_since_ms;
    if (!w->reported && elapsed >= w->cfg.grace_ms) {
        w->reported = true;
        w->episodes++;
    }
    if (w->stage == 0) {
        if (elapsed >= w->cfg.dhcp_restart_ms) {
            w->stage = 1;
            w->dhcp_restarts++;
            w->last_action_ms = now_ms;
            return HR_NETWATCH_DHCP_RESTART;
        }
        return HR_NETWATCH_NONE;
    }
    uint32_t delay = hr_netwatch_reconnect_delay_ms(w);
    if (w->stage == 1) {
        if (elapsed >= delay) {
            w->stage = 2;
            w->reconnects++;
            w->backoff_n++;
            w->last_action_ms = now_ms;
            return HR_NETWATCH_RECONNECT;
        }
        return HR_NETWATCH_NONE;
    }
    /*
     * Stage 2: a reconnect was issued. Normally the driver reports the
     * disassociation and the caller resets us through on_disassoc(); if it
     * does not (the request was refused, or the rejoin happened without our
     * seeing the events) ask again after the same, growing, delay.
     */
    if (now_ms - w->last_action_ms >= delay) {
        w->reconnects++;
        w->backoff_n++;
        w->last_action_ms = now_ms;
        return HR_NETWATCH_RECONNECT;
    }
    return HR_NETWATCH_NONE;
}

bool hr_netwatch_no_ip(const hr_netwatch_t *w, uint32_t now_ms)
{
    return w->associated && !w->have_ip && w->in_episode &&
           (now_ms - w->noip_since_ms) >= w->cfg.grace_ms;
}

uint32_t hr_netwatch_noip_for_ms(const hr_netwatch_t *w, uint32_t now_ms)
{
    if (!w->associated || w->have_ip || !w->in_episode) {
        return 0;
    }
    return now_ms - w->noip_since_ms;
}
