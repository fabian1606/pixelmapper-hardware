/*
 * pm_net.c - Ethernet/IP/UDP. Phase 0 stub (no network yet).
 */
#include "pm_net.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "pm_net";
static uint8_t s_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

esp_err_t pm_net_init(const pm_board_t *board)
{
    (void)board;
    ESP_LOGW(TAG, "stub: Ethernet not initialised");
    return ESP_OK;
}

void pm_net_get_mac(uint8_t mac[6]) { memcpy(mac, s_mac, 6); }

esp_err_t pm_net_apply_ip(const pm_ip_settings_t *ip, uint16_t oem)
{
    (void)ip; (void)oem;
    return ESP_OK;
}

void pm_net_renew_dhcp(void) {}

void pm_net_get_status(pm_net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->mac, s_mac, 6);
}

esp_err_t pm_net_send_udp(uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    (void)ip; (void)port; (void)data; (void)len;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t pm_net_post(pm_net_work_fn fn, void *arg)
{
    (void)fn; (void)arg;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t pm_net_start(pm_net_rx_fn rx, pm_net_tick_fn tick, void *ctx, uint32_t period_ms)
{
    (void)rx; (void)tick; (void)ctx; (void)period_ms;
    return ESP_OK;
}
