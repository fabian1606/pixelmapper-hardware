/*
 * pm_net_ip.h - IPv4 address logic of pm_net (pure C11, host-testable, no ESP-IDF).
 *
 * - Art-Net 4 default address 2.B.C.D/8 with B = MAC[3] + OemHi + OemLo (mod 256), C = MAC[4], D = MAC[5]
 *   (artnet4-digest.md "IP-Adressen", contract §2).
 * - State machine "DHCP with 5 s fallback": the DHCP client keeps running while the fallback address is active, so
 *   a DHCP server that appears later still wins (the lease replaces the fallback address).
 * - Static configuration (static_ip 0 = Art-Net default address, mask 0 = 255.0.0.0).
 *
 * The platform (pm_net.c) feeds events and executes the returned actions; all calls from the network task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pm_net_types.h"
#include "pm_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_NET_DEFAULT_MASK 0xFF000000u

/** Art-Net default address 2.B.C.D (host order). */
uint32_t pm_net_fallback_ip(const uint8_t mac[6], uint16_t oem);

/** One complete address. */
typedef struct {
    pm_net_addr_mode_t mode;
    uint32_t ip, mask, gateway; /**< host order */
} pm_net_addr_t;

/** Address used when DHCP is off (static_ip 0 -> Art-Net default, mask 0 -> 255.0.0.0). */
void pm_net_static_addr(const pm_ip_settings_t *s, const uint8_t mac[6], uint16_t oem, pm_net_addr_t *out);

/** Actions returned by the state machine (bit mask), executed by the platform in this order:
 *  DHCP_STOP, SET_STATIC, DHCP_START, SET_FALLBACK, NOTIFY. */
enum {
    PM_NET_ACT_DHCP_STOP = 1u << 0,    /**< stop the DHCP client */
    PM_NET_ACT_SET_STATIC = 1u << 1,   /**< configure sm->addr as static address (DHCP stopped) */
    PM_NET_ACT_DHCP_START = 1u << 2,   /**< (re)start the DHCP client, address 0 until a lease arrives */
    PM_NET_ACT_SET_FALLBACK = 1u << 3, /**< set sm->addr on the interface while the DHCP client keeps running */
    PM_NET_ACT_NOTIFY = 1u << 4,       /**< link or address changed (unsolicited ArtPollReply, UI) */
};

typedef struct {
    bool configured;
    bool link_up;
    bool dhcp;              /**< DHCP enabled in the settings */
    bool timer_armed;       /**< waiting for a lease since t0 */
    uint32_t t0;
    uint32_t fallback_ip;   /**< 2.B.C.D for this MAC/OEM */
    pm_net_addr_t addr;     /**< current address (mode NONE = no address) */
} pm_net_ipsm_t;

void pm_net_ipsm_init(pm_net_ipsm_t *sm, const uint8_t mac[6], uint16_t oem);
/** New IP settings (start, ArtIpProg, RDM, UI). */
unsigned pm_net_ipsm_configure(pm_net_ipsm_t *sm, const pm_ip_settings_t *s, const uint8_t mac[6], uint16_t oem,
                               uint32_t now_ms);
/** Ethernet link up/down. */
unsigned pm_net_ipsm_link(pm_net_ipsm_t *sm, bool up, uint32_t now_ms);
/** The DHCP client got (or renewed) a lease. Ignored when DHCP is disabled. */
unsigned pm_net_ipsm_dhcp_bound(pm_net_ipsm_t *sm, uint32_t ip, uint32_t mask, uint32_t gateway);
/** The DHCP lease was lost (lease expired, no renewal): back to "waiting", fallback after 5 s. */
unsigned pm_net_ipsm_dhcp_lost(pm_net_ipsm_t *sm, uint32_t now_ms);
/** Periodic: switches to the fallback address PM_NET_DHCP_FALLBACK_MS after waiting started. */
unsigned pm_net_ipsm_tick(pm_net_ipsm_t *sm, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
