/*
 * pm_net_types.h - plain types of pm_net (pure C, no ESP-IDF), shared with the host-testable address logic.
 */
#pragma once

#define PM_NET_DHCP_FALLBACK_MS 5000u
#define PM_NET_ARTNET_PORT 6454u /**< UDP port, source and destination (Art-Net 0x1936) */

typedef enum {
    PM_NET_ADDR_NONE = 0,     /**< no link / no address yet */
    PM_NET_ADDR_DHCP,
    PM_NET_ADDR_FALLBACK,     /**< DHCP enabled, no lease within 5 s: Art-Net default 2.B.C.D/8 */
    PM_NET_ADDR_STATIC,
} pm_net_addr_mode_t;
