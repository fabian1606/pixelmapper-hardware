/* pm_net_ip.c - IPv4 address logic of pm_net (pure C11), see pm_net_ip.h. */
#include "pm_net_ip.h"

#include <string.h>

uint32_t pm_net_fallback_ip(const uint8_t mac[6], uint16_t oem)
{
    /* Art-Net 4 p. 13: B = MAC[3] + OemHi + OemLo; the spec does not say "mod 256", a byte can only hold that. */
    uint8_t b = (uint8_t)(mac[3] + (oem >> 8) + (oem & 0xFFu));
    return (2u << 24) | ((uint32_t)b << 16) | ((uint32_t)mac[4] << 8) | mac[5];
}

void pm_net_static_addr(const pm_ip_settings_t *s, const uint8_t mac[6], uint16_t oem, pm_net_addr_t *out)
{
    out->mode = PM_NET_ADDR_STATIC;
    if (s->static_ip == 0) {
        out->ip = pm_net_fallback_ip(mac, oem);
        out->mask = s->static_mask ? s->static_mask : PM_NET_DEFAULT_MASK;
    } else {
        out->ip = s->static_ip;
        out->mask = s->static_mask ? s->static_mask : PM_NET_DEFAULT_MASK;
    }
    out->gateway = s->gateway;
}

void pm_net_ipsm_init(pm_net_ipsm_t *sm, const uint8_t mac[6], uint16_t oem)
{
    memset(sm, 0, sizeof(*sm));
    sm->fallback_ip = pm_net_fallback_ip(mac, oem);
}

static void wait_for_lease(pm_net_ipsm_t *sm, uint32_t now_ms)
{
    sm->timer_armed = sm->link_up;
    sm->t0 = now_ms;
}

unsigned pm_net_ipsm_configure(pm_net_ipsm_t *sm, const pm_ip_settings_t *s, const uint8_t mac[6], uint16_t oem,
                               uint32_t now_ms)
{
    sm->fallback_ip = pm_net_fallback_ip(mac, oem);
    bool first = !sm->configured;
    sm->configured = true;
    if (s->dhcp) {
        if (!first && sm->dhcp) {
            return 0; /* already in DHCP mode: keep lease / fallback, the client is running */
        }
        sm->dhcp = true;
        memset(&sm->addr, 0, sizeof(sm->addr));
        wait_for_lease(sm, now_ms);
        return PM_NET_ACT_DHCP_START | PM_NET_ACT_NOTIFY;
    }
    pm_net_addr_t a;
    pm_net_static_addr(s, mac, oem, &a);
    bool same = !first && !sm->dhcp && memcmp(&a, &sm->addr, sizeof(a)) == 0;
    sm->dhcp = false;
    sm->timer_armed = false;
    sm->addr = a;
    return same ? 0u : (PM_NET_ACT_DHCP_STOP | PM_NET_ACT_SET_STATIC | PM_NET_ACT_NOTIFY);
}

unsigned pm_net_ipsm_link(pm_net_ipsm_t *sm, bool up, uint32_t now_ms)
{
    if (sm->link_up == up) return 0;
    sm->link_up = up;
    if (!up) {
        /* ESP-IDF takes the interface down and stops the DHCP client (address 0); it restarts it on link up. */
        sm->timer_armed = false;
        if (sm->dhcp) memset(&sm->addr, 0, sizeof(sm->addr));
        return PM_NET_ACT_NOTIFY;
    }
    if (sm->dhcp) {
        wait_for_lease(sm, now_ms);
        return PM_NET_ACT_NOTIFY;
    }
    return sm->configured ? (PM_NET_ACT_SET_STATIC | PM_NET_ACT_NOTIFY) : PM_NET_ACT_NOTIFY;
}

unsigned pm_net_ipsm_dhcp_bound(pm_net_ipsm_t *sm, uint32_t ip, uint32_t mask, uint32_t gateway)
{
    if (!sm->dhcp || ip == 0) return 0;
    pm_net_addr_t a = {.mode = PM_NET_ADDR_DHCP, .ip = ip, .mask = mask, .gateway = gateway};
    sm->timer_armed = false;
    if (memcmp(&a, &sm->addr, sizeof(a)) == 0) return 0;
    sm->addr = a;
    return PM_NET_ACT_NOTIFY;
}

unsigned pm_net_ipsm_dhcp_lost(pm_net_ipsm_t *sm, uint32_t now_ms)
{
    if (!sm->dhcp || sm->addr.mode != PM_NET_ADDR_DHCP) return 0;
    memset(&sm->addr, 0, sizeof(sm->addr));
    wait_for_lease(sm, now_ms);
    return PM_NET_ACT_NOTIFY;
}

unsigned pm_net_ipsm_tick(pm_net_ipsm_t *sm, uint32_t now_ms)
{
    if (!sm->timer_armed || !sm->dhcp || !sm->link_up) return 0;
    if ((uint32_t)(now_ms - sm->t0) < PM_NET_DHCP_FALLBACK_MS) return 0;
    sm->timer_armed = false;
    sm->addr.mode = PM_NET_ADDR_FALLBACK;
    sm->addr.ip = sm->fallback_ip;
    sm->addr.mask = PM_NET_DEFAULT_MASK;
    sm->addr.gateway = 0;
    return PM_NET_ACT_SET_FALLBACK | PM_NET_ACT_NOTIFY;
}
