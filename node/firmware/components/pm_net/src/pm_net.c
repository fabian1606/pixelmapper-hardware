/*
 * pm_net.c - Ethernet (internal EMAC + IP101 over RMII), IPv4 (DHCP / 5 s Art-Net fallback / static), UDP 6454
 * and the network task (core 0).
 *
 * Threading model:
 *   - The network task owns the IP state machine (pm_net_ip.c), the socket receive path and everything posted with
 *     pm_net_post(). ESP-IDF event handlers (ETH/IP events, default event loop task) only post work items.
 *   - lwIP internals (fallback address while the DHCP client keeps running, DHCP state) are touched only inside
 *     tcpip_api_call(), i.e. in the lwIP thread.
 *   - pm_net_get_status() / pm_net_get_mac() / pm_net_send_udp() / pm_net_post() are callable from any task.
 *
 * DHCP + fallback: ESP-IDF starts the DHCP client on link up. Without a lease after 5 s the Art-Net address
 * 2.B.C.D/8 is put on the lwIP netif directly (netif_set_addr) while the DHCP client keeps discovering (lwIP retries
 * forever, back-off up to 60 s). A late lease replaces the fallback address (dhcp_bind -> netif_set_addr).
 * The lease state is polled once per second from lwIP (dhcp_supplied_address): ESP-IDF raises GOT_IP for any
 * address change (also for our own fallback address), and its IP-lost timer is slow.
 */
#include "pm_net.h"

#include <errno.h>
#include <string.h>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/sockets.h"
#include "pm_net_ip.h"
#include "pm_pids.h"
#include "sdkconfig.h"

static const char *TAG = "pm_net";

#define NET_TASK_CORE 0
#define RX_BUF_LEN 1536
#define RX_BATCH 32
#define DHCP_POLL_MS 1000u

typedef struct {
    pm_net_work_fn fn;
    void *arg;
} work_item_t;

/* shared between tasks */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static pm_net_status_t s_status;          /* guarded by s_lock */
static pm_ip_settings_t s_pending_ip;     /* guarded by s_lock */
static uint16_t s_pending_oem;            /* guarded by s_lock */
static uint8_t s_mac[6];                  /* written once in pm_net_init */
static QueueHandle_t s_queue;
static volatile int s_sock = -1;
static TaskHandle_t s_task;

/* network task only (after init) */
static esp_netif_t *s_netif;
static esp_eth_handle_t s_eth;
static pm_net_ipsm_t s_sm;
static pm_net_rx_fn s_rx;
static pm_net_tick_fn s_tick;
static void *s_ctx;
static uint32_t s_period_ms = 10;
static pm_net_change_fn s_change_fn;
static void *s_change_ctx;
static uint8_t s_rx_buf[RX_BUF_LEN];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static inline uint32_t to_lwip(uint32_t host) { return lwip_htonl(host); }
static inline uint32_t from_lwip(uint32_t net) { return lwip_ntohl(net); }

/* ------------------------------------------------------------------------------------------------ lwIP calls */

typedef struct {
    struct tcpip_api_call_data call; /* must be first */
    struct netif *netif;
    uint32_t ip, mask, gw;           /* network order */
    bool supplied;
} lwip_call_t;

static err_t lwip_set_addr(struct tcpip_api_call_data *c)
{
    lwip_call_t *m = (lwip_call_t *)c;
    ip4_addr_t ip = {.addr = m->ip}, mask = {.addr = m->mask}, gw = {.addr = m->gw};
    netif_set_addr(m->netif, &ip, &mask, &gw);
    return ERR_OK;
}

static err_t lwip_read_dhcp(struct tcpip_api_call_data *c)
{
    lwip_call_t *m = (lwip_call_t *)c;
    m->supplied = dhcp_supplied_address(m->netif) != 0;
    m->ip = ip4_addr_get_u32(netif_ip4_addr(m->netif));
    m->mask = ip4_addr_get_u32(netif_ip4_netmask(m->netif));
    m->gw = ip4_addr_get_u32(netif_ip4_gw(m->netif));
    return ERR_OK;
}

static err_t lwip_renew(struct tcpip_api_call_data *c)
{
    lwip_call_t *m = (lwip_call_t *)c;
    if (dhcp_supplied_address(m->netif)) dhcp_renew(m->netif);
    return ERR_OK;
}

static struct netif *lwip_netif(void)
{
    return (s_netif != NULL && s_eth != NULL) ? (struct netif *)esp_netif_get_netif_impl(s_netif) : NULL;
}

/* ------------------------------------------------------------------------------------------------ state machine glue */

static void log_ip(const char *what, uint32_t a)
{
    ESP_LOGI(TAG, "  %s %u.%u.%u.%u", what, (unsigned)(a >> 24), (unsigned)((a >> 16) & 0xFF),
             (unsigned)((a >> 8) & 0xFF), (unsigned)(a & 0xFF));
}

static void publish_status(bool notify)
{
    pm_net_status_t st;
    portENTER_CRITICAL(&s_lock);
    s_status.link_up = s_sm.link_up;
    s_status.mode = s_sm.addr.mode;
    s_status.ip = s_sm.addr.ip;
    s_status.mask = s_sm.addr.mask;
    s_status.gateway = s_sm.addr.gateway;
    memcpy(s_status.mac, s_mac, 6);
    st = s_status;
    portEXIT_CRITICAL(&s_lock);
    if (!notify) return;
    static const char *const MODES[] = {"none", "dhcp", "fallback", "static"};
    ESP_LOGI(TAG, "link %s, address %s", st.link_up ? "up" : "down", MODES[st.mode & 3]);
    if (st.mode != PM_NET_ADDR_NONE) {
        log_ip("ip", st.ip);
        log_ip("mask", st.mask);
        log_ip("gw", st.gateway);
    }
    if (s_change_fn) s_change_fn(s_change_ctx, &st);
}

static void run_actions(unsigned act)
{
    if (act == 0) return;
    if (s_netif != NULL && s_eth != NULL) {
        if (act & PM_NET_ACT_DHCP_STOP) {
            esp_err_t e = esp_netif_dhcpc_stop(s_netif);
            if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
                ESP_LOGW(TAG, "dhcpc_stop: %s", esp_err_to_name(e));
            }
        }
        if (act & PM_NET_ACT_SET_STATIC) {
            esp_netif_ip_info_t info = {
                .ip.addr = to_lwip(s_sm.addr.ip),
                .netmask.addr = to_lwip(s_sm.addr.mask),
                .gw.addr = to_lwip(s_sm.addr.gateway),
            };
            esp_err_t e = esp_netif_set_ip_info(s_netif, &info);
            if (e != ESP_OK) ESP_LOGW(TAG, "set_ip_info: %s", esp_err_to_name(e));
        }
        if (act & PM_NET_ACT_DHCP_START) {
            esp_err_t e = esp_netif_dhcpc_start(s_netif);
            if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                ESP_LOGW(TAG, "dhcpc_start: %s", esp_err_to_name(e));
            }
        }
        if (act & PM_NET_ACT_SET_FALLBACK) {
            struct netif *nif = lwip_netif();
            if (nif != NULL) {
                lwip_call_t m = {.netif = nif, .ip = to_lwip(s_sm.addr.ip), .mask = to_lwip(s_sm.addr.mask),
                                 .gw = to_lwip(s_sm.addr.gateway)};
                tcpip_api_call(lwip_set_addr, &m.call);
            }
            ESP_LOGW(TAG, "no DHCP lease after %u ms: Art-Net default address, DHCP keeps trying",
                     (unsigned)PM_NET_DHCP_FALLBACK_MS);
        }
    }
    publish_status((act & PM_NET_ACT_NOTIFY) != 0);
}

/** Poll the lwIP DHCP state (network task). */
static void poll_dhcp(void)
{
    struct netif *nif = lwip_netif();
    if (nif == NULL || !s_sm.dhcp) return;
    lwip_call_t m = {.netif = nif};
    tcpip_api_call(lwip_read_dhcp, &m.call);
    if (m.supplied) {
        run_actions(pm_net_ipsm_dhcp_bound(&s_sm, from_lwip(m.ip), from_lwip(m.mask), from_lwip(m.gw)));
    } else {
        run_actions(pm_net_ipsm_dhcp_lost(&s_sm, now_ms()));
    }
}

/* ------------------------------------------------------------------------------------------------ work items */

static void work_apply_ip(void *arg)
{
    (void)arg;
    pm_ip_settings_t ip;
    uint16_t oem;
    portENTER_CRITICAL(&s_lock);
    ip = s_pending_ip;
    oem = s_pending_oem;
    portEXIT_CRITICAL(&s_lock);
    run_actions(pm_net_ipsm_configure(&s_sm, &ip, s_mac, oem, now_ms()));
}

static void work_link(void *arg) { run_actions(pm_net_ipsm_link(&s_sm, arg != NULL, now_ms())); }

static void work_ip_event(void *arg)
{
    (void)arg;
    poll_dhcp(); /* GOT_IP is also raised for our own fallback address: ask lwIP whether DHCP supplied it */
}

static void work_renew(void *arg)
{
    (void)arg;
    struct netif *nif = lwip_netif();
    if (nif == NULL || !s_sm.dhcp) return;
    lwip_call_t m = {.netif = nif};
    tcpip_api_call(lwip_renew, &m.call);
}

/* ------------------------------------------------------------------------------------------------ IDF events */

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        pm_net_post(work_link, (void *)1);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
    case ETHERNET_EVENT_STOP:
        pm_net_post(work_link, NULL);
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data; (void)id;
    pm_net_post(work_ip_event, NULL);
}

/* ------------------------------------------------------------------------------------------------ init */

static esp_err_t eth_init(const pm_board_t *board)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.flags |= ETH_MAC_FLAG_PIN_TO_CORE; /* rx task on the installing core (app_main: core 0) */
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = board->eth.mdc;
    emac.smi_gpio.mdio_num = board->eth.mdio;
    emac.interface = EMAC_DATA_INTERFACE_RMII;
    if (board->eth.rmii_clk_in == PM_BOARD_PIN_NONE) {
        ESP_LOGE(TAG, "board without RMII REF_CLK input is not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN; /* 50 MHz from the IP101 (25 MHz crystal) */
    emac.clock_config.rmii.clock_gpio = (emac_rmii_clock_gpio_t)board->eth.rmii_clk_in;
    emac.emac_dataif_gpio.rmii.tx_en_num = CONFIG_PM_NET_RMII_TX_EN;
    emac.emac_dataif_gpio.rmii.txd0_num = CONFIG_PM_NET_RMII_TXD0;
    emac.emac_dataif_gpio.rmii.txd1_num = CONFIG_PM_NET_RMII_TXD1;
    emac.emac_dataif_gpio.rmii.crs_dv_num = CONFIG_PM_NET_RMII_CRS_DV;
    emac.emac_dataif_gpio.rmii.rxd0_num = CONFIG_PM_NET_RMII_RXD0;
    emac.emac_dataif_gpio.rmii.rxd1_num = CONFIG_PM_NET_RMII_RXD1;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = board->eth.phy_addr;
    phy_cfg.reset_gpio_num = board->eth.phy_reset;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (mac == NULL || phy == NULL) {
        ESP_LOGE(TAG, "EMAC/PHY object creation failed");
        if (mac) mac->del(mac);
        if (phy) phy->del(phy);
        return ESP_FAIL;
    }

    esp_eth_config_t ecfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&ecfg, &s_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s (PHY not answering?)", esp_err_to_name(err));
        s_eth = NULL;
        mac->del(mac);
        phy->del(phy);
        return err;
    }
    uint8_t mac_addr[6];
    if (esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac_addr) == ESP_OK) memcpy(s_mac, mac_addr, 6);
    err = esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth));
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event, NULL);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_ip_event, NULL);
    return err;
}

esp_err_t pm_net_init(const pm_board_t *board)
{
    if (board == NULL) return ESP_ERR_INVALID_ARG;
    if (s_queue != NULL) return ESP_ERR_INVALID_STATE;
    s_queue = xQueueCreate(CONFIG_PM_NET_POST_QUEUE_LEN, sizeof(work_item_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    esp_read_mac(s_mac, ESP_MAC_ETH); /* valid (eFuse) even when the PHY is missing */

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&ncfg);
    if (s_netif == NULL) return ESP_ERR_NO_MEM;

    err = eth_init(board);
    pm_net_ipsm_init(&s_sm, s_mac, PM_ARTNET_OEM_NODE);
    publish_status(false);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "EMAC + IP101 ready, MAC " MACSTR, MAC2STR(s_mac));
    }
    /* On error the queue and the MAC are still valid: pm_net_start runs the task without a link (DMX keeps going) */
    return err;
}

void pm_net_get_mac(uint8_t mac[6]) { memcpy(mac, s_mac, 6); }

/* ------------------------------------------------------------------------------------------------ public API */

esp_err_t pm_net_apply_ip(const pm_ip_settings_t *ip, uint16_t oem)
{
    if (ip == NULL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    s_pending_ip = *ip;
    s_pending_oem = oem;
    portEXIT_CRITICAL(&s_lock);
    return pm_net_post(work_apply_ip, NULL); /* last settings win; several posts apply the same copy */
}

void pm_net_renew_dhcp(void) { pm_net_post(work_renew, NULL); }

void pm_net_get_status(pm_net_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t pm_net_send_udp(uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    int sock = s_sock;
    if (sock < 0) return ESP_ERR_INVALID_STATE;
    if (ip == 0xFFFFFFFFu || ip == 0) return ESP_ERR_INVALID_ARG; /* limited broadcast not used (Art-Net 4 p. 10) */
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = lwip_htons(port),
        .sin_addr.s_addr = to_lwip(ip),
    };
    int n = sendto(sock, data, len, 0, (struct sockaddr *)&to, sizeof(to));
    return n == (int)len ? ESP_OK : ESP_FAIL;
}

esp_err_t pm_net_post(pm_net_work_fn fn, void *arg)
{
    if (fn == NULL) return ESP_ERR_INVALID_ARG;
    if (s_queue == NULL) return ESP_ERR_INVALID_STATE;
    work_item_t w = {.fn = fn, .arg = arg};
    return xQueueSend(s_queue, &w, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

void pm_net_set_change_cb(pm_net_change_fn fn, void *ctx)
{
    s_change_fn = fn;
    s_change_ctx = ctx;
}

bool pm_net_in_task(void) { return s_task != NULL && xTaskGetCurrentTaskHandle() == s_task; }

/* ------------------------------------------------------------------------------------------------ task */

static int open_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)); /* send + receive directed broadcasts */
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = lwip_htons(PM_NET_ARTNET_PORT),
        .sin_addr.s_addr = lwip_htonl(INADDR_ANY), /* unicast + broadcast on any address */
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void drain_work(void)
{
    work_item_t w;
    while (xQueueReceive(s_queue, &w, 0) == pdTRUE) w.fn(w.arg);
}

static void net_task(void *arg)
{
    (void)arg;
    const int sock = s_sock;
    uint32_t next_tick = now_ms();
    uint32_t next_dhcp_poll = next_tick + DHCP_POLL_MS;
    for (;;) {
        drain_work();
        uint32_t now = now_ms();
        int32_t wait = (int32_t)(next_tick - now);
        if (wait < 0) wait = 0;
        if (wait > (int32_t)s_period_ms) wait = (int32_t)s_period_ms;

        /* posted work waits at most one tick period (select has no wake-up from the queue) */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = wait * 1000};
        int r = select(sock + 1, &rfds, NULL, NULL, &tv);
        bool got = false;
        if (r > 0 && FD_ISSET(sock, &rfds)) {
            for (int i = 0; i < RX_BATCH; i++) {
                struct sockaddr_in from;
                socklen_t flen = sizeof(from);
                int n = recvfrom(sock, s_rx_buf, sizeof(s_rx_buf), MSG_DONTWAIT, (struct sockaddr *)&from, &flen);
                if (n <= 0) break;
                got = true;
                if (s_rx) {
                    s_rx(s_ctx, from_lwip(from.sin_addr.s_addr), lwip_ntohs(from.sin_port), s_rx_buf, (size_t)n);
                }
            }
        }

        now = now_ms();
        run_actions(pm_net_ipsm_tick(&s_sm, now));
        if ((int32_t)(now - next_dhcp_poll) >= 0) {
            next_dhcp_poll = now + DHCP_POLL_MS;
            poll_dhcp();
        }
        bool due = (int32_t)(now - next_tick) >= 0;
        if (due) next_tick = now + s_period_ms;
        if ((got || due) && s_tick) s_tick(s_ctx);
    }
}

esp_err_t pm_net_start(pm_net_rx_fn rx, pm_net_tick_fn tick, void *ctx, uint32_t period_ms)
{
    if (s_queue == NULL || s_task != NULL) return ESP_ERR_INVALID_STATE;
    s_rx = rx;
    s_tick = tick;
    s_ctx = ctx;
    s_period_ms = period_ms ? period_ms : 10;
    int sock = open_socket();
    if (sock < 0) {
        ESP_LOGE(TAG, "UDP socket %u: errno %d", (unsigned)PM_NET_ARTNET_PORT, errno);
        return ESP_FAIL;
    }
    s_sock = sock;
    if (xTaskCreatePinnedToCore(net_task, "pm_net", CONFIG_PM_NET_TASK_STACK, NULL, CONFIG_PM_NET_TASK_PRIO,
                                &s_task, NET_TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (s_eth != NULL) {
        esp_err_t err = esp_eth_start(s_eth);
        if (err != ESP_OK) ESP_LOGE(TAG, "esp_eth_start: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "no Ethernet driver: network task runs without link");
    }
    return ESP_OK;
}
