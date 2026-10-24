/*
 * pm_net.h - Ethernet (internal EMAC + IP101 over RMII), IPv4 (DHCP with 5 s fallback to the Art-Net address
 * 2.B.C.D/8, B = MAC[3] + OemHi + OemLo), UDP socket 6454 and the network task that drives pm_artnet.
 *
 * The network task (core 0) is the single thread of pm_artnet and of the RDM responder: it receives UDP, calls
 * pm_artnet_handle_packet(), pm_artnet_tick() every ~10 ms, and drains pm_port events and pm_config change
 * notifications (posted via pm_net_post()). It also implements the pm_artnet_ops_t UDP/net functions.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pm_board.h"
#include "pm_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_NET_DHCP_FALLBACK_MS 5000u

typedef enum {
    PM_NET_ADDR_NONE = 0,     /**< no link / no address yet */
    PM_NET_ADDR_DHCP,
    PM_NET_ADDR_FALLBACK,     /**< DHCP enabled, no lease within 5 s: Art-Net default 2.B.C.D/8 */
    PM_NET_ADDR_STATIC,
} pm_net_addr_mode_t;

typedef struct {
    bool link_up;
    pm_net_addr_mode_t mode;
    uint32_t ip, mask, gateway;   /**< host order */
    uint8_t mac[6];
} pm_net_status_t;

/** Bring up the EMAC/PHY from the board pins and read the MAC. Does not configure IP yet. */
esp_err_t pm_net_init(const pm_board_t *board);
/** MAC of the Ethernet interface (valid after pm_net_init) - also the source of the node UID. */
void pm_net_get_mac(uint8_t mac[6]);

/** Apply IP settings (DHCP/static). Called at start and on PM_CHG_IP. Non-blocking. */
esp_err_t pm_net_apply_ip(const pm_ip_settings_t *ip, uint16_t oem);
/** E1.37-2 INTERFACE_RENEW_DHCP. */
void pm_net_renew_dhcp(void);
void pm_net_get_status(pm_net_status_t *out);

/** Send a UDP datagram from port 6454 (any task, thread-safe). */
esp_err_t pm_net_send_udp(uint32_t ip, uint16_t port, const uint8_t *data, size_t len);

/** Work item executed in the network task (e.g. config change, port event forwarding). */
typedef void (*pm_net_work_fn)(void *arg);
/** Run `fn(arg)` in the network task. Thread-safe, never blocks (ESP_ERR_NO_MEM if the queue is full). */
esp_err_t pm_net_post(pm_net_work_fn fn, void *arg);

/** Called in the network task for every received datagram on 6454. */
typedef void (*pm_net_rx_fn)(void *ctx, uint32_t src_ip, uint16_t src_port, const uint8_t *data, size_t len);
/** Called in the network task every `period_ms` and after every received datagram batch. */
typedef void (*pm_net_tick_fn)(void *ctx);

/** Open the socket and start the network task (core 0). */
esp_err_t pm_net_start(pm_net_rx_fn rx, pm_net_tick_fn tick, void *ctx, uint32_t period_ms);

#ifdef __cplusplus
}
#endif
